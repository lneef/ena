/*
 * ENA control-path descriptors: admin submission/completion queue (AQ/ACQ)
 * entries and the asynchronous event notification queue (AENQ) entries.
 *
 * Layouts per docs/wiki/admin-queue.md and docs/wiki/aenq.md, taken from
 * the reference ena_admin_defs.h / ena_common_defs.h. Both admin rings use
 * 64-byte entries; the driver programs depth 32 (AQ/ACQ) and 16 (AENQ)
 * through the *_CAPS registers (see hw/ena_regs.h).
 */
#ifndef ENA_ADMIN_DESC_H
#define ENA_ADMIN_DESC_H

#include <stdint.h>

/* 48-bit DMA address as used in all admin payloads */
struct ena_common_mem_addr {
    uint32_t mem_addr_low;
    uint16_t mem_addr_high;
    uint16_t reserved16;
};

/* ------------------------------------------------------------------ */
/* Admin submission queue (AQ)                                        */
/* ------------------------------------------------------------------ */

enum ena_admin_aq_opcode {
    ENA_ADMIN_CREATE_SQ     = 1,
    ENA_ADMIN_DESTROY_SQ    = 2,
    ENA_ADMIN_CREATE_CQ     = 3,
    ENA_ADMIN_DESTROY_CQ    = 4,
    ENA_ADMIN_GET_FEATURE   = 8,
    ENA_ADMIN_SET_FEATURE   = 9,
    ENA_ADMIN_GET_STATS     = 11,
};

struct ena_admin_aq_common_desc {
    uint16_t command_id; /* bits 11:0; 15:12 reserved */
    uint8_t  opcode;     /* enum ena_admin_aq_opcode */
    uint8_t  flags;
};

#define ENA_ADMIN_AQ_COMMON_DESC_COMMAND_ID_MASK        0x0fff
/* aq_common_desc.flags */
#define ENA_ADMIN_AQ_COMMON_DESC_PHASE_MASK             0x01
#define ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_SHIFT        1
#define ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_MASK         0x02
#define ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_SHIFT 2
#define ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK 0x04

/* Out-of-line payload pointer; sits in the AQ entry's first union slot
 * when ctrl_data/ctrl_data_indirect is set (the reference driver always
 * uses the indirect mode). */
struct ena_admin_ctrl_buff_info {
    uint32_t length;
    struct ena_common_mem_addr address;
};

struct ena_admin_aq_entry {
    struct ena_admin_aq_common_desc aq_common_descriptor;
    union {
        uint32_t inline_data_w1[3];
        struct ena_admin_ctrl_buff_info control_buffer;
    } u;
    uint32_t inline_data_w4[12];
};

/* ------------------------------------------------------------------ */
/* Admin completion queue (ACQ)                                       */
/* ------------------------------------------------------------------ */

enum ena_admin_aq_completion_status {
    ENA_ADMIN_SUCCESS                       = 0,
    ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE   = 1,
    ENA_ADMIN_BAD_OPCODE                    = 2,
    ENA_ADMIN_UNSUPPORTED_OPCODE            = 3,
    ENA_ADMIN_MALFORMED_REQUEST             = 4,
    /* additional status in acq_common_desc.extended_status */
    ENA_ADMIN_ILLEGAL_PARAMETER             = 5,
    ENA_ADMIN_UNKNOWN_ERROR                 = 6,
    ENA_ADMIN_RESOURCE_BUSY                 = 7,
};

struct ena_admin_acq_common_desc {
    uint16_t command;         /* bits 11:0 echo the AQ command_id */
    uint8_t  status;          /* enum ena_admin_aq_completion_status */
    uint8_t  flags;           /* bit0 phase; 7:1 reserved */
    uint16_t extended_status;
    uint16_t sq_head_indx;    /* last AQ entry consumed by the device */
};

#define ENA_ADMIN_ACQ_COMMON_DESC_COMMAND_ID_MASK       0x0fff
#define ENA_ADMIN_ACQ_COMMON_DESC_PHASE_MASK            0x01

struct ena_admin_acq_entry {
    struct ena_admin_acq_common_desc acq_common_descriptor;
    uint32_t response_specific_data[14];
};

/* ------------------------------------------------------------------ */
/* GET_FEATURE / SET_FEATURE framing                                  */
/* ------------------------------------------------------------------ */

enum ena_admin_aq_feature_id {
    ENA_ADMIN_DEVICE_ATTRIBUTES             = 1,
    ENA_ADMIN_MAX_QUEUES_NUM                = 2,
    ENA_ADMIN_HW_HINTS                      = 3,
    ENA_ADMIN_LLQ                           = 4,
    ENA_ADMIN_EXTRA_PROPERTIES_STRINGS      = 5,
    ENA_ADMIN_EXTRA_PROPERTIES_FLAGS        = 6,
    ENA_ADMIN_MAX_QUEUES_EXT                = 7,
    ENA_ADMIN_RSS_HASH_FUNCTION             = 10,
    ENA_ADMIN_STATELESS_OFFLOAD_CONFIG      = 11,
    ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG  = 12,
    ENA_ADMIN_MTU                           = 14,
    ENA_ADMIN_RSS_HASH_INPUT                = 18,
    ENA_ADMIN_INTERRUPT_MODERATION          = 20,
    ENA_ADMIN_AENQ_CONFIG                   = 26,
    ENA_ADMIN_LINK_CONFIG                   = 27,
    ENA_ADMIN_HOST_ATTR_CONFIG              = 28,
    ENA_ADMIN_PHC_CONFIG                    = 29,
    ENA_ADMIN_FEATURES_OPCODE_NUM           = 32,
};

/* capabilities bitmap in the DEVICE_ATTRIBUTES response (distinct from
 * both the CAPS register and the supported_features bitmap) */
enum ena_admin_aq_caps_id {
    ENA_ADMIN_ENI_STATS                 = 0,
    ENA_ADMIN_ENA_SRD_INFO              = 1,
    ENA_ADMIN_CUSTOMER_METRICS          = 2,
    ENA_ADMIN_EXTENDED_RESET_REASONS    = 3,
    ENA_ADMIN_CDESC_MBZ                 = 4,
};

struct ena_admin_get_set_feature_common_desc {
    uint8_t flags;            /* bits 1:0 select: 0x1 current, 0x3 default */
    uint8_t feature_id;       /* enum ena_admin_aq_feature_id */
    uint8_t feature_version;  /* driver sends max supported; device echoes
                                 the version it actually serves */
    uint8_t reserved8;
};

#define ENA_ADMIN_GET_SET_FEATURE_COMMON_DESC_SELECT_MASK 0x03

/* ------------------------------------------------------------------ */
/* AENQ — asynchronous event notification queue                       */
/* ------------------------------------------------------------------ */

enum ena_admin_aenq_group {
    ENA_ADMIN_LINK_CHANGE           = 0,
    ENA_ADMIN_FATAL_ERROR           = 1,
    ENA_ADMIN_WARNING               = 2,
    ENA_ADMIN_NOTIFICATION          = 3,
    ENA_ADMIN_KEEP_ALIVE            = 4,
    ENA_ADMIN_REFRESH_CAPABILITIES  = 5,
    ENA_ADMIN_CONF_NOTIFICATIONS    = 6,
    ENA_ADMIN_DEVICE_REQUEST_RESET  = 7,
    ENA_ADMIN_AENQ_GROUPS_NUM       = 8,
};

enum ena_admin_aenq_notification_syndrome {
    ENA_ADMIN_UPDATE_HINTS = 2,
};

struct ena_admin_aenq_common_desc {
    uint16_t group;     /* enum ena_admin_aenq_group */
    uint16_t syndrome;  /* group-specific sub-code */
    uint8_t  flags;     /* bit0 phase; 7:1 reserved MBZ */
    uint8_t  reserved1[3];
    uint32_t timestamp_low;
    uint32_t timestamp_high;
};

#define ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK   0x01

struct ena_admin_aenq_entry {
    struct ena_admin_aenq_common_desc aenq_common_desc;
    uint32_t inline_data_w4[12];
};

/* Group-specific payloads (overlaying the entry from offset 16) */

struct ena_admin_aenq_link_change_desc {
    struct ena_admin_aenq_common_desc aenq_common_desc;
    uint32_t flags; /* bit0 = link_status, 1 = up */
};

#define ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK 0x01

/* counters are cumulative since last device reset, high<<32 | low */
struct ena_admin_aenq_keep_alive_desc {
    struct ena_admin_aenq_common_desc aenq_common_desc;
    uint32_t rx_drops_low;
    uint32_t rx_drops_high;
    uint32_t tx_drops_low;
    uint32_t tx_drops_high;
    uint32_t rx_overruns_low;
    uint32_t rx_overruns_high;
};

struct ena_admin_aenq_conf_notifications_desc {
    struct ena_admin_aenq_common_desc aenq_common_desc;
    uint64_t notifications_bitmap; /* bit N reported as config code N+1 */
    uint64_t reserved;
};

/* NOTIFICATION / UPDATE_HINTS payload (overlays inline_data_w4);
 * timeouts in ms, 0xFFFF = no timeout */
struct ena_admin_ena_hw_hints {
    uint16_t mmio_read_timeout;
    uint16_t driver_watchdog_timeout;
    uint16_t missing_tx_completion_timeout;
    uint16_t missed_tx_completion_count_threshold_to_reset;
    uint16_t admin_completion_tx_timeout;
    uint16_t netdev_wd_timeout;
    uint16_t max_tx_sgl_size;
    uint16_t max_rx_sgl_size;
    uint16_t reserved[8];
};

#endif /* ENA_ADMIN_DESC_H */
