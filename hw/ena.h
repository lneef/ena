/*
 * QEMU Amazon Elastic Network Adapter (ENA) emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ENA_H
#define HW_ENA_H

#include "hw/pci/pci_device.h"
#include "net/net.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/ena_defs/ena_defs.h"

#define TYPE_ENA "ena"
OBJECT_DECLARE_SIMPLE_TYPE(EnaState, ENA)

#define ENA_PCI_VENDOR_ID           0x1d0f
#define ENA_PCI_DEVICE_ID           0xec20

/* Device limits */
#define ENA_MAX_IO_QUEUES           8
#define ENA_MAX_SQ                  (2 * ENA_MAX_IO_QUEUES)
#define ENA_MAX_CQ                  (2 * ENA_MAX_IO_QUEUES)
#define ENA_MSIX_VECTORS            (1 + ENA_MAX_IO_QUEUES)
#define ENA_ADMIN_MSIX_VECTOR       0
#define ENA_MAX_QUEUE_DEPTH         1024
#define ENA_MAX_CQ_ENTRY_SIZE       32
#define ENA_MIN_QUEUE_DEPTH         16
#define ENA_MAX_TX_HEADER_SIZE      96
#define ENA_MAX_PKT_DESCS           17
#define ENA_MAX_MTU                 9000
#define ENA_MIN_MTU                 128
#define ENA_DMA_ADDR_WIDTH          48
#define ENA_RSS_IND_TBL_LOG_SIZE    7
#define ENA_RSS_IND_TBL_SIZE        (1 << ENA_RSS_IND_TBL_LOG_SIZE)
#define ENA_LINK_SPEED_MBPS         10000
#define ENA_KEEP_ALIVE_INTERVAL_MS  1000
/* host_info.os_type of the DPDK PMD, the only supported driver */
#define ENA_ADMIN_OS_DPDK           3

/* BAR layout */
#define ENA_REG_BAR                 0
#define ENA_MEM_BAR                 2
#define ENA_MSIX_BAR                4
#define ENA_REG_BAR_SIZE            0x4000
#define ENA_REG_FILE_SIZE           0x70
#define ENA_REG_COUNT               (ENA_REG_FILE_SIZE / 4)
#define ENA_REG_SQ_DB_BASE          0x1000
#define ENA_REG_CQ_UNMASK_BASE      0x2000

/* LLQ (device placement) memory: one fixed slice of BAR2 per SQ index */
#define ENA_LLQ_ENTRY_SIZE          128
#define ENA_LLQ_LARGE_ENTRY_SIZE    256
#define ENA_LLQ_DESCS_BEFORE_HEADER 2
#define ENA_LLQ_QUEUE_BYTES         (ENA_MAX_QUEUE_DEPTH * ENA_LLQ_ENTRY_SIZE)
#define ENA_MEM_BAR_SIZE            (ENA_MAX_SQ * ENA_LLQ_QUEUE_BYTES)

/* Register values */
#define ENA_VERSION_MAJOR           2
#define ENA_VERSION_MINOR           0
#define ENA_CTRL_VERSION_MAJOR      0
#define ENA_CTRL_VERSION_MINOR      0
#define ENA_CTRL_VERSION_SUBMINOR   1
#define ENA_CTRL_VERSION_IMPL_ID    1
#define ENA_CAPS_RESET_TIMEOUT      10   /* units of 100 ms */
#define ENA_CAPS_ADMIN_CMD_TO       0    /* 0: driver default */

typedef struct EnaCq {
    bool used;
    uint64_t base;
    uint16_t depth;
    uint8_t entry_size;
    bool intr_enabled;
    uint32_t msix_vector;
    uint16_t tail;
    bool phase;
} EnaCq;

/* Interrupt state of one MSI-X vector, shared by the CQs bound to it. */
typedef struct EnaIrq {
    bool unmasked;
    bool rx_pending;
    bool tx_pending;
    uint32_t rx_delay_us;
    uint32_t tx_delay_us;
    QEMUTimer *moder_timer;
    uint32_t vector;
    EnaState *s;
} EnaIrq;

typedef struct EnaTxMeta {
    uint16_t mss;
    uint8_t l3_hdr_len;
    uint8_t l3_hdr_off;
    uint8_t l4_hdr_len_words;
} EnaTxMeta;

typedef struct EnaSq {
    bool used;
    bool is_tx;
    bool llq;
    uint16_t cq_idx;
    uint64_t base;
    uint16_t depth;
    uint16_t entry_size;
    uint16_t head;
    uint16_t tail;
    EnaTxMeta meta;
} EnaSq;

typedef struct EnaRss {
    uint32_t key[ENA_ADMIN_RSS_KEY_PARTS];
    uint32_t key_parts;
    uint8_t func;
    uint32_t init_val;
    uint16_t input_sort;
    uint16_t fields[ENA_ADMIN_RSS_PROTO_NUM];
    uint16_t ind_tbl[ENA_RSS_IND_TBL_SIZE];
} EnaRss;

struct EnaState {
    PCIDevice parent_obj;

    NICState *nic;
    NICConf conf;
    MemoryRegion regs_mr;
    MemoryRegion llq_mr;

    uint32_t reg[ENA_REG_COUNT];

    /* admin queue */
    uint16_t aq_head;
    uint16_t acq_tail;
    bool acq_phase;

    /* async event notification queue */
    uint16_t aenq_tail;
    bool aenq_phase;
    uint32_t aenq_groups;
    QEMUTimer *keep_alive_timer;

    /* negotiated features */
    uint32_t mtu;
    uint64_t host_info_addr;
    uint64_t debug_area_addr;
    uint32_t debug_area_size;
    bool llq_enabled;
    uint16_t llq_entry_size;
    bool llq_large;
    EnaRss rss;

    EnaSq sq[ENA_MAX_SQ];
    EnaCq cq[ENA_MAX_CQ];
    EnaIrq irq[ENA_MSIX_VECTORS];

    struct NetTxPkt *tx_pkt;
    struct NetRxPkt *rx_pkt;

    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t rx_drops;
};

/* ena.c: shared helpers */
uint64_t ena_mem_addr(const struct ena_common_mem_addr *addr);
void ena_dma_read(EnaState *s, uint64_t addr, void *buf, size_t len);
void ena_dma_write(EnaState *s, uint64_t addr, const void *buf, size_t len);
uint8_t *ena_llq_mem(EnaState *s, const EnaSq *sq);
/* Writes one completion entry with the current phase already set by the caller. */
void ena_cq_push(EnaState *s, EnaCq *cq, const void *cdesc);
void ena_cq_intr(EnaState *s, EnaCq *cq, bool is_tx);
void ena_aenq_post(EnaState *s, uint16_t group, uint16_t syndrome,
                   const void *data, size_t len);
void ena_aenq_config(EnaState *s, uint32_t groups);
void ena_stats_reset(EnaState *s);

/* ena_admin.c */
void ena_admin_process(EnaState *s);

/* ena_tx.c */
void ena_tx_doorbell(EnaState *s, EnaSq *sq);

/* ena_rx.c */
void ena_rx_doorbell(EnaState *s, EnaSq *sq);
bool ena_rx_can_receive(EnaState *s);
ssize_t ena_rx_receive_iov(EnaState *s, const struct iovec *iov, int iovcnt);

#endif
