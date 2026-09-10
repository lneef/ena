/*
 * QEMU Amazon Elastic Network Adapter (ENA) emulation
 *
 * PCI device, register file, reset, readless MMIO, AENQ and completion
 * queue plumbing. Admin commands live in ena_admin.c, the datapath in
 * ena_tx.c and ena_rx.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "hw/pci/pci.h"
#include "hw/pci/msix.h"
#include "hw/core/qdev-properties.h"
#include "net/eth.h"
#include "hw/net/net_tx_pkt.h"
#include "hw/net/net_rx_pkt.h"
#include "hw/ena.h"

#define REG(s, off) ((s)->reg[(off) / 4])

uint64_t ena_mem_addr(const struct ena_common_mem_addr *addr)
{
    return ((uint64_t)le16_to_cpu(addr->mem_addr_high) << 32) |
           le32_to_cpu(addr->mem_addr_low);
}

void ena_dma_read(EnaState *s, uint64_t addr, void *buf, size_t len)
{
    pci_dma_read(PCI_DEVICE(s), addr, buf, len);
}

void ena_dma_write(EnaState *s, uint64_t addr, const void *buf, size_t len)
{
    pci_dma_write(PCI_DEVICE(s), addr, buf, len);
}

uint8_t *ena_llq_mem(EnaState *s, const EnaSq *sq)
{
    uint8_t *base = memory_region_get_ram_ptr(&s->llq_mr);

    assert(sq->llq);
    return base + (sq - s->sq) * ENA_LLQ_QUEUE_BYTES;
}

void ena_stats_reset(EnaState *s)
{
    s->tx_pkts = 0;
    s->tx_bytes = 0;
    s->rx_pkts = 0;
    s->rx_bytes = 0;
    s->rx_drops = 0;
}

/* Admin completions and AENQ events share MSI-X vector 0. */
static void ena_admin_intr(EnaState *s)
{
    if (REG(s, ENA_REGS_INTR_MASK_OFF) & 1) {
        return;
    }
    msix_notify(PCI_DEVICE(s), ENA_ADMIN_MSIX_VECTOR);
}

void ena_cq_push(EnaState *s, EnaCq *cq, const void *cdesc)
{
    uint64_t addr = cq->base +
                    (uint64_t)(cq->tail & (cq->depth - 1)) * cq->entry_size;

    assert(cq->used);
    ena_dma_write(s, addr, cdesc, cq->entry_size);
    cq->tail++;
    if ((cq->tail & (cq->depth - 1)) == 0) {
        cq->phase = !cq->phase;
    }
}

static void ena_irq_fire(EnaIrq *irq)
{
    timer_del(irq->moder_timer);
    irq->unmasked = false;
    irq->rx_pending = false;
    irq->tx_pending = false;
    msix_notify(PCI_DEVICE(irq->s), irq->vector);
}

static void ena_irq_moder_timer(void *opaque)
{
    ena_irq_fire(opaque);
}

static void ena_irq_schedule(EnaIrq *irq, uint32_t delay_us)
{
    if (delay_us == 0) {
        ena_irq_fire(irq);
        return;
    }
    timer_mod_anticipate(irq->moder_timer,
                         qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + delay_us);
}

void ena_cq_intr(EnaState *s, EnaCq *cq, bool is_tx)
{
    EnaIrq *irq;

    if (!cq->intr_enabled || cq->msix_vector >= ENA_MSIX_VECTORS) {
        return;
    }
    irq = &s->irq[cq->msix_vector];
    if (!irq->unmasked) {
        if (is_tx) {
            irq->tx_pending = true;
        } else {
            irq->rx_pending = true;
        }
        return;
    }
    ena_irq_schedule(irq, is_tx ? irq->tx_delay_us : irq->rx_delay_us);
}

/*
 * The unmask register of any CQ re-arms the vector the CQ is bound to.
 * FreeBSD and Linux write only the TX CQ register of an RX/TX pair.
 */
static void ena_cq_unmask_write(EnaState *s, EnaCq *cq, uint32_t val)
{
    EnaIrq *irq;

    if (!cq->used || cq->msix_vector >= ENA_MSIX_VECTORS) {
        return;
    }
    irq = &s->irq[cq->msix_vector];
    if (!(val & ENA_ETH_IO_INTR_REG_NO_MODERATION_UPDATE_MASK)) {
        irq->rx_delay_us = val & ENA_ETH_IO_INTR_REG_RX_INTR_DELAY_MASK;
        irq->tx_delay_us = (val & ENA_ETH_IO_INTR_REG_TX_INTR_DELAY_MASK) >>
                           ENA_ETH_IO_INTR_REG_TX_INTR_DELAY_SHIFT;
    }
    irq->unmasked = !!(val & ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK);
    if (!irq->unmasked) {
        timer_del(irq->moder_timer);
        return;
    }
    if (irq->rx_pending) {
        ena_irq_schedule(irq, irq->rx_delay_us);
    }
    if (irq->tx_pending) {
        ena_irq_schedule(irq, irq->tx_delay_us);
    }
}

void ena_aenq_post(EnaState *s, uint16_t group, uint16_t syndrome,
                   const void *data, size_t len)
{
    uint64_t base = ((uint64_t)REG(s, ENA_REGS_AENQ_BASE_HI_OFF) << 32) |
                    REG(s, ENA_REGS_AENQ_BASE_LO_OFF);
    uint16_t depth = REG(s, ENA_REGS_AENQ_CAPS_OFF) &
                     ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK;
    uint16_t head_db = REG(s, ENA_REGS_AENQ_HEAD_DB_OFF);
    struct ena_admin_aenq_entry e = {};

    assert(len <= sizeof(e.inline_data_w4));
    if (!base || !depth || !(s->aenq_groups & BIT(group))) {
        return;
    }
    if ((uint16_t)(head_db - s->aenq_tail) == 0) {
        return;
    }

    e.aenq_common_desc.group = cpu_to_le16(group);
    e.aenq_common_desc.syndrome = cpu_to_le16(syndrome);
    e.aenq_common_desc.flags = s->aenq_phase;
    e.aenq_common_desc.timestamp_low =
        cpu_to_le32(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
    e.aenq_common_desc.timestamp_high =
        cpu_to_le32(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) >> 32);
    memcpy(e.inline_data_w4, data, len);

    ena_dma_write(s, base + (uint64_t)(s->aenq_tail % depth) * sizeof(e),
                  &e, sizeof(e));
    s->aenq_tail++;
    if (s->aenq_tail % depth == 0) {
        s->aenq_phase = !s->aenq_phase;
    }
    REG(s, ENA_REGS_AENQ_TAIL_OFF) = s->aenq_tail;
    ena_admin_intr(s);
}

static void ena_post_link_change(EnaState *s)
{
    bool up = !qemu_get_queue(s->nic)->link_down;
    uint32_t flags = cpu_to_le32(up ? ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK : 0);

    ena_aenq_post(s, ENA_ADMIN_LINK_CHANGE, 0, &flags, sizeof(flags));
}

static void ena_keep_alive(void *opaque)
{
    EnaState *s = opaque;
    struct {
        uint32_t rx_drops_low, rx_drops_high;
        uint32_t tx_drops_low, tx_drops_high;
        uint32_t rx_overruns_low, rx_overruns_high;
    } d = {
        .rx_drops_low = cpu_to_le32(s->rx_drops),
        .rx_drops_high = cpu_to_le32(s->rx_drops >> 32),
    };

    ena_aenq_post(s, ENA_ADMIN_KEEP_ALIVE, 0, &d, sizeof(d));
    timer_mod(s->keep_alive_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                                   ENA_KEEP_ALIVE_INTERVAL_MS);
}

void ena_aenq_config(EnaState *s, uint32_t groups)
{
    s->aenq_groups = groups;
    if (groups & BIT(ENA_ADMIN_LINK_CHANGE)) {
        ena_post_link_change(s);
    }
    if (groups & BIT(ENA_ADMIN_KEEP_ALIVE)) {
        timer_mod(s->keep_alive_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                                       ENA_KEEP_ALIVE_INTERVAL_MS);
    } else {
        timer_del(s->keep_alive_timer);
    }
}

static void ena_rss_reset(EnaRss *rss)
{
    /* ENA default key: Microsoft's RSS key with its 4-byte words reversed */
    static const uint32_t default_key[ENA_ADMIN_RSS_KEY_PARTS] = {
        0x6d5a56da, 0x255b0ec2, 0x4167253d, 0x43a38fb0, 0xd0ca2bcb,
        0xae7b30b4, 0x77cb2da3, 0x8030f20c, 0x6a42b73b, 0xbeac01fa,
    };
    int i;

    memset(rss, 0, sizeof(*rss));
    memcpy(rss->key, default_key, sizeof(rss->key));
    rss->key_parts = ENA_ADMIN_RSS_KEY_PARTS;
    rss->func = ENA_ADMIN_TOEPLITZ;
    for (i = 0; i < ENA_ADMIN_RSS_PROTO_NUM; i++) {
        switch (i) {
        case ENA_ADMIN_RSS_TCP4:
        case ENA_ADMIN_RSS_UDP4:
        case ENA_ADMIN_RSS_TCP6:
        case ENA_ADMIN_RSS_UDP6:
        case ENA_ADMIN_RSS_TCP6_EX:
            rss->fields[i] = ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA |
                             ENA_ADMIN_RSS_L4_DP | ENA_ADMIN_RSS_L4_SP;
            break;
        case ENA_ADMIN_RSS_IP4:
        case ENA_ADMIN_RSS_IP6:
        case ENA_ADMIN_RSS_IP4_FRAG:
        case ENA_ADMIN_RSS_IP6_EX:
            rss->fields[i] = ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA;
            break;
        case ENA_ADMIN_RSS_NOT_IP:
            rss->fields[i] = ENA_ADMIN_RSS_L2_DA | ENA_ADMIN_RSS_L2_SA;
            break;
        }
    }
}

static void ena_dev_reset(EnaState *s)
{
    int i;

    memset(s->reg, 0, sizeof(s->reg));
    s->aq_head = 0;
    s->acq_tail = 0;
    s->acq_phase = true;
    s->aenq_tail = 0;
    s->aenq_phase = true;
    s->aenq_groups = 0;
    timer_del(s->keep_alive_timer);

    s->mtu = ENA_MAX_MTU;
    s->host_info_addr = 0;
    s->debug_area_addr = 0;
    s->debug_area_size = 0;
    s->llq_enabled = false;
    s->llq_entry_size = 0;
    ena_rss_reset(&s->rss);

    memset(s->sq, 0, sizeof(s->sq));
    memset(s->cq, 0, sizeof(s->cq));
    for (i = 0; i < ENA_MSIX_VECTORS; i++) {
        EnaIrq *irq = &s->irq[i];

        timer_del(irq->moder_timer);
        memset(irq, 0, offsetof(EnaIrq, moder_timer));
    }
    ena_stats_reset(s);

    REG(s, ENA_REGS_VERSION_OFF) =
        (ENA_VERSION_MAJOR << ENA_REGS_VERSION_MAJOR_VERSION_SHIFT) |
        ENA_VERSION_MINOR;
    REG(s, ENA_REGS_CONTROLLER_VERSION_OFF) =
        (ENA_CTRL_VERSION_IMPL_ID << ENA_REGS_CONTROLLER_VERSION_IMPL_ID_SHIFT) |
        (ENA_CTRL_VERSION_MAJOR << ENA_REGS_CONTROLLER_VERSION_MAJOR_VERSION_SHIFT) |
        (ENA_CTRL_VERSION_MINOR << ENA_REGS_CONTROLLER_VERSION_MINOR_VERSION_SHIFT) |
        ENA_CTRL_VERSION_SUBMINOR;
    REG(s, ENA_REGS_CAPS_OFF) =
        (ENA_CAPS_RESET_TIMEOUT << ENA_REGS_CAPS_RESET_TIMEOUT_SHIFT) |
        (ENA_DMA_ADDR_WIDTH << ENA_REGS_CAPS_DMA_ADDR_WIDTH_SHIFT) |
        (ENA_CAPS_ADMIN_CMD_TO << ENA_REGS_CAPS_ADMIN_CMD_TO_SHIFT);
    REG(s, ENA_REGS_DEV_STS_OFF) = ENA_REGS_DEV_STS_READY_MASK;
}

static uint32_t ena_reg_file_read(EnaState *s, uint32_t off)
{
    if (off < ENA_REG_FILE_SIZE) {
        return REG(s, off);
    }
    return 0;
}

static void ena_mmio_readless(EnaState *s, uint32_t req)
{
    uint64_t resp_addr = ((uint64_t)REG(s, ENA_REGS_MMIO_RESP_HI_OFF) << 32) |
                         REG(s, ENA_REGS_MMIO_RESP_LO_OFF);
    uint16_t req_id = req & ENA_REGS_MMIO_REG_READ_REQ_ID_MASK;
    uint16_t off = (req & ENA_REGS_MMIO_REG_READ_REG_OFF_MASK) >>
                   ENA_REGS_MMIO_REG_READ_REG_OFF_SHIFT;
    struct ena_admin_ena_mmio_req_read_less_resp resp = {
        .req_id = cpu_to_le16(req_id),
        .reg_off = cpu_to_le16(off),
        .reg_val = cpu_to_le32(ena_reg_file_read(s, off & ~3)),
    };

    if (!resp_addr) {
        return;
    }
    ena_dma_write(s, resp_addr, &resp, sizeof(resp));
}

static void ena_dev_ctl_write(EnaState *s, uint32_t val)
{
    if (val & ENA_REGS_DEV_CTL_DEV_RESET_MASK) {
        ena_dev_reset(s);
        REG(s, ENA_REGS_DEV_CTL_OFF) = val;
        REG(s, ENA_REGS_DEV_STS_OFF) = ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK;
        return;
    }
    REG(s, ENA_REGS_DEV_CTL_OFF) = val;
    if (REG(s, ENA_REGS_DEV_STS_OFF) & ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK) {
        REG(s, ENA_REGS_DEV_STS_OFF) = ENA_REGS_DEV_STS_READY_MASK;
    }
}

static uint64_t ena_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    return ena_reg_file_read(opaque, addr);
}

static void ena_reg_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned size)
{
    EnaState *s = opaque;
    uint32_t val = val64;

    if (addr >= ENA_REG_SQ_DB_BASE &&
        addr < ENA_REG_SQ_DB_BASE + ENA_MAX_SQ * 4) {
        EnaSq *sq = &s->sq[(addr - ENA_REG_SQ_DB_BASE) / 4];

        if (!sq->used) {
            return;
        }
        sq->tail = val;
        if (sq->is_tx) {
            ena_tx_doorbell(s, sq);
        } else {
            ena_rx_doorbell(s, sq);
        }
        return;
    }
    if (addr >= ENA_REG_CQ_UNMASK_BASE &&
        addr < ENA_REG_CQ_UNMASK_BASE + ENA_MAX_CQ * 4) {
        ena_cq_unmask_write(s, &s->cq[(addr - ENA_REG_CQ_UNMASK_BASE) / 4], val);
        return;
    }
    if (addr >= ENA_REG_FILE_SIZE) {
        return;
    }

    switch (addr) {
    case ENA_REGS_VERSION_OFF:
    case ENA_REGS_CONTROLLER_VERSION_OFF:
    case ENA_REGS_CAPS_OFF:
    case ENA_REGS_CAPS_EXT_OFF:
    case ENA_REGS_ACQ_TAIL_OFF:
    case ENA_REGS_AENQ_TAIL_OFF:
    case ENA_REGS_DEV_STS_OFF:
        break;
    case ENA_REGS_DEV_CTL_OFF:
        ena_dev_ctl_write(s, val);
        break;
    case ENA_REGS_MMIO_REG_READ_OFF:
        REG(s, addr) = val;
        ena_mmio_readless(s, val);
        break;
    case ENA_REGS_AQ_DB_OFF:
        REG(s, addr) = val;
        ena_admin_process(s);
        break;
    case ENA_REGS_AQ_CAPS_OFF:
        REG(s, addr) = val;
        s->aq_head = 0;
        break;
    case ENA_REGS_ACQ_CAPS_OFF:
        REG(s, addr) = val;
        s->acq_tail = 0;
        s->acq_phase = true;
        REG(s, ENA_REGS_ACQ_TAIL_OFF) = 0;
        break;
    case ENA_REGS_AENQ_CAPS_OFF:
        REG(s, addr) = val;
        s->aenq_tail = 0;
        s->aenq_phase = true;
        REG(s, ENA_REGS_AENQ_TAIL_OFF) = 0;
        break;
    default:
        REG(s, addr) = val;
        break;
    }
}

static const MemoryRegionOps ena_reg_ops = {
    .read = ena_reg_read,
    .write = ena_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static bool ena_nc_can_receive(NetClientState *nc)
{
    return ena_rx_can_receive(qemu_get_nic_opaque(nc));
}

static ssize_t ena_nc_receive_iov(NetClientState *nc, const struct iovec *iov,
                                  int iovcnt)
{
    return ena_rx_receive_iov(qemu_get_nic_opaque(nc), iov, iovcnt);
}

static ssize_t ena_nc_receive(NetClientState *nc, const uint8_t *buf,
                              size_t size)
{
    const struct iovec iov = { .iov_base = (void *)buf, .iov_len = size };

    return ena_rx_receive_iov(qemu_get_nic_opaque(nc), &iov, 1);
}

static void ena_nc_link_status_changed(NetClientState *nc)
{
    ena_post_link_change(qemu_get_nic_opaque(nc));
}

static NetClientInfo net_ena_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = ena_nc_can_receive,
    .receive = ena_nc_receive,
    .receive_iov = ena_nc_receive_iov,
    .link_status_changed = ena_nc_link_status_changed,
};

static void ena_realize(PCIDevice *pci_dev, Error **errp)
{
    EnaState *s = ENA(pci_dev);
    DeviceState *dev = DEVICE(pci_dev);
    int i;

    memory_region_init_io(&s->regs_mr, OBJECT(s), &ena_reg_ops, s,
                          "ena-regs", ENA_REG_BAR_SIZE);
    pci_register_bar(pci_dev, ENA_REG_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->regs_mr);

    memory_region_init_ram(&s->llq_mr, OBJECT(s), "ena-llq",
                           ENA_MEM_BAR_SIZE, &error_fatal);
    pci_register_bar(pci_dev, ENA_MEM_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->llq_mr);

    if (msix_init_exclusive_bar(pci_dev, ENA_MSIX_VECTORS, ENA_MSIX_BAR,
                                errp)) {
        return;
    }
    for (i = 0; i < ENA_MSIX_VECTORS; i++) {
        msix_vector_use(pci_dev, i);
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_ena_info, &s->conf,
                          object_get_typename(OBJECT(s)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    net_tx_pkt_init(&s->tx_pkt, ENA_MAX_PKT_DESCS);
    net_rx_pkt_init(&s->rx_pkt);

    s->keep_alive_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ena_keep_alive, s);
    for (i = 0; i < ENA_MSIX_VECTORS; i++) {
        s->irq[i].s = s;
        s->irq[i].vector = i;
        s->irq[i].moder_timer = timer_new_us(QEMU_CLOCK_VIRTUAL,
                                             ena_irq_moder_timer, &s->irq[i]);
    }
}

static void ena_exit(PCIDevice *pci_dev)
{
    EnaState *s = ENA(pci_dev);
    int i;

    for (i = 0; i < ENA_MSIX_VECTORS; i++) {
        timer_free(s->irq[i].moder_timer);
    }
    timer_free(s->keep_alive_timer);
    net_tx_pkt_uninit(s->tx_pkt);
    net_rx_pkt_uninit(s->rx_pkt);
    qemu_del_nic(s->nic);
    msix_uninit_exclusive_bar(pci_dev);
}

static void ena_qdev_reset_hold(Object *obj, ResetType type)
{
    ena_dev_reset(ENA(obj));
}

static const Property ena_properties[] = {
    DEFINE_NIC_PROPERTIES(EnaState, conf),
    /* recommend 256-byte LLQ entries to the driver instead of 128-byte ones */
    DEFINE_PROP_BOOL("llq-large-header", EnaState, llq_large, false),
};

static void ena_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ena_realize;
    k->exit = ena_exit;
    k->vendor_id = ENA_PCI_VENDOR_ID;
    k->device_id = ENA_PCI_DEVICE_ID;
    k->revision = 0;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    rc->phases.hold = ena_qdev_reset_hold;
    dc->desc = "Amazon Elastic Network Adapter";
    device_class_set_props(dc, ena_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo ena_info = {
    .name = TYPE_ENA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EnaState),
    .class_init = ena_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ena_register_types(void)
{
    type_register_static(&ena_info);
}

type_init(ena_register_types)
