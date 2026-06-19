/*
 * QEMU device emulation of the Amazon Elastic Network Adapter (ENA).
 *
 * Stub: exposes the BAR0 register window per docs/wiki/registers.md —
 * identity/caps registers, the reset handshake and the indirect
 * (readless) MMIO read mechanism. Admin queue processing, AENQ and the
 * datapath are not implemented yet.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "net/net.h"
#include "qom/object.h"

#include "ena_regs.h"
#include "ena_admin_desc.h"
#include "ena_io_desc.h"

#define TYPE_ENA "ena"
OBJECT_DECLARE_SIMPLE_TYPE(EnaState, ENA)

#define ENA_PCI_VENDOR_ID   0x1d0f /* Amazon */
#define ENA_PCI_DEVICE_ID   0x0ec2 /* ENA physical function */

#define ENA_BAR0_SIZE       (16 * KiB)
#define ENA_MSIX_BAR_IDX    1
#define ENA_MSIX_VECTORS    9 /* vector 0 = admin/AENQ, 1..8 = IO queues */

/* All admin/AENQ rings use 64-byte entries (admin-queue.md, aenq.md). */
#define ENA_ADMIN_ENTRY_SIZE    64

/* Device identity reported in GET_FEATURE(DEVICE_ATTRIBUTES). */
#define ENA_DEV_PHYS_ADDR_WIDTH 48
#define ENA_DEV_VIRT_ADDR_WIDTH 48
#define ENA_DEV_MAX_MTU        9001 

/* AENQ groups the device can ever produce (GET_FEATURE(AENQ_CONFIG)). */
#define ENA_AENQ_SUPPORTED_GROUPS \
    ((1u << ENA_ADMIN_LINK_CHANGE) | (1u << ENA_ADMIN_FATAL_ERROR) | \
     (1u << ENA_ADMIN_WARNING) | (1u << ENA_ADMIN_NOTIFICATION) | \
     (1u << ENA_ADMIN_KEEP_ALIVE))

/* supported_features bitmap in the DEVICE_ATTRIBUTES response. STATELESS_-
 * OFFLOAD_CONFIG is advertised so the driver's mandatory GET(11) is answered
 * (with an all-zero, i.e. no-offload, descriptor — offloads.md §1); RSS/hash
 * features are deliberately omitted (no offloads except inherent RX scatter). */
#define ENA_SUPPORTED_FEATURES \
    ((1u << ENA_ADMIN_DEVICE_ATTRIBUTES) | (1u << ENA_ADMIN_MTU) | \
     (1u << ENA_ADMIN_AENQ_CONFIG) | (1u << ENA_ADMIN_LLQ) | \
     (1u << ENA_ADMIN_STATELESS_OFFLOAD_CONFIG))

/* IO queue limits and the per-SQ doorbell window inside BAR0. The device
 * hands each created SQ a doorbell offset of ENA_IO_SQ_DB_BASE + sq_idx*4;
 * the driver writes the SQ tail there (queue-setup.md §4). */
#define ENA_MAX_IO_QUEUES   128
#define ENA_IO_SQ_DB_BASE   0x1000
#define ENA_IO_SQ_DB_OFF(idx)   (ENA_IO_SQ_DB_BASE + (idx) * 4)

/* SQ direction (sq_identity bits 7:5) and placement policy (sq_caps_2 bits 3:0). */
#define ENA_ADMIN_SQ_DIRECTION_SHIFT    5
#define ENA_ADMIN_SQ_DIRECTION_TX       1
#define ENA_ADMIN_SQ_PLACEMENT_MASK     0x0f
#define ENA_ADMIN_SQ_PLACEMENT_HOST     1
#define ENA_ADMIN_SQ_PLACEMENT_DEV      3   /* LLQ: descriptors in device memory */

/* TX completion descriptor size (sizeof(struct ena_eth_io_tx_cdesc)). */
#define ENA_TX_CDESC_SIZE   8

/* TX SQ descriptor size (sizeof(struct ena_eth_io_tx_desc)). */
#define ENA_TX_DESC_SIZE    16

/* RX SQ buffer descriptor / RX completion descriptor sizes. */
#define ENA_RX_DESC_SIZE    16  /* sizeof(struct ena_eth_io_rx_desc) */
#define ENA_RX_CDESC_SIZE   16  /* sizeof(struct ena_eth_io_rx_cdesc_base) */

/* SQ direction Rx (sq_identity bits 7:5); Tx is ENA_ADMIN_SQ_DIRECTION_TX. */
#define ENA_ADMIN_SQ_DIRECTION_RX       2

/* Max RX buffers one frame may be scattered across (ENA_PKT_MAX_BUFS, the
 * driver's per-packet SGL ceiling; rx-path.md §2.4). A frame needing more
 * posted buffers than this is dropped. */
#define ENA_RX_MAX_SGL      17

/* An rx_desc.length of 0 means 64 KiB (rx-path.md §2.1). */
#define ENA_RX_DESC_LEN_64K 65536

/*
 * LLQ (low-latency queue) device memory (BAR2, the "MEM BAR"). LLQ TX SQs push
 * their descriptor ring + inline headers into this BAR (llq.md §1, §5). Each
 * LLQ SQ gets a fixed slice large enough for the worst-case ring: 256B*512 or
 * 128B*1024 = 128 KiB. One slice per IO queue: the driver supports up to
 * ENA_MAX_NUM_IO_QUEUES (128) queues, so 128 slices.
 */
#define ENA_MAX_LLQ_SQ      ENA_MAX_IO_QUEUES
#define ENA_LLQ_SLICE_SIZE  (128 * KiB)
#define ENA_MEM_BAR_IDX     2
#define ENA_MEM_BAR_SIZE    (ENA_MAX_LLQ_SQ * ENA_LLQ_SLICE_SIZE)
#define ENA_LLQ_SLICE_OFF(slice)    ((uint32_t)(slice) * ENA_LLQ_SLICE_SIZE)

/* LLQ feature sub-enums (llq.md §2). header_location/entry_size/stride are
 * bitfields in the supported masks, plain enum values in the enabled fields. */
#define ENA_ADMIN_LLQ_INLINE_HEADER         1
#define ENA_ADMIN_LLQ_ENTRY_SIZE_128B       1
#define ENA_ADMIN_LLQ_ENTRY_SIZE_192B       2
#define ENA_ADMIN_LLQ_ENTRY_SIZE_256B       4
#define ENA_ADMIN_LLQ_SINGLE_DESC_PER_ENTRY     1
#define ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY  2

/* Device-advertised LLQ capabilities (GET_FEATURE(LLQ)). */
#define ENA_LLQ_MAX_DEPTH       1024    /* 128B entries */
#define ENA_LLQ_MAX_WIDE_DEPTH  512     /* 256B entries */
#define ENA_LLQ_FEATURE_VERSION 1

/* Default negotiated LLQ config (recommended; used until the driver SETs it). */
#define ENA_LLQ_DEF_ENTRY_SIZE          256
#define ENA_LLQ_DEF_DESCS_BEFORE_HEADER 2

/* VERSION 2.0; controller version 0.0.1 = MIN_ENA_CTRL_VER */
#define ENA_STUB_VERSION        (2 << ENA_REGS_VERSION_MAJOR_VERSION_SHIFT)
#define ENA_STUB_CTRL_VERSION   ENA_MIN_CTRL_VER
/* 48-bit DMA, reset timeout 1 (100 ms unit), admin timeout 0 (3 s default) */
#define ENA_STUB_CAPS \
    ((48 << ENA_REGS_CAPS_DMA_ADDR_WIDTH_SHIFT) | \
     (1 << ENA_REGS_CAPS_RESET_TIMEOUT_SHIFT))

struct EnaState {
    PCIDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion mem_bar;   /* BAR2: LLQ device memory */
    uint8_t *llq_mem;       /* host pointer to mem_bar backing RAM */

    /* NIC backend: frames from the QEMU network layer land in ena_rx_receive,
     * which scatters them into posted RX SQ buffers (rx-path.md). */
    NICState *nic;
    NICConf  conf;

    /* Deferred TX processing: a doorbell latches the SQ tail and arms this BH,
     * which drains the TX SQs out of the MMIO write path (tx-path.md §2). */
    QEMUBH   *tx_bh;
    bool     tx_scheduled;  /* BH armed/in-progress — avoid redundant schedule */

    /* driver-programmed configuration (write-only registers) */
    uint32_t aq_base_lo, aq_base_hi, aq_caps;
    uint32_t acq_base_lo, acq_base_hi, acq_caps;
    uint32_t aenq_base_lo, aenq_base_hi, aenq_caps;
    uint32_t aenq_head_db;
    uint32_t intr_mask;
    uint32_t mmio_resp_lo, mmio_resp_hi;

    uint32_t dev_sts;

    /* Negotiated LLQ config (GET defaults / SET_FEATURE(LLQ), llq.md §2-§4). */
    uint16_t llq_entry_size;            /* bytes per pushed LLQ entry */
    uint8_t  llq_descs_before_header;   /* descriptors in the first entry */
    uint8_t  llq_descs_per_entry;       /* descriptors in subsequent entries */
    uint8_t  llq_header_location;
    bool     llq_slice_used[ENA_MAX_LLQ_SQ];

    /* admin queue processing state */
    uint32_t aq_head;       /* absolute count of AQ entries consumed */
    uint32_t acq_tail;      /* absolute ACQ producer index */
    uint8_t  acq_phase;     /* phase bit stamped on the next completion */

    /* AENQ production state */
    uint32_t aenq_prod;     /* absolute AENQ producer index */
    uint8_t  aenq_phase;    /* phase bit stamped on the next event */
    bool     aenq_started;  /* initial events already produced */
    uint32_t aenq_enabled_groups;

    /* IO queues (device-assigned index = array slot) */
    struct EnaIoCq {
        bool     valid;
        uint64_t base;
        uint16_t depth;
        uint8_t  entry_size_words;
        uint16_t tail;      /* device CQ producer index */
        uint8_t  phase;     /* phase bit stamped on the next cdesc */
    } io_cq[ENA_MAX_IO_QUEUES];
    struct EnaIoSq {
        bool     valid;
        uint64_t base;
        uint16_t depth;
        uint16_t cq_idx;
        uint8_t  direction;
        uint16_t head;      /* device SQ consumer index (lines for LLQ) */
        uint32_t tail;      /* last value written to the SQ doorbell */

        /* LLQ (DEV placement) state; llq=false for host placement. */
        bool     llq;
        int8_t   llq_slice;             /* MEM BAR slice index, -1 if host */
        uint32_t llq_offset;            /* byte offset of the ring into BAR2 */
        uint16_t llq_entry_size;        /* snapshot of negotiated config */
        uint8_t  llq_descs_before_header;
        uint8_t  llq_descs_per_entry;
    } io_sq[ENA_MAX_IO_QUEUES];
};

/* Clear all driver-programmed state; reset clears the device including
 * the mmio response buffer address (the driver re-publishes it). */
static void ena_cfg_reset(EnaState *s)
{
    s->aq_base_lo = s->aq_base_hi = s->aq_caps = 0;
    s->acq_base_lo = s->acq_base_hi = s->acq_caps = 0;
    s->aenq_base_lo = s->aenq_base_hi = s->aenq_caps = 0;
    s->aenq_head_db = 0;
    s->intr_mask = 0;
    s->mmio_resp_lo = s->mmio_resp_hi = 0;

    s->aq_head = 0;
    s->acq_tail = 0;
    s->acq_phase = 1;
    s->aenq_prod = 0;
    s->aenq_phase = 1;
    s->aenq_started = false;
    s->aenq_enabled_groups = 0;

    /* LLQ negotiated config defaults to the recommended values (llq.md §3);
     * the driver may override them via SET_FEATURE(LLQ) before CREATE_SQ. */
    s->llq_entry_size = ENA_LLQ_DEF_ENTRY_SIZE;
    s->llq_descs_before_header = ENA_LLQ_DEF_DESCS_BEFORE_HEADER;
    s->llq_descs_per_entry = ENA_LLQ_DEF_ENTRY_SIZE / ENA_TX_DESC_SIZE;
    s->llq_header_location = ENA_ADMIN_LLQ_INLINE_HEADER;
    memset(s->llq_slice_used, 0, sizeof(s->llq_slice_used));

    memset(s->io_cq, 0, sizeof(s->io_cq));
    memset(s->io_sq, 0, sizeof(s->io_sq));

    /* A pending TX BH finds no valid SQs after this and no-ops; allow a
     * post-reset doorbell to re-arm it. */
    s->tx_scheduled = false;
}

static uint32_t ena_reg_read(EnaState *s, hwaddr addr)
{
    switch (addr) {
    case ENA_REGS_VERSION_OFF:
        return ENA_STUB_VERSION;
    case ENA_REGS_CONTROLLER_VERSION_OFF:
        return ENA_STUB_CTRL_VERSION;
    case ENA_REGS_CAPS_OFF:
        return ENA_STUB_CAPS;
    case ENA_REGS_CAPS_EXT_OFF:
        return 0;
    case ENA_REGS_DEV_STS_OFF:
        return s->dev_sts;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ena: read of unimplemented register 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

/* Indirect (readless) register read: DMA-write the 8-byte response
 * {req_id, reg_off, reg_val} to the buffer published via MMIO_RESP_LO/HI. */
static void ena_mmio_reg_read_req(EnaState *s, uint32_t val)
{
    uint16_t req_id = val & ENA_REGS_MMIO_REG_READ_REQ_ID_MASK;
    uint16_t reg_off = (val & ENA_REGS_MMIO_REG_READ_REG_OFF_MASK) >>
                       ENA_REGS_MMIO_REG_READ_REG_OFF_SHIFT;
    dma_addr_t resp = ((dma_addr_t)s->mmio_resp_hi << 32) | s->mmio_resp_lo;
    uint8_t buf[8];

    if (!resp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ena: readless read with no response buffer\n");
        return;
    }

    stw_le_p(buf + 0, req_id);
    stw_le_p(buf + 2, reg_off);
    stl_le_p(buf + 4, ena_reg_read(s, reg_off));
    pci_dma_write(PCI_DEVICE(s), resp, buf, sizeof(buf));
}

/* ------------------------------------------------------------------ */
/* Admin queue (AQ/ACQ) command processing                            */
/* ------------------------------------------------------------------ */

static dma_addr_t ena_acq_base(EnaState *s)
{
    return ((dma_addr_t)s->acq_base_hi << 32) | s->acq_base_lo;
}

/*
 * Post one completion to the ACQ at the current producer index: echo the
 * command_id, stamp status/phase and the consumed-so-far sq_head_indx, and
 * copy @resp_len bytes of feature-response data into the entry body. Advance
 * the producer, flipping the phase bit on ring wrap (admin-queue.md §2-§4).
 */
static void ena_acq_complete(EnaState *s, uint16_t cmd_id, uint8_t status,
                             const void *resp, size_t resp_len)
{
    uint32_t depth = s->acq_caps & ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK;
    dma_addr_t base = ena_acq_base(s);
    uint8_t entry[ENA_ADMIN_ENTRY_SIZE];
    uint32_t idx;

    if (!depth || !base) {
        return;
    }
    idx = s->acq_tail % depth;

    memset(entry, 0, sizeof(entry));
    /* acq_common_desc: command(0), status(2), flags(3), ext_status(4),
     * sq_head_indx(6). */
    stw_le_p(entry + 0, cmd_id & ENA_ADMIN_ACQ_COMMON_DESC_COMMAND_ID_MASK);
    entry[2] = status;
    entry[3] = s->acq_phase & ENA_ADMIN_ACQ_COMMON_DESC_PHASE_MASK;
    stw_le_p(entry + 6, (uint16_t)s->aq_head);
    if (resp && resp_len) {
        g_assert(resp_len <= sizeof(entry) - 8);
        memcpy(entry + 8, resp, resp_len);
    }

    pci_dma_write(PCI_DEVICE(s), base + (dma_addr_t)idx * ENA_ADMIN_ENTRY_SIZE,
                  entry, sizeof(entry));

    s->acq_tail++;
    if (s->acq_tail % depth == 0) {
        s->acq_phase ^= 1;
    }
}

/* GET_FEATURE(DEVICE_ATTRIBUTES): emit struct ena_admin_device_attr_feature_desc
 * inline in the completion body (admin-queue.md §6). */
static void ena_get_device_attributes(EnaState *s, uint16_t cmd_id)
{
    uint8_t desc[40];
    static const uint8_t mac[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };

    memset(desc, 0, sizeof(desc));
    stl_le_p(desc + 0, 0);                          /* impl_id */
    stl_le_p(desc + 4, 0);                          /* device_version */
    stl_le_p(desc + 8, ENA_SUPPORTED_FEATURES);     /* supported_features */
    stl_le_p(desc + 12, 0);                         /* capabilities */
    stl_le_p(desc + 16, ENA_DEV_PHYS_ADDR_WIDTH);   /* phys_addr_width */
    stl_le_p(desc + 20, ENA_DEV_VIRT_ADDR_WIDTH);   /* virt_addr_width */
    memcpy(desc + 24, mac, sizeof(mac));            /* mac_addr[6] + resv[2] */
    stl_le_p(desc + 32, ENA_DEV_MAX_MTU);           /* max_mtu */

    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, desc, sizeof(desc));
}

/* GET_FEATURE(AENQ_CONFIG): supported (advertised) + enabled (current). */
static void ena_get_aenq_config(EnaState *s, uint16_t cmd_id)
{
    uint8_t desc[8];

    stl_le_p(desc + 0, ENA_AENQ_SUPPORTED_GROUPS);
    stl_le_p(desc + 4, s->aenq_enabled_groups);

    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, desc, sizeof(desc));
}

/*
 * GET_FEATURE(STATELESS_OFFLOAD_CONFIG): emit struct
 * ena_admin_feature_offload_desc (3 x u32 = tx / rx_supported / rx_enabled,
 * offloads.md §1). We advertise NO stateless offloads — no TX/RX checksum, no
 * TSO, no RX hash — so all three words are zero. RX scatter is inherent to the
 * multi-buffer Rx datapath and is not an admin-advertised offload, so nothing
 * is set for it here. The feature is GET-only (the driver never SETs it).
 */
static void ena_get_offload_config(EnaState *s, uint16_t cmd_id)
{
    uint8_t desc[12];

    memset(desc, 0, sizeof(desc));
    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, desc, sizeof(desc));
}

/*
 * GET_FEATURE(LLQ): emit struct ena_admin_feature_llq_desc (llq.md §2). We
 * support inline-header mode with 128B/256B entries and single/multiple stride;
 * the supported bitmaps let the driver pick, the enabled fields are echoed back
 * on SET_FEATURE. No accel-mode features are advertised (accel_mode = 0).
 */
static void ena_get_llq_config(EnaState *s, uint16_t cmd_id)
{
    uint8_t desc[56];

    memset(desc, 0, sizeof(desc));
    stl_le_p(desc + 0, ENA_MAX_LLQ_SQ);             /* max_llq_num */
    stl_le_p(desc + 4, ENA_LLQ_MAX_DEPTH);          /* max_llq_depth */
    stw_le_p(desc + 8, ENA_ADMIN_LLQ_INLINE_HEADER); /* header_location supp. */
    stw_le_p(desc + 12, ENA_ADMIN_LLQ_ENTRY_SIZE_128B |
                        ENA_ADMIN_LLQ_ENTRY_SIZE_256B); /* entry_size supp. */
    stw_le_p(desc + 16, 0xff);                      /* desc_num_before_header */
    stw_le_p(desc + 20, ENA_ADMIN_LLQ_SINGLE_DESC_PER_ENTRY |
                        ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY); /* stride supp. */
    desc[24] = ENA_LLQ_FEATURE_VERSION;             /* feature_version */
    desc[25] = ENA_ADMIN_LLQ_ENTRY_SIZE_256B;       /* entry_size_recommended */
    stw_le_p(desc + 26, ENA_LLQ_MAX_WIDE_DEPTH);    /* max_wide_llq_depth */
    /* accel_mode (offset 28, 8 bytes): no accel features advertised. */

    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, desc, sizeof(desc));
}

static void ena_handle_get_feature(EnaState *s, const uint8_t *cmd,
                                   uint16_t cmd_id)
{
    uint8_t feature_id = cmd[17]; /* feat_common.feature_id (offset 16+1) */

    switch (feature_id) {
    case ENA_ADMIN_DEVICE_ATTRIBUTES:
        ena_get_device_attributes(s, cmd_id);
        break;
    case ENA_ADMIN_AENQ_CONFIG:
        ena_get_aenq_config(s, cmd_id);
        break;
    case ENA_ADMIN_LLQ:
        ena_get_llq_config(s, cmd_id);
        break;
    case ENA_ADMIN_STATELESS_OFFLOAD_CONFIG:
        ena_get_offload_config(s, cmd_id);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ena: GET_FEATURE of unsupported feature %u\n",
                      feature_id);
        ena_acq_complete(s, cmd_id, ENA_ADMIN_UNSUPPORTED_OPCODE, NULL, 0);
        break;
    }
}

/* Map an LLQ ring-entry-size enum (llq.md §2) to its byte width. */
static uint16_t ena_llq_entry_size_bytes(uint16_t enabled)
{
    switch (enabled) {
    case ENA_ADMIN_LLQ_ENTRY_SIZE_128B: return 128;
    case ENA_ADMIN_LLQ_ENTRY_SIZE_192B: return 192;
    case ENA_ADMIN_LLQ_ENTRY_SIZE_256B: return 256;
    default:                            return 0;
    }
}

/*
 * SET_FEATURE(LLQ): record the driver's chosen LLQ layout from the enabled
 * fields of the ena_admin_feature_llq_desc payload (offset 20). This happens
 * before any CREATE_SQ; each LLQ SQ snapshots the config at creation
 * (llq.md §4).
 */
static void ena_set_llq_config(EnaState *s, const uint8_t *cmd, uint16_t cmd_id)
{
    uint16_t header_loc = lduw_le_p(cmd + 30); /* header_location_ctrl_enabled */
    uint16_t entry_enum = lduw_le_p(cmd + 34); /* entry_size_ctrl_enabled */
    uint16_t descs_bh   = lduw_le_p(cmd + 38); /* desc_num_before_header_enabled */
    uint16_t stride     = lduw_le_p(cmd + 42); /* descriptors_stride_ctrl_enabled */
    uint16_t entry_size = ena_llq_entry_size_bytes(entry_enum);

    if (header_loc != ENA_ADMIN_LLQ_INLINE_HEADER || !entry_size ||
        descs_bh * ENA_TX_DESC_SIZE >= entry_size) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
        return;
    }

    s->llq_header_location = header_loc;
    s->llq_entry_size = entry_size;
    s->llq_descs_before_header = descs_bh;
    s->llq_descs_per_entry =
        (stride == ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY) ?
        entry_size / ENA_TX_DESC_SIZE : 1;

    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, NULL, 0);
}

static void ena_handle_set_feature(EnaState *s, const uint8_t *cmd,
                                   uint16_t cmd_id)
{
    uint8_t feature_id = cmd[17];

    switch (feature_id) {
    case ENA_ADMIN_MTU:
        /* payload (offset 20): mtu — accepted, datapath not yet implemented. */
        ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, NULL, 0);
        break;
    case ENA_ADMIN_LLQ:
        ena_set_llq_config(s, cmd, cmd_id);
        break;
    case ENA_ADMIN_AENQ_CONFIG:
        /* feature_aenq_desc payload (offset 20): supported, enabled. */
        s->aenq_enabled_groups = ldl_le_p(cmd + 24) & ENA_AENQ_SUPPORTED_GROUPS;
        ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, NULL, 0);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ena: SET_FEATURE of unsupported feature %u\n",
                      feature_id);
        ena_acq_complete(s, cmd_id, ENA_ADMIN_UNSUPPORTED_OPCODE, NULL, 0);
        break;
    }
}

/* Read a 48-bit struct ena_common_mem_addr at @off (low32, high16). */
static uint64_t ena_read_mem_addr(const uint8_t *cmd, size_t off)
{
    return ((uint64_t)lduw_le_p(cmd + off + 4) << 32) | ldl_le_p(cmd + off);
}

static int ena_alloc_slot_cq(EnaState *s)
{
    for (int i = 0; i < ENA_MAX_IO_QUEUES; i++) {
        if (!s->io_cq[i].valid) {
            return i;
        }
    }
    return -1;
}

static int ena_alloc_slot_sq(EnaState *s)
{
    for (int i = 0; i < ENA_MAX_IO_QUEUES; i++) {
        if (!s->io_sq[i].valid) {
            return i;
        }
    }
    return -1;
}

/* Reserve an LLQ device-memory slice (one per LLQ TX SQ); -1 if none free. */
static int ena_alloc_llq_slice(EnaState *s)
{
    for (int i = 0; i < ENA_MAX_LLQ_SQ; i++) {
        if (!s->llq_slice_used[i]) {
            s->llq_slice_used[i] = true;
            return i;
        }
    }
    return -1;
}

/* CREATE_CQ (opcode 3): register the CQ ring and return its device index plus
 * the (here zero, polling/no-IRQ) per-CQ register offsets (queue-setup.md §3). */
static void ena_handle_create_cq(EnaState *s, const uint8_t *cmd,
                                 uint16_t cmd_id)
{
    uint8_t entry_size_words = cmd[5] & 0x1f; /* cq_caps_2 bits 4:0 */
    uint16_t depth = lduw_le_p(cmd + 6);
    uint64_t base = ena_read_mem_addr(cmd, 12);
    uint8_t resp[24];
    int idx;

    if (!depth || (depth & (depth - 1)) || !base || !entry_size_words) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
        return;
    }
    idx = ena_alloc_slot_cq(s);
    if (idx < 0) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE,
                         NULL, 0);
        return;
    }

    s->io_cq[idx].valid = true;
    s->io_cq[idx].base = base;
    s->io_cq[idx].depth = depth;
    s->io_cq[idx].entry_size_words = entry_size_words;
    s->io_cq[idx].tail = 0;
    s->io_cq[idx].phase = 1;

    memset(resp, 0, sizeof(resp));
    stw_le_p(resp + 0, (uint16_t)idx);  /* cq_idx */
    stw_le_p(resp + 2, depth);          /* cq_actual_depth */
    /* numa(4)/head_db(8)/unmask(12) offsets: unused in polling/no-IRQ mode. */
    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, resp, sizeof(resp));
}

/* CREATE_SQ (opcode 1): register the SQ against an existing CQ and return its
 * index + doorbell offset (queue-setup.md §4). HOST placement keeps the
 * descriptor ring in host memory; DEV placement (LLQ) places it in a MEM BAR
 * slice and returns llq_descriptors_offset (llq.md §5). */
static void ena_handle_create_sq(EnaState *s, const uint8_t *cmd,
                                 uint16_t cmd_id)
{
    uint8_t direction = cmd[4] >> ENA_ADMIN_SQ_DIRECTION_SHIFT;
    uint8_t placement = cmd[6] & ENA_ADMIN_SQ_PLACEMENT_MASK;
    uint16_t cq_idx = lduw_le_p(cmd + 8);
    uint16_t depth = lduw_le_p(cmd + 10);
    uint64_t base = ena_read_mem_addr(cmd, 12);
    bool llq = (placement == ENA_ADMIN_SQ_PLACEMENT_DEV);
    int slice = -1;
    uint8_t resp[16];
    int idx;

    if (placement != ENA_ADMIN_SQ_PLACEMENT_HOST && !llq) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
        return;
    }
    if (!depth || (depth & (depth - 1))) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
        return;
    }
    if (llq) {
        /* LLQ is TX-only; the ring lives in device memory (sq_ba unused) and
         * must fit one slice (llq.md §1, §5). */
        if (direction != ENA_ADMIN_SQ_DIRECTION_TX ||
            (uint32_t)depth * s->llq_entry_size > ENA_LLQ_SLICE_SIZE) {
            ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
            return;
        }
    } else if (!base) {
        /* HOST placement: the SQ ring base must be valid. */
        ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
        return;
    }
    /* The CQ must have been created first (queue-setup.md §2). */
    if (cq_idx >= ENA_MAX_IO_QUEUES || !s->io_cq[cq_idx].valid) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
        return;
    }
    idx = ena_alloc_slot_sq(s);
    if (idx < 0) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE,
                         NULL, 0);
        return;
    }
    if (llq) {
        slice = ena_alloc_llq_slice(s);
        if (slice < 0) {
            ena_acq_complete(s, cmd_id, ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE,
                             NULL, 0);
            return;
        }
    }

    s->io_sq[idx].valid = true;
    s->io_sq[idx].base = base;
    s->io_sq[idx].depth = depth;
    s->io_sq[idx].cq_idx = cq_idx;
    s->io_sq[idx].direction = direction;
    s->io_sq[idx].head = 0;
    s->io_sq[idx].tail = 0;
    s->io_sq[idx].llq = llq;
    s->io_sq[idx].llq_slice = llq ? slice : -1;
    if (llq) {
        /* Snapshot the negotiated LLQ layout for this SQ (llq.md §3-§4). */
        s->io_sq[idx].llq_offset = ENA_LLQ_SLICE_OFF(slice);
        s->io_sq[idx].llq_entry_size = s->llq_entry_size;
        s->io_sq[idx].llq_descs_before_header = s->llq_descs_before_header;
        s->io_sq[idx].llq_descs_per_entry = s->llq_descs_per_entry;
    }

    memset(resp, 0, sizeof(resp));
    stw_le_p(resp + 0, (uint16_t)idx);              /* sq_idx */
    stl_le_p(resp + 4, ENA_IO_SQ_DB_OFF(idx));      /* sq_doorbell_offset */
    if (llq) {
        stl_le_p(resp + 8, s->io_sq[idx].llq_offset); /* llq_descriptors_offset */
        /* llq_headers_offset(12): inline-header mode, unused (llq.md §5). */
    }
    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, resp, sizeof(resp));
}

/* DESTROY_SQ (opcode 2): free the SQ identified by its device-assigned index. */
static void ena_handle_destroy_sq(EnaState *s, const uint8_t *cmd,
                                  uint16_t cmd_id)
{
    uint16_t sq_idx = lduw_le_p(cmd + 4); /* ena_admin_sq.sq_idx */

    if (sq_idx >= ENA_MAX_IO_QUEUES || !s->io_sq[sq_idx].valid) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
        return;
    }
    if (s->io_sq[sq_idx].llq) {
        s->llq_slice_used[s->io_sq[sq_idx].llq_slice] = false;
    }
    s->io_sq[sq_idx].valid = false;
    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, NULL, 0);
}

/* DESTROY_CQ (opcode 4): free the CQ identified by its device-assigned index. */
static void ena_handle_destroy_cq(EnaState *s, const uint8_t *cmd,
                                  uint16_t cmd_id)
{
    uint16_t cq_idx = lduw_le_p(cmd + 4);

    if (cq_idx >= ENA_MAX_IO_QUEUES || !s->io_cq[cq_idx].valid) {
        ena_acq_complete(s, cmd_id, ENA_ADMIN_ILLEGAL_PARAMETER, NULL, 0);
        return;
    }
    s->io_cq[cq_idx].valid = false;
    ena_acq_complete(s, cmd_id, ENA_ADMIN_SUCCESS, NULL, 0);
}

static void ena_handle_admin_cmd(EnaState *s, const uint8_t *cmd)
{
    uint16_t cmd_id = lduw_le_p(cmd) & ENA_ADMIN_AQ_COMMON_DESC_COMMAND_ID_MASK;
    uint8_t opcode = cmd[2];

    switch (opcode) {
    case ENA_ADMIN_GET_FEATURE:
        ena_handle_get_feature(s, cmd, cmd_id);
        break;
    case ENA_ADMIN_SET_FEATURE:
        ena_handle_set_feature(s, cmd, cmd_id);
        break;
    case ENA_ADMIN_CREATE_CQ:
        ena_handle_create_cq(s, cmd, cmd_id);
        break;
    case ENA_ADMIN_CREATE_SQ:
        ena_handle_create_sq(s, cmd, cmd_id);
        break;
    case ENA_ADMIN_DESTROY_SQ:
        ena_handle_destroy_sq(s, cmd, cmd_id);
        break;
    case ENA_ADMIN_DESTROY_CQ:
        ena_handle_destroy_cq(s, cmd, cmd_id);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "ena: unknown admin opcode %u\n", opcode);
        ena_acq_complete(s, cmd_id, ENA_ADMIN_BAD_OPCODE, NULL, 0);
        break;
    }
}

/*
 * AQ doorbell: the driver writes the absolute producer index. Consume every
 * AQ entry between our head and that index, dispatching each command and
 * posting its completion synchronously (polling mode).
 */
static void ena_aq_doorbell(EnaState *s, uint32_t tail)
{
    uint32_t depth = s->aq_caps & ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK;
    dma_addr_t base = ((dma_addr_t)s->aq_base_hi << 32) | s->aq_base_lo;

    if (!depth || !base) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: AQ doorbell before AQ setup\n");
        return;
    }

    while (s->aq_head != tail) {
        uint8_t cmd[ENA_ADMIN_ENTRY_SIZE];
        uint32_t idx = s->aq_head % depth;

        pci_dma_read(PCI_DEVICE(s),
                     base + (dma_addr_t)idx * ENA_ADMIN_ENTRY_SIZE,
                     cmd, sizeof(cmd));
        s->aq_head++;
        ena_handle_admin_cmd(s, cmd);
    }
}

/* ------------------------------------------------------------------ */
/* AENQ — asynchronous event production                               */
/* ------------------------------------------------------------------ */

/* Push one AENQ event of @group with @payload at offset 16, advancing the
 * producer index and flipping the phase bit on wrap (aenq.md §4). */
static void ena_aenq_push(EnaState *s, uint16_t group, const void *payload,
                          size_t payload_len)
{
    uint32_t depth = s->aenq_caps & ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK;
    dma_addr_t base = ((dma_addr_t)s->aenq_base_hi << 32) | s->aenq_base_lo;
    uint8_t entry[ENA_ADMIN_ENTRY_SIZE];
    uint32_t idx;

    if (!depth || !base) {
        return;
    }
    idx = s->aenq_prod % depth;

    memset(entry, 0, sizeof(entry));
    /* aenq_common_desc: group(0), syndrome(2), flags(4), timestamps(8,12). */
    stw_le_p(entry + 0, group);
    entry[4] = s->aenq_phase & ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK;
    if (payload && payload_len) {
        g_assert(payload_len <= sizeof(entry) - 16);
        memcpy(entry + 16, payload, payload_len);
    }

    pci_dma_write(PCI_DEVICE(s), base + (dma_addr_t)idx * ENA_ADMIN_ENTRY_SIZE,
                  entry, sizeof(entry));

    s->aenq_prod++;
    if (s->aenq_prod % depth == 0) {
        s->aenq_phase ^= 1;
    }
}

/*
 * The driver enables the AENQ by writing the head doorbell (publishing all
 * entries as free). On that first write, report the initial link state and a
 * keep-alive heartbeat for the groups the driver enabled (aenq.md §3, §7).
 */
static void ena_aenq_head_db(EnaState *s, uint32_t head)
{
    s->aenq_head_db = head;

    if (s->aenq_started || !head) {
        return;
    }
    s->aenq_started = true;

    if (s->aenq_enabled_groups & (1u << ENA_ADMIN_LINK_CHANGE)) {
        uint8_t lc[4];
        stl_le_p(lc, ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK);
        ena_aenq_push(s, ENA_ADMIN_LINK_CHANGE, lc, sizeof(lc));
    }
    if (s->aenq_enabled_groups & (1u << ENA_ADMIN_KEEP_ALIVE)) {
        ena_aenq_push(s, ENA_ADMIN_KEEP_ALIVE, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* TX datapath (host + LLQ placement, polling)                         */
/* ------------------------------------------------------------------ */

/*
 * Post one TX completion descriptor to CQ @cq_idx: echo @req_id, stamp the
 * CQ's current producer phase and the consumed SQ head, then advance the CQ
 * producer, flipping the phase bit on ring wrap (tx-path.md §4,
 * tx-descriptors.md §6). The 8-byte cdesc is published in a single DMA write so
 * its body is coherent with the phase bit the driver polls on (tx-path.md §5).
 */
static void ena_tx_cq_post(EnaState *s, uint16_t cq_idx, uint16_t sub_qid,
                           uint16_t req_id, uint16_t sq_head)
{
    struct EnaIoCq *cq = &s->io_cq[cq_idx];
    uint8_t cdesc[ENA_TX_CDESC_SIZE];
    uint32_t idx = cq->tail & (cq->depth - 1);

    memset(cdesc, 0, sizeof(cdesc));
    stw_le_p(cdesc + 0, req_id);
    cdesc[2] = 0;                                       /* status: success */
    cdesc[3] = cq->phase & ENA_ETH_IO_TX_CDESC_PHASE_MASK;
    stw_le_p(cdesc + 4, sub_qid);
    stw_le_p(cdesc + 6, sq_head);

    pci_dma_write(PCI_DEVICE(s), cq->base + (dma_addr_t)idx * ENA_TX_CDESC_SIZE,
                  cdesc, sizeof(cdesc));

    cq->tail++;
    if ((cq->tail & (cq->depth - 1)) == 0) {
        cq->phase ^= 1;
    }
}

/*
 * Consume TX SQ @sq_idx up to the latched producer tail (sq->tail, set by the
 * doorbell). Walk the host-memory descriptor ring from the device's head,
 * accumulating each packet from its first data descriptor through the one with
 * LAST set, and post one completion per packet whose first descriptor requested
 * one (tx-path.md §1, §4). req_id and comp_req are taken from the first data
 * descriptor of the packet (tx-descriptors.md §4). Offload meta descriptors
 * (bit 23) are skipped — offloads are out of scope for the core datapath.
 */
static void ena_tx_sq_process(EnaState *s, uint16_t sq_idx)
{
    struct EnaIoSq *sq = &s->io_sq[sq_idx];
    bool in_packet = false, comp_req = false;
    uint16_t req_id = 0;

    while (sq->head != sq->tail) {
        struct ena_eth_io_tx_desc d;
        uint32_t idx = sq->head & (sq->depth - 1);
        uint32_t len_ctrl, meta_ctrl;

        pci_dma_read(PCI_DEVICE(s), sq->base + (dma_addr_t)idx * sizeof(d),
                     &d, sizeof(d));
        sq->head++;

        len_ctrl = le32_to_cpu(d.len_ctrl);
        meta_ctrl = le32_to_cpu(d.meta_ctrl);

        if (len_ctrl & ENA_ETH_IO_TX_DESC_META_DESC_MASK) {
            continue; /* offload meta descriptor — not implemented */
        }
        if (!in_packet) {
            /* First data descriptor: carries comp_req and the split req_id. */
            comp_req = len_ctrl & ENA_ETH_IO_TX_DESC_COMP_REQ_MASK;
            req_id = (meta_ctrl & ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK) >>
                     ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT;
            req_id |= ((len_ctrl & ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK) >>
                       ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT) << 10;
            in_packet = true;
        }
        if (len_ctrl & ENA_ETH_IO_TX_DESC_LAST_MASK) {
            if (comp_req) {
                ena_tx_cq_post(s, sq->cq_idx, sq_idx, req_id, sq->head);
            }
            in_packet = false;
        }
    }
}

/*
 * LLQ variant of ena_tx_sq_process: the descriptor ring lives in device memory
 * (MEM BAR slice), and sq->head / sq->tail are line (entry) indices, not
 * descriptor indices. Each packet starts at a fresh line; the first line holds
 * up to llq_descs_before_header descriptors (16B each) followed by the inline
 * header, and any overflow descriptors spill to the start of subsequent lines
 * (llq_descs_per_entry each). We walk descriptors honouring that layout to find
 * FIRST..LAST, then advance head past the packet's last line (llq.md §6, §9).
 */
static void ena_tx_llq_process(EnaState *s, uint16_t sq_idx)
{
    struct EnaIoSq *sq = &s->io_sq[sq_idx];

    while (sq->head != sq->tail) {
        uint16_t line = sq->head;
        uint8_t slot = 0;
        uint8_t descs_in_line = sq->llq_descs_before_header;
        bool in_packet = false, comp_req = false;
        uint16_t req_id = 0;

        for (;;) {
            struct ena_eth_io_tx_desc d;
            uint32_t off = sq->llq_offset +
                           (uint32_t)(line & (sq->depth - 1)) *
                           sq->llq_entry_size + (uint32_t)slot * sizeof(d);
            uint32_t len_ctrl, meta_ctrl;

            memcpy(&d, s->llq_mem + off, sizeof(d));
            slot++;
            descs_in_line--;

            len_ctrl = le32_to_cpu(d.len_ctrl);
            meta_ctrl = le32_to_cpu(d.meta_ctrl);

            if (len_ctrl & ENA_ETH_IO_TX_DESC_META_DESC_MASK) {
                /* offload meta descriptor — not implemented, but it occupies a
                 * descriptor slot in the line. */
            } else if (!in_packet) {
                comp_req = len_ctrl & ENA_ETH_IO_TX_DESC_COMP_REQ_MASK;
                req_id = (meta_ctrl & ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK) >>
                         ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT;
                req_id |= ((len_ctrl & ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK) >>
                           ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT) << 10;
                in_packet = true;
            }
            if (len_ctrl & ENA_ETH_IO_TX_DESC_LAST_MASK) {
                break;
            }
            if (descs_in_line == 0) {
                line++;
                slot = 0;
                descs_in_line = sq->llq_descs_per_entry;
            }
        }

        sq->head = line + 1; /* the packet closes its current line */
        if (comp_req) {
            ena_tx_cq_post(s, sq->cq_idx, sq_idx, req_id, sq->head);
        }
    }
}

/*
 * Deferred TX task: armed by a TX SQ doorbell, runs out of the MMIO write path.
 * Drain every TX SQ that has descriptors published up to its latched tail
 * (tx-path.md §2). Clearing tx_scheduled first lets a doorbell arriving while
 * we run re-arm the task, so its newly published descriptors get drained on the
 * next run rather than being lost.
 */
static void ena_tx_bh(void *opaque)
{
    EnaState *s = opaque;

    s->tx_scheduled = false;

    for (int idx = 0; idx < ENA_MAX_IO_QUEUES; idx++) {
        struct EnaIoSq *sq = &s->io_sq[idx];

        if (!sq->valid || sq->direction != ENA_ADMIN_SQ_DIRECTION_TX) {
            continue;
        }
        if (sq->llq) {
            ena_tx_llq_process(s, idx);
        } else {
            ena_tx_sq_process(s, idx);
        }
    }
}

/* ------------------------------------------------------------------ */
/* RX datapath (host placement, polling)                               */
/* ------------------------------------------------------------------ */

/*
 * Post one RX completion descriptor to CQ @cq_idx for a consumed buffer: echo
 * @req_id and the per-buffer @length, set FIRST on the packet's first buffer
 * and LAST on its last, stamp the CQ's current producer phase (status bit 24),
 * then advance the CQ producer, flipping the phase bit on ring wrap
 * (rx-path.md §4, rx-descriptors.md). offset/hash/sub_qid are left zero (the
 * driver reads offset as a data offset into the first buffer; we place at 0).
 * The 16-byte cdesc is published in one DMA write so its body is coherent with
 * the phase bit the driver polls on.
 */
static void ena_rx_cq_post(EnaState *s, uint16_t cq_idx, uint16_t req_id,
                           uint16_t length, bool first, bool last)
{
    struct EnaIoCq *cq = &s->io_cq[cq_idx];
    struct ena_eth_io_rx_cdesc_base cdesc;
    uint32_t idx = cq->tail & (cq->depth - 1);
    uint32_t status = (cq->phase & 1) << ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT;

    if (first) {
        status |= ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK;
    }
    if (last) {
        status |= ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK;
    }

    memset(&cdesc, 0, sizeof(cdesc));
    cdesc.status = cpu_to_le32(status);
    cdesc.length = cpu_to_le16(length);
    cdesc.req_id = cpu_to_le16(req_id);

    pci_dma_write(PCI_DEVICE(s), cq->base + (dma_addr_t)idx * ENA_RX_CDESC_SIZE,
                  &cdesc, sizeof(cdesc));

    cq->tail++;
    if ((cq->tail & (cq->depth - 1)) == 0) {
        cq->phase ^= 1;
    }
}

/* Read the RX SQ buffer descriptor at absolute index @pos (rx-path.md §2.1). */
static void ena_rx_read_desc(EnaState *s, struct EnaIoSq *sq, uint16_t pos,
                             struct ena_eth_io_rx_desc *d)
{
    uint32_t idx = pos & (sq->depth - 1);

    pci_dma_read(PCI_DEVICE(s), sq->base + (dma_addr_t)idx * ENA_RX_DESC_SIZE,
                 d, sizeof(*d));
}

static uint32_t ena_rx_desc_capacity(const struct ena_eth_io_rx_desc *d)
{
    uint16_t len = le16_to_cpu(d->length);

    return len ? len : ENA_RX_DESC_LEN_64K;
}

/* Locate the first valid RX SQ, or NULL if none is set up. The datapath tests
 * use a single RX queue; a multi-queue device would steer by RSS here. */
static struct EnaIoSq *ena_rx_find_sq(EnaState *s)
{
    for (int i = 0; i < ENA_MAX_IO_QUEUES; i++) {
        if (s->io_sq[i].valid &&
            s->io_sq[i].direction == ENA_ADMIN_SQ_DIRECTION_RX) {
            return &s->io_sq[i];
        }
    }
    return NULL;
}

/*
 * QEMU network backend RX callback. Scatter one incoming frame across the
 * posted RX SQ buffers (one cdesc per buffer, FIRST..LAST), or DROP it if it
 * cannot be placed — too few posted buffers, insufficient total capacity, or
 * more buffers than the SGL ceiling (rx-path.md §3, §7). A dropped frame writes
 * no cdesc and consumes no buffer; the device stays ready. Returning @size in
 * all cases tells the net layer the frame was consumed (not re-queued).
 *
 * Available buffers span [head, tail): the doorbell latched the driver's
 * absolute SQ tail (rx-path.md §2.2), head is the device's consumer cursor.
 */
static ssize_t ena_rx_receive(NetClientState *nc, const uint8_t *buf,
                              size_t size)
{
    EnaState *s = qemu_get_nic_opaque(nc);
    struct EnaIoSq *sq = ena_rx_find_sq(s);
    struct ena_eth_io_rx_desc descs[ENA_RX_MAX_SGL];
    uint16_t avail, nbufs = 0;
    size_t remaining = size;
    size_t off = 0;
    int i;

    if (!sq) {
        return size; /* no RX queue: drop */
    }
    avail = (uint16_t)sq->tail - sq->head;

    /* Walk posted buffers until the frame is covered, bounded by the posted
     * count and the SGL ceiling. Falling short of @size on either bound means
     * the frame cannot be placed. */
    while (remaining > 0 && nbufs < avail && nbufs < ENA_RX_MAX_SGL) {
        ena_rx_read_desc(s, sq, sq->head + nbufs, &descs[nbufs]);
        remaining -= MIN(remaining, ena_rx_desc_capacity(&descs[nbufs]));
        nbufs++;
    }
    if (remaining > 0) {
        return size; /* cannot place the whole frame: drop */
    }

    /* Place the frame: DMA each chunk into its buffer and post its cdesc. */
    for (i = 0; i < nbufs; i++) {
        uint32_t cap = ena_rx_desc_capacity(&descs[i]);
        size_t chunk = MIN(size - off, cap);
        uint64_t addr = le32_to_cpu(descs[i].buff_addr_lo) |
                        ((uint64_t)le16_to_cpu(descs[i].buff_addr_hi) << 32);

        pci_dma_write(PCI_DEVICE(s), addr, buf + off, chunk);
        ena_rx_cq_post(s, sq->cq_idx, le16_to_cpu(descs[i].req_id), chunk,
                       i == 0, i == nbufs - 1);
        off += chunk;
    }
    sq->head += nbufs;

    return size;
}

/* Accept frames whenever the device is ready; unplaceable frames are dropped
 * in ena_rx_receive (so an out-of-buffers frame is dropped, not re-queued). */
static bool ena_rx_can_receive(NetClientState *nc)
{
    EnaState *s = qemu_get_nic_opaque(nc);

    return s->dev_sts & ENA_REGS_DEV_STS_READY_MASK;
}

static NetClientInfo net_ena_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = ena_rx_can_receive,
    .receive = ena_rx_receive,
};

static void ena_dev_ctl_write(EnaState *s, uint32_t val)
{
    if (val & ENA_REGS_DEV_CTL_DEV_RESET_MASK) {
        ena_cfg_reset(s);
        s->dev_sts = ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK;
    } else {
        s->dev_sts = ENA_REGS_DEV_STS_READY_MASK;
    }
}

static uint64_t ena_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    return ena_reg_read(opaque, addr);
}

static void ena_mmio_write(void *opaque, hwaddr addr, uint64_t val64,
                           unsigned size)
{
    EnaState *s = opaque;
    uint32_t val = val64;

    /* Per-SQ doorbell window (queue-setup.md §4): the driver writes the new SQ
     * tail. For a TX SQ, latch the tail and schedule the deferred task to
     * retrieve and process the published descriptors (tx-path.md §2). */
    if (addr >= ENA_IO_SQ_DB_BASE &&
        addr < ENA_IO_SQ_DB_OFF(ENA_MAX_IO_QUEUES)) {
        uint32_t idx = (addr - ENA_IO_SQ_DB_BASE) / 4;

        if (s->io_sq[idx].valid) {
            s->io_sq[idx].tail = val;
            if (s->io_sq[idx].direction == ENA_ADMIN_SQ_DIRECTION_TX &&
                !s->tx_scheduled) {
                s->tx_scheduled = true;
                qemu_bh_schedule(s->tx_bh);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ena: doorbell to unconfigured SQ %u\n", idx);
        }
        return;
    }

    switch (addr) {
    case ENA_REGS_AQ_BASE_LO_OFF:
        s->aq_base_lo = val;
        break;
    case ENA_REGS_AQ_BASE_HI_OFF:
        s->aq_base_hi = val;
        break;
    case ENA_REGS_AQ_CAPS_OFF:
        s->aq_caps = val;
        break;
    case ENA_REGS_ACQ_BASE_LO_OFF:
        s->acq_base_lo = val;
        break;
    case ENA_REGS_ACQ_BASE_HI_OFF:
        s->acq_base_hi = val;
        break;
    case ENA_REGS_ACQ_CAPS_OFF:
        s->acq_caps = val;
        break;
    case ENA_REGS_AQ_DB_OFF:
        ena_aq_doorbell(s, val);
        break;
    case ENA_REGS_AENQ_CAPS_OFF:
        s->aenq_caps = val;
        break;
    case ENA_REGS_AENQ_BASE_LO_OFF:
        s->aenq_base_lo = val;
        break;
    case ENA_REGS_AENQ_BASE_HI_OFF:
        s->aenq_base_hi = val;
        break;
    case ENA_REGS_AENQ_HEAD_DB_OFF:
        ena_aenq_head_db(s, val);
        break;
    case ENA_REGS_INTR_MASK_OFF:
        s->intr_mask = val;
        break;
    case ENA_REGS_DEV_CTL_OFF:
        ena_dev_ctl_write(s, val);
        break;
    case ENA_REGS_MMIO_REG_READ_OFF:
        ena_mmio_reg_read_req(s, val);
        break;
    case ENA_REGS_MMIO_RESP_LO_OFF:
        s->mmio_resp_lo = val;
        break;
    case ENA_REGS_MMIO_RESP_HI_OFF:
        s->mmio_resp_hi = val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ena: write 0x%x to unimplemented register 0x%"
                      HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps ena_mmio_ops = {
    .read = ena_mmio_read,
    .write = ena_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ena_realize(PCIDevice *pci_dev, Error **errp)
{
    EnaState *s = ENA(pci_dev);

    pci_dev->config[PCI_INTERRUPT_PIN] = 1;

    memory_region_init_io(&s->mmio, OBJECT(s), &ena_mmio_ops, s,
                          "ena-regs", ENA_BAR0_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* BAR2: LLQ device memory. The driver pushes TX descriptor entries here
     * with plain MMIO writes; the device reads them back via s->llq_mem
     * (no DMA). 64-bit prefetchable per device-init.md §1. */
    memory_region_init_ram(&s->mem_bar, OBJECT(s), "ena-llq",
                           ENA_MEM_BAR_SIZE, &error_fatal);
    s->llq_mem = memory_region_get_ram_ptr(&s->mem_bar);
    pci_register_bar(pci_dev, ENA_MEM_BAR_IDX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->mem_bar);

    msix_init_exclusive_bar(pci_dev, ENA_MSIX_VECTORS, ENA_MSIX_BAR_IDX,
                            errp);

    s->tx_bh = qemu_bh_new_guarded(ena_tx_bh, s,
                                   &DEVICE(s)->mem_reentrancy_guard);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_ena_info, &s->conf,
                          object_get_typename(OBJECT(s)), DEVICE(s)->id,
                          &DEVICE(s)->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void ena_exit(PCIDevice *pci_dev)
{
    EnaState *s = ENA(pci_dev);

    qemu_bh_delete(s->tx_bh);
    qemu_del_nic(s->nic);
    msix_uninit_exclusive_bar(pci_dev);
}

static void ena_reset_hold(Object *obj, ResetType type)
{
    EnaState *s = ENA(obj);

    ena_cfg_reset(s);
    s->dev_sts = ENA_REGS_DEV_STS_READY_MASK;
}

static const Property ena_properties[] = {
    DEFINE_NIC_PROPERTIES(EnaState, conf),
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
    rc->phases.hold = ena_reset_hold;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "Amazon Elastic Network Adapter (stub)";
    device_class_set_props(dc, ena_properties);
}

static const TypeInfo ena_info = {
    .name          = TYPE_ENA,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EnaState),
    .class_init    = ena_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ena_register_types(void)
{
    type_register_static(&ena_info);
}

type_init(ena_register_types)
