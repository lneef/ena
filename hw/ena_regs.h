/*
 * ENA register map (PCI BAR0 MMIO).
 *
 * Offsets, masks and shifts per docs/wiki/registers.md, taken verbatim
 * from the reference ena_regs_defs.h. All registers are 32-bit; 64-bit
 * addresses are split into LO/HI pairs.
 */
#ifndef ENA_REGS_H
#define ENA_REGS_H

#include <stdint.h>

/* Register offsets (byte offsets from start of BAR0) */
#define ENA_REGS_VERSION_OFF                0x00 /* RO */
#define ENA_REGS_CONTROLLER_VERSION_OFF     0x04 /* RO */
#define ENA_REGS_CAPS_OFF                   0x08 /* RO */
#define ENA_REGS_CAPS_EXT_OFF               0x0c /* RO, unused by com layer */
#define ENA_REGS_AQ_BASE_LO_OFF             0x10 /* WO */
#define ENA_REGS_AQ_BASE_HI_OFF             0x14 /* WO */
#define ENA_REGS_AQ_CAPS_OFF                0x18 /* WO */
#define ENA_REGS_ACQ_BASE_LO_OFF            0x20 /* WO */
#define ENA_REGS_ACQ_BASE_HI_OFF            0x24 /* WO */
#define ENA_REGS_ACQ_CAPS_OFF               0x28 /* WO */
#define ENA_REGS_AQ_DB_OFF                  0x2c /* WO doorbell */
#define ENA_REGS_ACQ_TAIL_OFF               0x30 /* unused by com layer */
#define ENA_REGS_AENQ_CAPS_OFF              0x34 /* WO */
#define ENA_REGS_AENQ_BASE_LO_OFF           0x38 /* WO */
#define ENA_REGS_AENQ_BASE_HI_OFF           0x3c /* WO */
#define ENA_REGS_AENQ_HEAD_DB_OFF           0x40 /* WO doorbell */
#define ENA_REGS_AENQ_TAIL_OFF              0x44 /* unused by com layer */
#define ENA_REGS_INTR_MASK_OFF              0x4c /* WO */
#define ENA_REGS_DEV_CTL_OFF                0x54 /* WO */
#define ENA_REGS_DEV_STS_OFF                0x58 /* RO */
#define ENA_REGS_MMIO_REG_READ_OFF          0x5c /* WO doorbell */
#define ENA_REGS_MMIO_RESP_LO_OFF           0x60 /* WO */
#define ENA_REGS_MMIO_RESP_HI_OFF           0x64 /* WO */
#define ENA_REGS_RSS_IND_ENTRY_UPDATE_OFF   0x68 /* unused by com layer */
#define ENA_REGS_PHC_DB_OFF                 0x100 /* WO doorbell */

/* VERSION (0x00) */
#define ENA_REGS_VERSION_MINOR_VERSION_MASK             0x000000ff
#define ENA_REGS_VERSION_MAJOR_VERSION_SHIFT            8
#define ENA_REGS_VERSION_MAJOR_VERSION_MASK             0x0000ff00

/* CONTROLLER_VERSION (0x04) */
#define ENA_REGS_CONTROLLER_VERSION_SUBMINOR_VERSION_MASK   0x000000ff
#define ENA_REGS_CONTROLLER_VERSION_MINOR_VERSION_SHIFT     8
#define ENA_REGS_CONTROLLER_VERSION_MINOR_VERSION_MASK      0x0000ff00
#define ENA_REGS_CONTROLLER_VERSION_MAJOR_VERSION_SHIFT     16
#define ENA_REGS_CONTROLLER_VERSION_MAJOR_VERSION_MASK      0x00ff0000
#define ENA_REGS_CONTROLLER_VERSION_IMPL_ID_SHIFT           24
#define ENA_REGS_CONTROLLER_VERSION_IMPL_ID_MASK            0xff000000

/* CAPS (0x08); RESET_TIMEOUT and ADMIN_CMD_TO unit is 100 ms */
#define ENA_REGS_CAPS_CONTIGUOUS_QUEUE_REQUIRED_MASK    0x00000001
#define ENA_REGS_CAPS_RESET_TIMEOUT_SHIFT               1
#define ENA_REGS_CAPS_RESET_TIMEOUT_MASK                0x0000003e
#define ENA_REGS_CAPS_DMA_ADDR_WIDTH_SHIFT              8
#define ENA_REGS_CAPS_DMA_ADDR_WIDTH_MASK               0x0000ff00
#define ENA_REGS_CAPS_ADMIN_CMD_TO_SHIFT                16
#define ENA_REGS_CAPS_ADMIN_CMD_TO_MASK                 0x000f0000

/* AQ_CAPS (0x18) */
#define ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK                  0x0000ffff
#define ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_SHIFT            16
#define ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_MASK             0xffff0000

/* ACQ_CAPS (0x28) */
#define ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK                0x0000ffff
#define ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_SHIFT          16
#define ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_MASK           0xffff0000

/* AENQ_CAPS (0x34) */
#define ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK              0x0000ffff
#define ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_SHIFT        16
#define ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_MASK         0xffff0000

/* INTR_MASK (0x4c): bit 0 set = admin/AENQ interrupt masked */
#define ENA_REGS_INTR_MASK_INTR_MASK                    0x00000001

/* DEV_CTL (0x54) */
#define ENA_REGS_DEV_CTL_DEV_RESET_MASK                 0x00000001
#define ENA_REGS_DEV_CTL_AQ_RESTART_SHIFT               1
#define ENA_REGS_DEV_CTL_AQ_RESTART_MASK                0x00000002
#define ENA_REGS_DEV_CTL_QUIESCENT_SHIFT                2
#define ENA_REGS_DEV_CTL_QUIESCENT_MASK                 0x00000004
#define ENA_REGS_DEV_CTL_IO_RESUME_SHIFT                3
#define ENA_REGS_DEV_CTL_IO_RESUME_MASK                 0x00000008
#define ENA_REGS_DEV_CTL_RESET_REASON_EXT_SHIFT         24
#define ENA_REGS_DEV_CTL_RESET_REASON_EXT_MASK          0x0f000000
#define ENA_REGS_DEV_CTL_RESET_REASON_SHIFT             28
#define ENA_REGS_DEV_CTL_RESET_REASON_MASK              0xf0000000

/* DEV_STS (0x58) */
#define ENA_REGS_DEV_STS_READY_MASK                         0x00000001
#define ENA_REGS_DEV_STS_AQ_RESTART_IN_PROGRESS_MASK        0x00000002
#define ENA_REGS_DEV_STS_AQ_RESTART_FINISHED_MASK           0x00000004
#define ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK             0x00000008
#define ENA_REGS_DEV_STS_RESET_FINISHED_MASK                0x00000010
#define ENA_REGS_DEV_STS_FATAL_ERROR_MASK                   0x00000020
#define ENA_REGS_DEV_STS_QUIESCENT_STATE_IN_PROGRESS_MASK   0x00000040
#define ENA_REGS_DEV_STS_QUIESCENT_STATE_ACHIEVED_MASK      0x00000080

/* MMIO_REG_READ (0x5c) */
#define ENA_REGS_MMIO_REG_READ_REQ_ID_MASK              0x0000ffff
#define ENA_REGS_MMIO_REG_READ_REG_OFF_SHIFT            16
#define ENA_REGS_MMIO_REG_READ_REG_OFF_MASK             0xffff0000

/* RSS_IND_ENTRY_UPDATE (0x68) */
#define ENA_REGS_RSS_IND_ENTRY_UPDATE_INDEX_MASK        0x0000ffff
#define ENA_REGS_RSS_IND_ENTRY_UPDATE_CQ_IDX_SHIFT      16
#define ENA_REGS_RSS_IND_ENTRY_UPDATE_CQ_IDX_MASK       0xffff0000

/* PHC_DB (0x100) */
#define ENA_REGS_PHC_DB_REQ_ID_MASK                     0x0000ffff

/* DEV_CTL reset reason values; 0..15 fit the legacy RESET_REASON nibble,
 * 16..20 additionally need RESET_REASON_EXT (EXTENDED_RESET_REASONS cap). */
enum ena_regs_reset_reason_types {
    ENA_REGS_RESET_NORMAL                       = 0,
    ENA_REGS_RESET_KEEP_ALIVE_TO                = 1,
    ENA_REGS_RESET_ADMIN_TO                     = 2,
    ENA_REGS_RESET_MISS_TX_CMPL                 = 3,
    ENA_REGS_RESET_INV_RX_REQ_ID                = 4,
    ENA_REGS_RESET_INV_TX_REQ_ID                = 5,
    ENA_REGS_RESET_TOO_MANY_RX_DESCS            = 6,
    ENA_REGS_RESET_INIT_ERR                     = 7,
    ENA_REGS_RESET_DRIVER_INVALID_STATE         = 8,
    ENA_REGS_RESET_OS_TRIGGER                   = 9,
    ENA_REGS_RESET_OS_NETDEV_WD                 = 10,
    ENA_REGS_RESET_SHUTDOWN                     = 11,
    ENA_REGS_RESET_USER_TRIGGER                 = 12,
    ENA_REGS_RESET_GENERIC                      = 13,
    ENA_REGS_RESET_MISS_INTERRUPT               = 14,
    ENA_REGS_RESET_SUSPECTED_POLL_STARVATION    = 15,
    ENA_REGS_RESET_RX_DESCRIPTOR_MALFORMED      = 16,
    ENA_REGS_RESET_TX_DESCRIPTOR_MALFORMED      = 17,
    ENA_REGS_RESET_MISSING_ADMIN_INTERRUPT      = 18,
    ENA_REGS_RESET_DEVICE_REQUEST               = 19,
    ENA_REGS_RESET_MISS_FIRST_INTERRUPT         = 20,
};

/* Indirect (readless) MMIO read: 8-byte host response buffer the device
 * DMA-writes after a MMIO_REG_READ doorbell. The driver polls req_id and
 * also verifies the echoed reg_off. */
struct ena_mmio_read_less_resp {
    uint16_t req_id;
    uint16_t reg_off;
    uint32_t reg_val;
};

/* Driver-side constants relevant to register behavior */
#define ENA_MMIO_READ_TIMEOUT_SENTINEL  0xffffffff /* returned on failed read */
#define ENA_MAX_PHYS_ADDR_SIZE_BITS     48
#define ENA_MIN_CTRL_VER                0x000001   /* controller >= 0.0.1 */

#endif /* ENA_REGS_H */
