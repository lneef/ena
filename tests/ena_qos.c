/*
 * libqos driver and helpers for the ENA qtests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "libqos/libqos-malloc.h"
#include "standard-headers/linux/pci_regs.h"
#include "tests/ena_qos.h"

#define ENA_ADMIN_ENTRY_SIZE   64
#define ENA_RESET_POLL_LIMIT   1000
#define ENA_BACKEND_TIMEOUT_MS 5000

static QTestState *qts(QEna *d)
{
    return d->dev.bus->qts;
}

uint32_t ena_reg_read(QEna *d, uint32_t off)
{
    return qpci_io_readl(&d->dev, d->regs, off);
}

void ena_reg_write(QEna *d, uint32_t off, uint32_t val)
{
    qpci_io_writel(&d->dev, d->regs, off, val);
}

void ena_mmio_resp_setup(QEna *d)
{
    ena_reg_write(d, ENA_REGS_MMIO_RESP_LO_OFF, (uint32_t)d->mmio_resp);
    ena_reg_write(d, ENA_REGS_MMIO_RESP_HI_OFF, (uint32_t)(d->mmio_resp >> 32));
}

uint32_t ena_readless(QEna *d, uint16_t off)
{
    struct ena_admin_ena_mmio_req_read_less_resp resp;
    uint16_t req_id = ++d->mmio_seq;
    int i;

    resp.req_id = cpu_to_le16(req_id + 0xdead);
    qtest_memwrite(qts(d), d->mmio_resp, &resp, sizeof(resp));
    ena_reg_write(d, ENA_REGS_MMIO_REG_READ_OFF,
                  ((uint32_t)off << ENA_REGS_MMIO_REG_READ_REG_OFF_SHIFT) | req_id);

    for (i = 0; i < ENA_RESET_POLL_LIMIT; i++) {
        qtest_memread(qts(d), d->mmio_resp, &resp, sizeof(resp));
        if (le16_to_cpu(resp.req_id) == req_id) {
            break;
        }
        qtest_clock_step(qts(d), 1000);
    }
    g_assert_cmpuint(le16_to_cpu(resp.req_id), ==, req_id);
    g_assert_cmpuint(le16_to_cpu(resp.reg_off), ==, off);
    return le32_to_cpu(resp.reg_val);
}

static void ena_wait_reset_state(QEna *d, uint32_t expected)
{
    int i;

    for (i = 0; i < ENA_RESET_POLL_LIMIT; i++) {
        uint32_t sts = ena_readless(d, ENA_REGS_DEV_STS_OFF);

        if ((sts & ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK) == expected) {
            return;
        }
        qtest_clock_step(qts(d), 100000);
    }
    g_assert_not_reached();
}

void ena_dev_reset(QEna *d)
{
    ena_reg_write(d, ENA_REGS_DEV_CTL_OFF, ENA_REGS_DEV_CTL_DEV_RESET_MASK |
                  (ENA_REGS_RESET_NORMAL << ENA_REGS_DEV_CTL_RESET_REASON_SHIFT));
    ena_mmio_resp_setup(d);
    ena_wait_reset_state(d, ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK);
    ena_reg_write(d, ENA_REGS_DEV_CTL_OFF, 0);
    ena_wait_reset_state(d, 0);
    g_assert_cmphex(ena_readless(d, ENA_REGS_DEV_STS_OFF) &
                    ENA_REGS_DEV_STS_READY_MASK, ==, ENA_REGS_DEV_STS_READY_MASK);
}

static uint64_t ena_alloc_zeroed(QEna *d, size_t size)
{
    uint64_t addr = guest_alloc(d->alloc, size);

    g_assert(addr);
    qtest_memset(qts(d), addr, 0, size);
    return addr;
}

void ena_admin_init(QEna *d)
{
    uint32_t caps;

    d->aq = ena_alloc_zeroed(d, ENA_TEST_AQ_DEPTH * ENA_ADMIN_ENTRY_SIZE);
    d->acq = ena_alloc_zeroed(d, ENA_TEST_AQ_DEPTH * ENA_ADMIN_ENTRY_SIZE);
    d->aenq = ena_alloc_zeroed(d, ENA_TEST_AENQ_DEPTH * ENA_ADMIN_ENTRY_SIZE);
    d->aq_tail = 0;
    d->acq_head = 0;
    d->acq_phase = true;
    d->aenq_head = ENA_TEST_AENQ_DEPTH;
    d->aenq_phase = true;

    ena_reg_write(d, ENA_REGS_AQ_BASE_LO_OFF, (uint32_t)d->aq);
    ena_reg_write(d, ENA_REGS_AQ_BASE_HI_OFF, (uint32_t)(d->aq >> 32));
    ena_reg_write(d, ENA_REGS_ACQ_BASE_LO_OFF, (uint32_t)d->acq);
    ena_reg_write(d, ENA_REGS_ACQ_BASE_HI_OFF, (uint32_t)(d->acq >> 32));
    caps = ENA_TEST_AQ_DEPTH |
           (ENA_ADMIN_ENTRY_SIZE << ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_SHIFT);
    ena_reg_write(d, ENA_REGS_AQ_CAPS_OFF, caps);
    ena_reg_write(d, ENA_REGS_ACQ_CAPS_OFF, caps);

    ena_reg_write(d, ENA_REGS_AENQ_BASE_LO_OFF, (uint32_t)d->aenq);
    ena_reg_write(d, ENA_REGS_AENQ_BASE_HI_OFF, (uint32_t)(d->aenq >> 32));
    ena_reg_write(d, ENA_REGS_AENQ_CAPS_OFF, ENA_TEST_AENQ_DEPTH |
                  (ENA_ADMIN_ENTRY_SIZE << ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_SHIFT));
}

int ena_admin_cmd(QEna *d, void *cmd, size_t cmd_len, void *resp,
                  size_t resp_len)
{
    struct ena_admin_aq_entry entry = {};
    struct ena_admin_acq_entry comp;
    uint16_t cmd_id = d->aq_tail % ENA_TEST_AQ_DEPTH;
    bool aq_phase = ((d->aq_tail / ENA_TEST_AQ_DEPTH) & 1) == 0;
    int i;

    g_assert(cmd_len <= sizeof(entry));
    g_assert(resp_len <= sizeof(comp));
    memcpy(&entry, cmd, cmd_len);
    entry.aq_common_descriptor.command_id = cpu_to_le16(cmd_id);
    entry.aq_common_descriptor.flags |= aq_phase;
    qtest_memwrite(qts(d), d->aq + cmd_id * sizeof(entry), &entry, sizeof(entry));
    d->aq_tail++;
    ena_reg_write(d, ENA_REGS_AQ_DB_OFF, d->aq_tail);

    for (i = 0; i < ENA_RESET_POLL_LIMIT; i++) {
        qtest_memread(qts(d), d->acq + (d->acq_head % ENA_TEST_AQ_DEPTH) *
                      sizeof(comp), &comp, sizeof(comp));
        if ((comp.acq_common_descriptor.flags & ENA_ADMIN_ACQ_COMMON_DESC_PHASE_MASK)
            == d->acq_phase) {
            break;
        }
        qtest_clock_step(qts(d), 1000);
    }
    g_assert_cmpuint(comp.acq_common_descriptor.flags &
                     ENA_ADMIN_ACQ_COMMON_DESC_PHASE_MASK, ==, d->acq_phase);
    g_assert_cmpuint(le16_to_cpu(comp.acq_common_descriptor.command) &
                     ENA_ADMIN_ACQ_COMMON_DESC_COMMAND_ID_MASK, ==, cmd_id);
    d->acq_head++;
    if (d->acq_head % ENA_TEST_AQ_DEPTH == 0) {
        d->acq_phase = !d->acq_phase;
    }
    if (resp) {
        memcpy(resp, &comp, resp_len);
    }
    return comp.acq_common_descriptor.status;
}

int ena_get_feature(QEna *d, uint8_t id, uint8_t ver, uint64_t ctrl_buf,
                    uint32_t ctrl_len, struct ena_admin_get_feat_resp *resp)
{
    struct ena_admin_get_feat_cmd cmd = {};

    cmd.aq_common_descriptor.opcode = ENA_ADMIN_GET_FEATURE;
    if (ctrl_len) {
        cmd.aq_common_descriptor.flags =
            ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK;
    }
    cmd.control_buffer.address.mem_addr_low = cpu_to_le32(ctrl_buf);
    cmd.control_buffer.address.mem_addr_high = cpu_to_le16(ctrl_buf >> 32);
    cmd.control_buffer.length = cpu_to_le32(ctrl_len);
    cmd.feat_common.feature_id = id;
    cmd.feat_common.feature_version = ver;
    return ena_admin_cmd(d, &cmd, sizeof(cmd), resp, sizeof(*resp));
}

int ena_set_feature(QEna *d, struct ena_admin_set_feat_cmd *cmd,
                    uint64_t ctrl_buf, uint32_t ctrl_len)
{
    struct ena_admin_set_feat_resp resp;

    cmd->aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
    if (ctrl_len) {
        cmd->aq_common_descriptor.flags =
            ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK;
    }
    cmd->control_buffer.address.mem_addr_low = cpu_to_le32(ctrl_buf);
    cmd->control_buffer.address.mem_addr_high = cpu_to_le16(ctrl_buf >> 32);
    cmd->control_buffer.length = cpu_to_le32(ctrl_len);
    return ena_admin_cmd(d, cmd, sizeof(*cmd), &resp, sizeof(resp));
}

int ena_create_cq(QEna *d, uint16_t depth, uint8_t entry_words,
                  uint32_t msix_vector, uint64_t base,
                  struct ena_admin_acq_create_cq_resp_desc *resp)
{
    struct ena_admin_aq_create_cq_cmd cmd = {};

    cmd.aq_common_descriptor.opcode = ENA_ADMIN_CREATE_CQ;
    cmd.cq_caps_1 = ENA_ADMIN_AQ_CREATE_CQ_CMD_INTERRUPT_MODE_ENABLED_MASK;
    cmd.cq_caps_2 = entry_words & ENA_ADMIN_AQ_CREATE_CQ_CMD_CQ_ENTRY_SIZE_WORDS_MASK;
    cmd.cq_depth = cpu_to_le16(depth);
    cmd.msix_vector = cpu_to_le32(msix_vector);
    cmd.cq_ba.mem_addr_low = cpu_to_le32(base);
    cmd.cq_ba.mem_addr_high = cpu_to_le16(base >> 32);
    return ena_admin_cmd(d, &cmd, sizeof(cmd), resp, sizeof(*resp));
}

int ena_create_sq(QEna *d, bool tx, uint8_t placement, uint16_t cq_idx,
                  uint16_t depth, uint64_t base,
                  struct ena_admin_acq_create_sq_resp_desc *resp)
{
    struct ena_admin_aq_create_sq_cmd cmd = {};

    cmd.aq_common_descriptor.opcode = ENA_ADMIN_CREATE_SQ;
    cmd.sq_identity = (tx ? ENA_ADMIN_SQ_DIRECTION_TX : ENA_ADMIN_SQ_DIRECTION_RX)
                      << ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_SHIFT;
    cmd.sq_caps_2 = placement |
                    (ENA_ADMIN_COMPLETION_POLICY_DESC <<
                     ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_SHIFT);
    cmd.sq_caps_3 = ENA_ADMIN_AQ_CREATE_SQ_CMD_IS_PHYSICALLY_CONTIGUOUS_MASK;
    cmd.cq_idx = cpu_to_le16(cq_idx);
    cmd.sq_depth = cpu_to_le16(depth);
    cmd.sq_ba.mem_addr_low = cpu_to_le32(base);
    cmd.sq_ba.mem_addr_high = cpu_to_le16(base >> 32);
    return ena_admin_cmd(d, &cmd, sizeof(cmd), resp, sizeof(*resp));
}

int ena_destroy_sq(QEna *d, uint16_t sq_idx, bool tx)
{
    struct ena_admin_aq_destroy_sq_cmd cmd = {};
    struct ena_admin_acq_destroy_sq_resp_desc resp;

    cmd.aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_SQ;
    cmd.sq.sq_idx = cpu_to_le16(sq_idx);
    cmd.sq.sq_identity = (tx ? ENA_ADMIN_SQ_DIRECTION_TX : ENA_ADMIN_SQ_DIRECTION_RX)
                         << ENA_ADMIN_SQ_SQ_DIRECTION_SHIFT;
    return ena_admin_cmd(d, &cmd, sizeof(cmd), &resp, sizeof(resp));
}

int ena_destroy_cq(QEna *d, uint16_t cq_idx)
{
    struct ena_admin_aq_destroy_cq_cmd cmd = {};
    struct ena_admin_acq_destroy_cq_resp_desc resp;

    cmd.aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_CQ;
    cmd.cq_idx = cpu_to_le16(cq_idx);
    return ena_admin_cmd(d, &cmd, sizeof(cmd), &resp, sizeof(resp));
}

void ena_aenq_enable(QEna *d)
{
    ena_reg_write(d, ENA_REGS_AENQ_HEAD_DB_OFF, d->aenq_head);
}

bool ena_aenq_poll(QEna *d, struct ena_admin_aenq_entry *e)
{
    uint16_t idx = d->aenq_head % ENA_TEST_AENQ_DEPTH;

    qtest_memread(qts(d), d->aenq + idx * sizeof(*e), e, sizeof(*e));
    if ((e->aenq_common_desc.flags & ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK)
        != d->aenq_phase) {
        return false;
    }
    d->aenq_head++;
    if (d->aenq_head % ENA_TEST_AENQ_DEPTH == 0) {
        d->aenq_phase = !d->aenq_phase;
    }
    ena_reg_write(d, ENA_REGS_AENQ_HEAD_DB_OFF, d->aenq_head);
    return true;
}

void ena_msix_setup(QEna *d, uint16_t vector)
{
    uint64_t off = d->dev.msix_table_off + vector * PCI_MSIX_ENTRY_SIZE;
    uint32_t ctrl;

    g_assert(d->dev.msix_enabled);
    d->msix_addr[vector] = ena_alloc_zeroed(d, 4);
    qpci_io_writel(&d->dev, d->dev.msix_table_bar, off + PCI_MSIX_ENTRY_LOWER_ADDR,
                   (uint32_t)d->msix_addr[vector]);
    qpci_io_writel(&d->dev, d->dev.msix_table_bar, off + PCI_MSIX_ENTRY_UPPER_ADDR,
                   (uint32_t)(d->msix_addr[vector] >> 32));
    qpci_io_writel(&d->dev, d->dev.msix_table_bar, off + PCI_MSIX_ENTRY_DATA,
                   ENA_TEST_MSIX_DATA);
    ctrl = qpci_io_readl(&d->dev, d->dev.msix_table_bar,
                         off + PCI_MSIX_ENTRY_VECTOR_CTRL);
    qpci_io_writel(&d->dev, d->dev.msix_table_bar, off + PCI_MSIX_ENTRY_VECTOR_CTRL,
                   ctrl & ~PCI_MSIX_ENTRY_CTRL_MASKBIT);
}

bool ena_msix_fired(QEna *d, uint16_t vector)
{
    g_assert(d->msix_addr[vector]);
    return qtest_readl(qts(d), d->msix_addr[vector]) == ENA_TEST_MSIX_DATA;
}

void ena_msix_clear(QEna *d, uint16_t vector)
{
    qtest_writel(qts(d), d->msix_addr[vector], 0);
}

void ena_bringup(QEna *d)
{
    ena_dev_reset(d);
    ena_admin_init(d);
    ena_msix_setup(d, ENA_TEST_ADMIN_VECTOR);
    ena_aenq_enable(d);
}

/* network backend */

static void ena_test_after(void *sockets)
{
    int *fds = sockets;

    close(fds[0]);
    qos_invalidate_command_line();
    close(fds[1]);
    g_free(fds);
}

void *ena_test_before(GString *cmd_line, void *arg)
{
    int *fds = g_new(int, 2);
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, fds);

    g_assert_cmpint(ret, !=, -1);
    g_string_append_printf(cmd_line, " -netdev socket,fd=%d,id=hs0 ", fds[1]);
    g_test_queue_destroy(ena_test_after, fds);
    return fds;
}

int ena_backend_fd(void *data)
{
    return ((int *)data)[0];
}

void ena_backend_send(int fd, const void *frame, size_t len)
{
    uint32_t hdr = htonl(len);
    ssize_t ret;

    ret = send(fd, &hdr, sizeof(hdr), 0);
    g_assert_cmpint(ret, ==, sizeof(hdr));
    ret = send(fd, frame, len, 0);
    g_assert_cmpint(ret, ==, len);
}

ssize_t ena_backend_recv(int fd, void *buf, size_t cap)
{
    GPollFD pfd = { .fd = fd, .events = G_IO_IN };
    uint32_t hdr;
    uint32_t len;
    ssize_t ret;
    size_t got = 0;

    if (g_poll(&pfd, 1, ENA_BACKEND_TIMEOUT_MS) <= 0) {
        return -1;
    }
    ret = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
    if (ret != sizeof(hdr)) {
        return -1;
    }
    len = ntohl(hdr);
    g_assert_cmpuint(len, <=, cap);
    while (got < len) {
        ret = recv(fd, (char *)buf + got, len - got, 0);
        g_assert_cmpint(ret, >, 0);
        got += ret;
    }
    return len;
}

/* qos graph node */

static void ena_foreach_callback(QPCIDevice *dev, int devfn, void *data)
{
    QPCIDevice *res = data;

    memcpy(res, dev, sizeof(QPCIDevice));
    g_free(dev);
}

static void ena_start_hw(QOSGraphObject *obj)
{
    QEna *d = (QEna *)obj;

    qpci_device_enable(&d->dev);
    qpci_msix_enable(&d->dev);
}

static void ena_destructor(QOSGraphObject *obj)
{
    QEna *d = (QEna *)obj;

    qpci_msix_disable(&d->dev);
    qpci_iounmap(&d->dev, d->mem);
    qpci_iounmap(&d->dev, d->regs);
}

static void *ena_get_driver(void *obj, const char *interface)
{
    QEna *d = obj;

    if (!g_strcmp0(interface, "pci-device")) {
        return &d->dev;
    }
    fprintf(stderr, "%s not present in ena\n", interface);
    g_assert_not_reached();
}

static void *ena_create(void *pci_bus, QGuestAllocator *alloc, void *addr)
{
    QEna *d = g_new0(QEna, 1);
    QPCIBus *bus = pci_bus;
    QPCIAddress *address = addr;

    qpci_device_foreach(bus, address->vendor_id, address->device_id,
                        ena_foreach_callback, &d->dev);
    d->regs = qpci_iomap(&d->dev, ENA_TEST_REG_BAR, NULL);
    d->mem = qpci_iomap(&d->dev, ENA_TEST_MEM_BAR, NULL);
    d->alloc = alloc;
    d->mmio_resp = ena_alloc_zeroed(d, 8);

    d->obj.get_driver = ena_get_driver;
    d->obj.start_hw = ena_start_hw;
    d->obj.destructor = ena_destructor;
    return &d->obj;
}

static void ena_register_nodes(void)
{
    QPCIAddress addr = {
        .vendor_id = ENA_TEST_VENDOR_ID,
        .device_id = ENA_TEST_DEVICE_ID,
    };
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "netdev=hs0,mac=52:54:00:12:34:56",
    };

    add_qpci_address(&opts, &addr);
    qos_node_create_driver("ena", ena_create);
    qos_node_consumes("ena", "pci-bus", &opts);
}

libqos_init(ena_register_nodes);
