/*
 * Shared IO-queue (CREATE/DESTROY SQ & CQ) helpers for ENA datapath tests.
 *
 * Builds on ena_test_common.h. Provides the admin command/response structs
 * for CREATE_CQ, CREATE_SQ, DESTROY_SQ and DESTROY_CQ, plus convenience
 * helpers that create a host-placement (non-LLQ) IO queue pair and tear it
 * down. Layouts and ordering per docs/wiki/queue-setup.md. LLQ (DEV
 * placement) specifics live in the LLQ test file (docs/wiki/llq.md).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#ifndef TESTS_ENA_TEST_QUEUE_H
#define TESTS_ENA_TEST_QUEUE_H

#include "ena_test_common.h"

/* SQ direction (sq_identity bits 7:5). */
#define ENA_ADMIN_SQ_DIRECTION_SHIFT    5
#define ENA_ADMIN_SQ_DIRECTION_TX       1
#define ENA_ADMIN_SQ_DIRECTION_RX       2

/* Placement policy (sq_caps_2 bits 3:0). */
#define ENA_ADMIN_SQ_PLACEMENT_HOST     1   /* descriptors in host memory */
#define ENA_ADMIN_SQ_PLACEMENT_DEV      3   /* descriptors in device mem (LLQ) */

/* Completion policy (sq_caps_2 bits 6:4). */
#define ENA_ADMIN_SQ_COMPLETION_DESC    0   /* one CQE per SQ descriptor */
#define ENA_ADMIN_SQ_COMPLETION_POLICY_SHIFT 4

/* sq_caps_3 bit0 */
#define ENA_ADMIN_SQ_IS_PHYS_CONTIG     0x01

/* cq_caps_1 bit5 */
#define ENA_ADMIN_CQ_INTERRUPT_MODE_ENABLED 0x20
/* cq_caps_2 bits4:0 */
#define ENA_ADMIN_CQ_ENTRY_SIZE_WORDS_MASK  0x1f

/* CDESC entry sizes, words = bytes / 4. */
#define ENA_TX_CDESC_SIZE   8   /* sizeof(ena_eth_io_tx_cdesc) */
#define ENA_RX_CDESC_SIZE   16  /* sizeof(ena_eth_io_rx_cdesc_base) */
#define ENA_TX_DESC_SIZE    16  /* sizeof(ena_eth_io_tx_desc) */
#define ENA_RX_DESC_SIZE    16  /* sizeof(ena_eth_io_rx_desc) */

/* ------------------------------------------------------------------ */
/* Command / response structs (queue-setup.md §3-§6)                   */
/* ------------------------------------------------------------------ */

typedef struct QEMU_PACKED ENACreateCqCmd {
    struct ena_admin_aq_common_desc common;
    uint8_t  cq_caps_1;
    uint8_t  cq_caps_2;
    uint16_t cq_depth;
    uint32_t msix_vector;
    struct ena_common_mem_addr cq_ba;
} ENACreateCqCmd;

typedef struct QEMU_PACKED ENACreateCqResp {
    struct ena_admin_acq_common_desc acq_common;
    uint16_t cq_idx;
    uint16_t cq_actual_depth;
    uint32_t numa_node_register_offset;
    uint32_t cq_head_db_register_offset;
    uint32_t cq_interrupt_unmask_register_offset;
} ENACreateCqResp;

typedef struct QEMU_PACKED ENACreateSqCmd {
    struct ena_admin_aq_common_desc common;
    uint8_t  sq_identity;
    uint8_t  reserved8_w1;
    uint8_t  sq_caps_2;
    uint8_t  sq_caps_3;
    uint16_t cq_idx;
    uint16_t sq_depth;
    struct ena_common_mem_addr sq_ba;
    struct ena_common_mem_addr sq_head_writeback;
    uint32_t reserved0_w7;
    uint32_t reserved0_w8;
} ENACreateSqCmd;

typedef struct QEMU_PACKED ENACreateSqResp {
    struct ena_admin_acq_common_desc acq_common;
    uint16_t sq_idx;
    uint16_t reserved;
    uint32_t sq_doorbell_offset;
    uint32_t llq_descriptors_offset;
    uint32_t llq_headers_offset;
} ENACreateSqResp;

typedef struct QEMU_PACKED ENAAdminSq {
    uint16_t sq_idx;
    uint8_t  sq_identity;
    uint8_t  reserved1;
} ENAAdminSq;

typedef struct QEMU_PACKED ENADestroySqCmd {
    struct ena_admin_aq_common_desc common;
    ENAAdminSq sq;
} ENADestroySqCmd;

typedef struct QEMU_PACKED ENADestroyCqCmd {
    struct ena_admin_aq_common_desc common;
    uint16_t cq_idx;
    uint16_t reserved1;
} ENADestroyCqCmd;

/* Driver-side handle for one created IO queue pair (single direction). */
typedef struct ENAQueue {
    uint8_t  direction;         /* ENA_ADMIN_SQ_DIRECTION_TX / _RX */
    uint16_t depth;             /* ring depth (power of 2) */

    uint64_t cq_base;           /* CQ cdesc ring (guest phys) */
    uint64_t sq_base;           /* SQ desc ring (guest phys, HOST placement) */

    uint16_t cq_idx;            /* device-assigned CQ index */
    uint16_t sq_idx;            /* device-assigned SQ index */
    uint32_t sq_doorbell_off;   /* SQ doorbell offset into BAR0 */

    /* LLQ (DEV placement) state; llq=false for host placement. */
    bool     llq;
    uint32_t llq_desc_off;      /* llq_descriptors_offset into BAR2 (MEM BAR) */
    uint16_t llq_entry_size;    /* bytes per pushed LLQ entry/line */
    uint8_t  llq_descs_before_header;
    uint8_t  llq_descs_per_entry;

    /* Ring tracking, mirroring the driver (queue-setup.md §8). For LLQ,
     * sq_tail counts entries/lines rather than descriptors. */
    uint16_t sq_tail;
    uint8_t  sq_phase;
    uint16_t cq_head;
    uint8_t  cq_phase;
} ENAQueue;

/* ------------------------------------------------------------------ */
/* Low-level create / destroy                                          */
/* ------------------------------------------------------------------ */

/* CREATE_CQ for @cdesc_size-byte entries over @depth slots at @cq_ba. */
static inline uint8_t ena_try_create_cq(ENATestCtx *t, uint16_t depth,
                                        uint32_t cdesc_size, uint64_t cq_ba,
                                        ENACreateCqResp *resp)
{
    ENACreateCqCmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.common.opcode = ENA_ADMIN_CREATE_CQ;
    cmd.cq_caps_1 = ENA_ADMIN_CQ_INTERRUPT_MODE_ENABLED;
    cmd.cq_caps_2 = (cdesc_size / 4) & ENA_ADMIN_CQ_ENTRY_SIZE_WORDS_MASK;
    cmd.cq_depth = cpu_to_le16(depth);
    cmd.msix_vector = cpu_to_le32(0xffffffff); /* -1: no datapath interrupt */
    ena_set_mem_addr(&cmd.cq_ba, cq_ba);

    return ena_admin_try(t, &cmd, sizeof(cmd), resp, sizeof(*resp));
}

/* CREATE_SQ (host placement) feeding @cq_idx. */
static inline uint8_t ena_try_create_sq_host(ENATestCtx *t, uint8_t direction,
                                             uint16_t cq_idx, uint16_t depth,
                                             uint64_t sq_ba,
                                             ENACreateSqResp *resp)
{
    ENACreateSqCmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.common.opcode = ENA_ADMIN_CREATE_SQ;
    cmd.sq_identity = direction << ENA_ADMIN_SQ_DIRECTION_SHIFT;
    cmd.sq_caps_2 = (ENA_ADMIN_SQ_PLACEMENT_HOST & 0x0f) |
                    (ENA_ADMIN_SQ_COMPLETION_DESC <<
                     ENA_ADMIN_SQ_COMPLETION_POLICY_SHIFT);
    cmd.sq_caps_3 = ENA_ADMIN_SQ_IS_PHYS_CONTIG;
    cmd.cq_idx = cpu_to_le16(cq_idx);
    cmd.sq_depth = cpu_to_le16(depth);
    ena_set_mem_addr(&cmd.sq_ba, sq_ba);

    return ena_admin_try(t, &cmd, sizeof(cmd), resp, sizeof(*resp));
}

/* CREATE_SQ (DEV/LLQ placement, TX only) feeding @cq_idx. The descriptor ring
 * lives in device memory, so sq_ba is left zero (llq.md §5). */
static inline uint8_t ena_try_create_sq_dev(ENATestCtx *t, uint16_t cq_idx,
                                            uint16_t depth,
                                            ENACreateSqResp *resp)
{
    ENACreateSqCmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.common.opcode = ENA_ADMIN_CREATE_SQ;
    cmd.sq_identity = ENA_ADMIN_SQ_DIRECTION_TX << ENA_ADMIN_SQ_DIRECTION_SHIFT;
    cmd.sq_caps_2 = (ENA_ADMIN_SQ_PLACEMENT_DEV & 0x0f) |
                    (ENA_ADMIN_SQ_COMPLETION_DESC <<
                     ENA_ADMIN_SQ_COMPLETION_POLICY_SHIFT);
    cmd.sq_caps_3 = ENA_ADMIN_SQ_IS_PHYS_CONTIG;
    cmd.cq_idx = cpu_to_le16(cq_idx);
    cmd.sq_depth = cpu_to_le16(depth);

    return ena_admin_try(t, &cmd, sizeof(cmd), resp, sizeof(*resp));
}

static inline uint8_t ena_try_destroy_sq(ENATestCtx *t, uint16_t sq_idx,
                                         uint8_t direction)
{
    ENADestroySqCmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.common.opcode = ENA_ADMIN_DESTROY_SQ;
    cmd.sq.sq_idx = cpu_to_le16(sq_idx);
    cmd.sq.sq_identity = direction << ENA_ADMIN_SQ_DIRECTION_SHIFT;

    return ena_admin_try(t, &cmd, sizeof(cmd), NULL, 0);
}

static inline uint8_t ena_try_destroy_cq(ENATestCtx *t, uint16_t cq_idx)
{
    ENADestroyCqCmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.common.opcode = ENA_ADMIN_DESTROY_CQ;
    cmd.cq_idx = cpu_to_le16(cq_idx);

    return ena_admin_try(t, &cmd, sizeof(cmd), NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Host queue-pair convenience helpers                                 */
/* ------------------------------------------------------------------ */

/*
 * Create a host-placement IO queue pair for @direction: allocate the CQ and
 * SQ rings, CREATE_CQ then CREATE_SQ (queue-setup.md §2 ordering), asserting
 * success, and fill in @q. The SQ doorbell offset and device-assigned indices
 * come from the responses.
 */
static inline void ena_create_io_queue_host(ENATestCtx *t, uint8_t direction,
                                            uint16_t depth, ENAQueue *q)
{
    uint32_t cdesc_size = (direction == ENA_ADMIN_SQ_DIRECTION_TX) ?
                          ENA_TX_CDESC_SIZE : ENA_RX_CDESC_SIZE;
    uint32_t sdesc_size = (direction == ENA_ADMIN_SQ_DIRECTION_TX) ?
                          ENA_TX_DESC_SIZE : ENA_RX_DESC_SIZE;
    ENACreateCqResp cq_resp;
    ENACreateSqResp sq_resp;
    uint8_t status;

    memset(q, 0, sizeof(*q));
    q->direction = direction;
    q->depth = depth;
    q->sq_phase = 1;
    q->cq_phase = 1;

    q->cq_base = guest_alloc(t->alloc, (size_t)depth * cdesc_size);
    q->sq_base = guest_alloc(t->alloc, (size_t)depth * sdesc_size);
    qtest_memset(t->qts, q->cq_base, 0, (size_t)depth * cdesc_size);
    qtest_memset(t->qts, q->sq_base, 0, (size_t)depth * sdesc_size);

    status = ena_try_create_cq(t, depth, cdesc_size, q->cq_base, &cq_resp);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    q->cq_idx = le16_to_cpu(cq_resp.cq_idx);

    status = ena_try_create_sq_host(t, direction, q->cq_idx, depth,
                                    q->sq_base, &sq_resp);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    q->sq_idx = le16_to_cpu(sq_resp.sq_idx);
    q->sq_doorbell_off = le32_to_cpu(sq_resp.sq_doorbell_offset);
}

/*
 * Create an LLQ (DEV-placement) TX queue pair: a host-memory TX CQ plus a
 * device-memory TX SQ. The caller must have negotiated the LLQ layout via
 * SET_FEATURE(LLQ) first and passes the matching @entry_size /
 * @descs_before_header / @descs_per_entry so @q can lay entries out the way the
 * device parses them (llq.md §6). The SQ's device-memory ring offset comes from
 * the CREATE_SQ response (llq_descriptors_offset).
 */
static inline void ena_create_io_queue_llq(ENATestCtx *t, uint16_t depth,
                                           uint16_t entry_size,
                                           uint8_t descs_before_header,
                                           uint8_t descs_per_entry, ENAQueue *q)
{
    ENACreateCqResp cq_resp;
    ENACreateSqResp sq_resp;
    uint8_t status;

    memset(q, 0, sizeof(*q));
    q->direction = ENA_ADMIN_SQ_DIRECTION_TX;
    q->depth = depth;
    q->sq_phase = 1;
    q->cq_phase = 1;
    q->llq = true;
    q->llq_entry_size = entry_size;
    q->llq_descs_before_header = descs_before_header;
    q->llq_descs_per_entry = descs_per_entry;

    q->cq_base = guest_alloc(t->alloc, (size_t)depth * ENA_TX_CDESC_SIZE);
    qtest_memset(t->qts, q->cq_base, 0, (size_t)depth * ENA_TX_CDESC_SIZE);

    status = ena_try_create_cq(t, depth, ENA_TX_CDESC_SIZE, q->cq_base, &cq_resp);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    q->cq_idx = le16_to_cpu(cq_resp.cq_idx);

    status = ena_try_create_sq_dev(t, q->cq_idx, depth, &sq_resp);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    q->sq_idx = le16_to_cpu(sq_resp.sq_idx);
    q->sq_doorbell_off = le32_to_cpu(sq_resp.sq_doorbell_offset);
    q->llq_desc_off = le32_to_cpu(sq_resp.llq_descriptors_offset);
}

/* Tear down a queue pair: DESTROY_SQ then DESTROY_CQ (queue-setup.md §6). */
static inline void ena_destroy_io_queue(ENATestCtx *t, ENAQueue *q)
{
    uint8_t status;

    status = ena_try_destroy_sq(t, q->sq_idx, q->direction);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    status = ena_try_destroy_cq(t, q->cq_idx);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
}

#endif /* TESTS_ENA_TEST_QUEUE_H */
