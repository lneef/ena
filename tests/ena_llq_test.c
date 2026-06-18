/*
 * ENA LLQ (Low Latency Queue) TX datapath qtests (DEV placement, polling).
 *
 * Exercises the device-side LLQ flow documented in docs/wiki/llq.md and
 * docs/wiki/queue-setup.md §4: the driver negotiates the LLQ layout via
 * GET/SET_FEATURE(ENA_ADMIN_LLQ), creates a TX SQ with placement policy DEV
 * (descriptor ring in the MEM BAR, BAR2), then *pushes* fixed-size entries
 * (descriptor list + inline header) into device memory and rings the per-SQ
 * doorbell with the new tail counted in entries/lines. The device must parse
 * the descriptors out of device memory honouring the line layout (up to
 * descs_before_header descriptors in the first line, the rest spilling to
 * subsequent lines descs_per_entry at a time) and post exactly one TX
 * completion per packet whose first descriptor had comp_req set (llq.md §6, §9).
 *
 * Like the host TX suite, completions are found purely by the CQ phase bit and
 * the cdesc status/sub_qid/sq_head_idx fields are not asserted (the reference
 * driver never reads them). Offloads / meta descriptors are out of scope.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#include "ena_test_queue.h"

#define ENA_LLQ_TEST_DEPTH      256
#define ENA_LLQ_TEST_PKT_LEN    128
#define ENA_MEM_BAR_IDX         2   /* LLQ device memory (BAR2) */

/* Device-advertised LLQ capabilities (GET_FEATURE(LLQ), llq.md §5). The queue
 * count tracks the driver's ENA_MAX_NUM_IO_QUEUES; depths follow from the
 * 128 KiB per-SQ slice (128 KiB / entry_size). */
#define ENA_LLQ_EXP_MAX_NUM         128
#define ENA_LLQ_EXP_MAX_DEPTH       1024    /* 128 KiB / 128B entries */
#define ENA_LLQ_EXP_MAX_WIDE_DEPTH  512     /* 128 KiB / 256B entries */

/* LLQ feature sub-enums and the negotiated config struct (llq.md §2). */
#define ENA_ADMIN_LLQ_INLINE_HEADER             1
#define ENA_ADMIN_LLQ_ENTRY_SIZE_128B           1
#define ENA_ADMIN_LLQ_ENTRY_SIZE_256B           4
#define ENA_ADMIN_LLQ_SINGLE_DESC_PER_ENTRY     1
#define ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY  2
#define ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2     2

/* struct ena_admin_feature_llq_desc payload (llq.md §2), 36 bytes. */
typedef struct QEMU_PACKED ENAFeatureLlqDesc {
    uint32_t max_llq_num;
    uint32_t max_llq_depth;
    uint16_t header_location_ctrl_supported;
    uint16_t header_location_ctrl_enabled;
    uint16_t entry_size_ctrl_supported;
    uint16_t entry_size_ctrl_enabled;
    uint16_t desc_num_before_header_supported;
    uint16_t desc_num_before_header_enabled;
    uint16_t descriptors_stride_ctrl_supported;
    uint16_t descriptors_stride_ctrl_enabled;
    uint8_t  feature_version;
    uint8_t  entry_size_recommended;
    uint16_t max_wide_llq_depth;
    uint32_t accel_mode[2];
} ENAFeatureLlqDesc;

typedef struct QEMU_PACKED ENAGetLlqResp {
    struct ena_admin_acq_common_desc common;
    ENAFeatureLlqDesc llq;
} ENAGetLlqResp;

/* One descriptor of a packet, as the test lays it out. */
typedef struct LLQDescSpec {
    uint16_t len;
    uint16_t req_id;
    bool first;
    bool last;
    bool comp_req;
    bool meta;      /* offload meta descriptor: occupies a slot, skipped */
} LLQDescSpec;

/* ------------------------------------------------------------------ */
/* LLQ feature negotiation                                             */
/* ------------------------------------------------------------------ */

/*
 * SET_FEATURE(LLQ) selecting inline-header mode, @entry_enum entry size,
 * descs_before_header=2 and multiple-descs-per-entry stride. Returns the
 * resulting descs_per_entry (entry_bytes / 16) the device will use.
 */
static uint8_t ena_llq_negotiate(ENATestCtx *t, uint16_t entry_enum,
                                 uint16_t entry_bytes)
{
    ENAFeatureLlqDesc desc;
    uint8_t status;

    memset(&desc, 0, sizeof(desc));
    desc.header_location_ctrl_enabled = cpu_to_le16(ENA_ADMIN_LLQ_INLINE_HEADER);
    desc.entry_size_ctrl_enabled = cpu_to_le16(entry_enum);
    desc.desc_num_before_header_enabled =
        cpu_to_le16(ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2);
    desc.descriptors_stride_ctrl_enabled =
        cpu_to_le16(ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY);

    status = ena_set_feature(t, ENA_ADMIN_LLQ, 0, &desc, sizeof(desc), NULL, 0);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    return entry_bytes / ENA_TX_DESC_SIZE;
}

/*
 * As ena_llq_negotiate but lets the caller pick the descriptors-stride control
 * (SINGLE vs MULTIPLE descs per spilled line). Returns the descs_per_entry the
 * device derives: entry_bytes/16 for MULTIPLE stride, 1 for SINGLE.
 */
static uint8_t ena_llq_negotiate_stride(ENATestCtx *t, uint16_t entry_enum,
                                        uint16_t entry_bytes, uint16_t stride)
{
    ENAFeatureLlqDesc desc;
    uint8_t status;

    memset(&desc, 0, sizeof(desc));
    desc.header_location_ctrl_enabled = cpu_to_le16(ENA_ADMIN_LLQ_INLINE_HEADER);
    desc.entry_size_ctrl_enabled = cpu_to_le16(entry_enum);
    desc.desc_num_before_header_enabled =
        cpu_to_le16(ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2);
    desc.descriptors_stride_ctrl_enabled = cpu_to_le16(stride);

    status = ena_set_feature(t, ENA_ADMIN_LLQ, 0, &desc, sizeof(desc), NULL, 0);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    return stride == ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY ?
           entry_bytes / ENA_TX_DESC_SIZE : 1;
}

/* ------------------------------------------------------------------ */
/* LLQ entry push (device-memory ring)                                 */
/* ------------------------------------------------------------------ */

static void ena_llq_build_desc(struct ena_eth_io_tx_desc *d, uint8_t phase,
                               const LLQDescSpec *s)
{
    uint32_t len_ctrl = 0, meta_ctrl = 0;

    len_ctrl |= s->len & ENA_ETH_IO_TX_DESC_LENGTH_MASK;
    len_ctrl |= ((uint32_t)phase << ENA_ETH_IO_TX_DESC_PHASE_SHIFT) &
                ENA_ETH_IO_TX_DESC_PHASE_MASK;
    if (s->first) {
        len_ctrl |= ENA_ETH_IO_TX_DESC_FIRST_MASK;
    }
    if (s->last) {
        len_ctrl |= ENA_ETH_IO_TX_DESC_LAST_MASK;
    }
    if (s->comp_req) {
        len_ctrl |= ENA_ETH_IO_TX_DESC_COMP_REQ_MASK;
    }
    if (s->meta) {
        len_ctrl |= ENA_ETH_IO_TX_DESC_META_DESC_MASK;
    }
    len_ctrl |= ((uint32_t)(s->req_id >> 10) <<
                 ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT) &
                ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK;
    meta_ctrl |= ((uint32_t)s->req_id << ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT) &
                 ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK;

    d->len_ctrl = cpu_to_le32(len_ctrl);
    d->meta_ctrl = cpu_to_le32(meta_ctrl);
    d->buff_addr_lo = 0;
    d->buff_addr_hi_hdr_sz = 0;
}

/*
 * Push one packet's descriptor list into the LLQ device-memory ring, laying the
 * descriptors across lines exactly as the device parses them (llq.md §6): the
 * first line holds up to descs_before_header descriptors, the rest spill to the
 * start of subsequent lines descs_per_entry at a time. Advances q->sq_tail past
 * the packet's last line. The inline header bytes between the descriptors and
 * the next line are not written — the device never reads them.
 */
static void ena_llq_push(ENATestCtx *t, QPCIBar mem, ENAQueue *q,
                         const LLQDescSpec *ds, int n)
{
    uint16_t line = q->sq_tail;
    uint8_t slot = 0;
    uint8_t left = q->llq_descs_before_header;
    int i;

    for (i = 0; i < n; i++) {
        struct ena_eth_io_tx_desc d;
        uint8_t phase = 1 ^ ((line / q->depth) & 1);
        uint32_t off = q->llq_desc_off +
                       (uint32_t)(line & (q->depth - 1)) * q->llq_entry_size +
                       (uint32_t)slot * ENA_TX_DESC_SIZE;

        ena_llq_build_desc(&d, phase, &ds[i]);
        qpci_memwrite(t->dev, mem, off, &d, sizeof(d));
        slot++;
        left--;
        if (i == n - 1) {
            break;
        }
        if (left == 0) {
            line++;
            slot = 0;
            left = q->llq_descs_per_entry;
        }
    }

    q->sq_tail = line + 1;
}

/* Ring the SQ doorbell with the current absolute line tail (llq.md §8). */
static void ena_llq_doorbell(ENATestCtx *t, ENAQueue *q)
{
    ena_reg_write(t, q->sq_doorbell_off, q->sq_tail);
}

/* ------------------------------------------------------------------ */
/* TX CQ completion helpers (host-memory CQ, identical to the host path)*/
/* ------------------------------------------------------------------ */

static uint16_t ena_llq_poll_cdesc(ENATestCtx *t, ENAQueue *q)
{
    uint32_t idx = q->cq_head & (q->depth - 1);
    struct ena_eth_io_tx_cdesc c;
    int i;

    for (i = 0; i < ENA_TEST_POLL_RETRIES; i++) {
        qtest_memread(t->qts, q->cq_base + (uint64_t)idx * ENA_TX_CDESC_SIZE,
                      &c, sizeof(c));
        if ((c.flags & ENA_ETH_IO_TX_CDESC_PHASE_MASK) == q->cq_phase) {
            g_assert_cmphex(c.flags & ENA_ETH_IO_TX_CDESC_MBZ6_MASK, ==, 0);

            q->cq_head++;
            if ((q->cq_head & (q->depth - 1)) == 0) {
                q->cq_phase ^= 1;
            }
            return le16_to_cpu(c.req_id);
        }
        g_usleep(ENA_TEST_POLL_DELAY_US);
    }

    g_assert_not_reached(); /* device never posted the TX completion */
}

static void ena_llq_assert_no_cdesc(ENATestCtx *t, ENAQueue *q)
{
    uint32_t idx = q->cq_head & (q->depth - 1);
    struct ena_eth_io_tx_cdesc c;

    qtest_memread(t->qts, q->cq_base + (uint64_t)idx * ENA_TX_CDESC_SIZE,
                  &c, sizeof(c));
    g_assert_cmphex(c.flags & ENA_ETH_IO_TX_CDESC_PHASE_MASK, !=, q->cq_phase);
}

static void ena_assert_ready(ENATestCtx *t)
{
    uint32_t sts = ena_reg_read(t, ENA_REGS_DEV_STS_OFF);

    g_assert_cmphex(sts & ENA_REGS_DEV_STS_READY_MASK, ==,
                    ENA_REGS_DEV_STS_READY_MASK);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_FATAL_ERROR_MASK, ==, 0);
}

/* Map the LLQ MEM BAR (BAR2) for direct entry pushes. */
static QPCIBar ena_llq_map_mem_bar(ENATestCtx *t)
{
    return qpci_iomap(t->dev, ENA_MEM_BAR_IDX, NULL);
}

/* ------------------------------------------------------------------ */
/* GET_FEATURE(LLQ) advertises a usable LLQ configuration              */
/* ------------------------------------------------------------------ */

static void test_llq_get_feature(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAGetLlqResp resp;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    status = ena_get_feature(&t, ENA_ADMIN_LLQ, 0, &resp, sizeof(resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    /* The device must offer inline-header mode, both 128B and 256B entries,
     * multiple-descs-per-entry stride, and the slice-derived count/depths
     * (llq.md §5): 128 queues, 1024-deep 128B rings, 512-deep 256B rings. */
    g_assert_cmpuint(le32_to_cpu(resp.llq.max_llq_num), ==, ENA_LLQ_EXP_MAX_NUM);
    g_assert_cmpuint(le32_to_cpu(resp.llq.max_llq_depth), ==,
                     ENA_LLQ_EXP_MAX_DEPTH);
    g_assert_cmpuint(le16_to_cpu(resp.llq.max_wide_llq_depth), ==,
                     ENA_LLQ_EXP_MAX_WIDE_DEPTH);
    g_assert_cmphex(le16_to_cpu(resp.llq.header_location_ctrl_supported) &
                    ENA_ADMIN_LLQ_INLINE_HEADER, ==, ENA_ADMIN_LLQ_INLINE_HEADER);
    g_assert_cmphex(le16_to_cpu(resp.llq.entry_size_ctrl_supported) &
                    (ENA_ADMIN_LLQ_ENTRY_SIZE_128B | ENA_ADMIN_LLQ_ENTRY_SIZE_256B),
                    ==, ENA_ADMIN_LLQ_ENTRY_SIZE_128B | ENA_ADMIN_LLQ_ENTRY_SIZE_256B);
    g_assert_cmphex(le16_to_cpu(resp.llq.descriptors_stride_ctrl_supported) &
                    ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY, ==,
                    ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY);
}

/* ------------------------------------------------------------------ */
/* Single-descriptor packet -> one completion (256B entries)           */
/* ------------------------------------------------------------------ */

static void test_llq_single_buffer(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t req_id = 7;
    const LLQDescSpec pkt[] = {
        { ENA_LLQ_TEST_PKT_LEN, req_id, true, true, true },
    };

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_256B, 256);
    ena_create_io_queue_llq(&t, ENA_LLQ_TEST_DEPTH, 256,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    ena_llq_push(&t, mem, &q, pkt, ARRAY_SIZE(pkt));
    ena_llq_doorbell(&t, &q);

    g_assert_cmpuint(ena_llq_poll_cdesc(&t, &q), ==, req_id);

    ena_llq_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Multi-descriptor packet spanning lines -> a single completion       */
/* ------------------------------------------------------------------ */

/*
 * With 128B entries and descs_before_header=2, a three-descriptor packet does
 * not fit in the first line: desc0/desc1 land in line 0, desc2 spills to line 1
 * (llq.md §6). The device must accumulate all three as one packet across the
 * two lines and post exactly one completion carrying the first descriptor's
 * req_id.
 */
static void test_llq_multi_line_single_completion(void *obj, void *data,
                                                  QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t req_id = 11;
    const LLQDescSpec pkt[] = {
        { ENA_LLQ_TEST_PKT_LEN, req_id, true,  false, true  },
        { ENA_LLQ_TEST_PKT_LEN, 0,      false, false, false },
        { ENA_LLQ_TEST_PKT_LEN, 0,      false, true,  false },
    };

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_128B, 128);
    ena_create_io_queue_llq(&t, ENA_LLQ_TEST_DEPTH, 128,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    ena_llq_push(&t, mem, &q, pkt, ARRAY_SIZE(pkt));
    ena_llq_doorbell(&t, &q);

    g_assert_cmpuint(ena_llq_poll_cdesc(&t, &q), ==, req_id);

    ena_llq_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Several packets behind one doorbell -> a completion each            */
/* ------------------------------------------------------------------ */

static void test_llq_burst_one_doorbell(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t req_ids[] = { 3, 1, 2 };
    const int n = ARRAY_SIZE(req_ids);
    bool seen[16] = { false };
    int i;

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_256B, 256);
    ena_create_io_queue_llq(&t, ENA_LLQ_TEST_DEPTH, 256,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    for (i = 0; i < n; i++) {
        const LLQDescSpec pkt[] = {
            { ENA_LLQ_TEST_PKT_LEN, req_ids[i], true, true, true },
        };
        ena_llq_push(&t, mem, &q, pkt, ARRAY_SIZE(pkt));
    }
    ena_llq_doorbell(&t, &q);

    for (i = 0; i < n; i++) {
        uint16_t rid = ena_llq_poll_cdesc(&t, &q);

        g_assert_cmpuint(rid, <, ARRAY_SIZE(seen));
        g_assert_false(seen[rid]);
        seen[rid] = true;
    }
    for (i = 0; i < n; i++) {
        g_assert_true(seen[req_ids[i]]);
    }

    ena_llq_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* CQ phase bit tracking across a full ring wrap                       */
/* ------------------------------------------------------------------ */

static void test_llq_cq_phase_wrap(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t depth = 4;
    const int iters = 2 * depth + 1;
    int i;

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_256B, 256);
    ena_create_io_queue_llq(&t, depth, 256,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    for (i = 0; i < iters; i++) {
        uint16_t req_id = i % depth;
        uint8_t phase_before = q.cq_phase;
        const LLQDescSpec pkt[] = {
            { ENA_LLQ_TEST_PKT_LEN, req_id, true, true, true },
        };

        ena_llq_push(&t, mem, &q, pkt, ARRAY_SIZE(pkt));
        ena_llq_doorbell(&t, &q);

        g_assert_cmpuint(ena_llq_poll_cdesc(&t, &q), ==, req_id);

        if ((q.cq_head & (depth - 1)) == 0) {
            g_assert_cmpuint(q.cq_phase, !=, phase_before);
        } else {
            g_assert_cmpuint(q.cq_phase, ==, phase_before);
        }
    }

    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Negative: oversized depth and wrong direction are rejected          */
/* ------------------------------------------------------------------ */

static void test_llq_create_rejects(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENACreateCqResp cq_resp;
    ENACreateSqResp sq_resp;
    ENACreateSqCmd rx_cmd;
    uint64_t cq_base;
    uint16_t cq_idx;
    uint8_t status;

    ena_bringup(&t, obj, alloc);
    ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_256B, 256);

    cq_base = guest_alloc(t.alloc, (size_t)ENA_LLQ_TEST_DEPTH * ENA_TX_CDESC_SIZE);
    status = ena_try_create_cq(&t, ENA_LLQ_TEST_DEPTH, ENA_TX_CDESC_SIZE,
                               cq_base, &cq_resp);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    cq_idx = le16_to_cpu(cq_resp.cq_idx);

    /* 256B * 1024 = 256 KiB exceeds the 128 KiB per-SQ LLQ slice. */
    status = ena_try_create_sq_dev(&t, cq_idx, 1024, &sq_resp);
    g_assert_cmpuint(status, ==, ENA_ADMIN_ILLEGAL_PARAMETER);

    /* LLQ is TX-only: a DEV-placement RX SQ must be rejected. */
    memset(&rx_cmd, 0, sizeof(rx_cmd));
    rx_cmd.common.opcode = ENA_ADMIN_CREATE_SQ;
    rx_cmd.sq_identity = ENA_ADMIN_SQ_DIRECTION_RX << ENA_ADMIN_SQ_DIRECTION_SHIFT;
    rx_cmd.sq_caps_2 = (ENA_ADMIN_SQ_PLACEMENT_DEV & 0x0f) |
                       (ENA_ADMIN_SQ_COMPLETION_DESC <<
                        ENA_ADMIN_SQ_COMPLETION_POLICY_SHIFT);
    rx_cmd.sq_caps_3 = ENA_ADMIN_SQ_IS_PHYS_CONTIG;
    rx_cmd.cq_idx = cpu_to_le16(cq_idx);
    rx_cmd.sq_depth = cpu_to_le16(ENA_LLQ_TEST_DEPTH);
    status = ena_admin_try(&t, &rx_cmd, sizeof(rx_cmd), &sq_resp, sizeof(sq_resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_ILLEGAL_PARAMETER);

    ena_assert_ready(&t);
    status = ena_try_destroy_cq(&t, cq_idx);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Adversarial: comp_req=0 packet must yield NO completion, yet the device    */
/* must still advance its line head so the *next* packet completes correctly. */
/* ------------------------------------------------------------------ */

/*
 * Two single-line packets behind one doorbell: the first has comp_req clear,
 * the second has it set. The device must post exactly one completion carrying
 * the *second* packet's req_id. A device that posts for the suppressed packet,
 * or that fails to advance its head past it, would surface the wrong req_id
 * here (or a spurious second cdesc).
 */
static void test_llq_comp_req_suppressed(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t rid_silent = 100;
    const uint16_t rid_signal = 200;
    const LLQDescSpec silent[] = {
        { ENA_LLQ_TEST_PKT_LEN, rid_silent, true, true, false, false },
    };
    const LLQDescSpec signal[] = {
        { ENA_LLQ_TEST_PKT_LEN, rid_signal, true, true, true, false },
    };

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_256B, 256);
    ena_create_io_queue_llq(&t, ENA_LLQ_TEST_DEPTH, 256,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    ena_llq_push(&t, mem, &q, silent, ARRAY_SIZE(silent));
    ena_llq_push(&t, mem, &q, signal, ARRAY_SIZE(signal));
    ena_llq_doorbell(&t, &q);

    /* Only the second packet may complete, and it must report its own req_id. */
    g_assert_cmpuint(ena_llq_poll_cdesc(&t, &q), ==, rid_signal);
    ena_llq_assert_no_cdesc(&t, &q);

    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Adversarial: SINGLE-desc-per-entry stride => one descriptor per spilled    */
/* line. Exercises the descs_per_entry==1 spill path the MULTIPLE-stride       */
/* tests never reach.                                                          */
/* ------------------------------------------------------------------ */

/*
 * With descs_before_header=2 and SINGLE stride, a three-descriptor packet
 * lays out desc0/desc1 in line 0 and desc2 *alone* in line 1 (descs_per_entry
 * collapses to 1). The device must accumulate all three as one packet and post
 * a single completion; off-by-one line accounting (e.g. assuming 8/16 descs in
 * the spilled line) would read a stale/garbage descriptor as the packet's tail.
 */
static void test_llq_single_stride_spill(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t req_id = 21;
    const LLQDescSpec pkt[] = {
        { ENA_LLQ_TEST_PKT_LEN, req_id, true,  false, true,  false },
        { ENA_LLQ_TEST_PKT_LEN, 0,      false, false, false, false },
        { ENA_LLQ_TEST_PKT_LEN, 0,      false, true,  false, false },
    };

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate_stride(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_128B, 128,
                                   ENA_ADMIN_LLQ_SINGLE_DESC_PER_ENTRY);
    g_assert_cmpuint(dpe, ==, 1);
    ena_create_io_queue_llq(&t, ENA_LLQ_TEST_DEPTH, 128,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    ena_llq_push(&t, mem, &q, pkt, ARRAY_SIZE(pkt));
    ena_llq_doorbell(&t, &q);

    g_assert_cmpuint(ena_llq_poll_cdesc(&t, &q), ==, req_id);
    ena_llq_assert_no_cdesc(&t, &q);

    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Adversarial: a packet spanning three lines (MULTIPLE stride, 128B).        */
/* ------------------------------------------------------------------ */

/*
 * 128B entries with descs_before_header=2 hold 2 descs in line 0 and
 * descs_per_entry=8 in each subsequent line. An 11-descriptor packet therefore
 * spans three lines: 2 + 8 + 1. The device must walk all three lines, keep the
 * packet open until LAST, advance head by exactly three lines, and post one
 * completion. A device that resets its per-line counter wrongly between the
 * second and third line would close the packet early (extra completion) or run
 * off into the next slot.
 */
static void test_llq_three_line_spill(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t req_id = 33;
    const int ndesc = 11;
    LLQDescSpec pkt[11];
    int i;

    for (i = 0; i < ndesc; i++) {
        pkt[i].len = ENA_LLQ_TEST_PKT_LEN;
        pkt[i].req_id = (i == 0) ? req_id : 0;
        pkt[i].first = (i == 0);
        pkt[i].last = (i == ndesc - 1);
        pkt[i].comp_req = (i == 0);
        pkt[i].meta = false;
    }

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_128B, 128);
    ena_create_io_queue_llq(&t, ENA_LLQ_TEST_DEPTH, 128,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    ena_llq_push(&t, mem, &q, pkt, ndesc);
    /* 2 + 8 + 1 descriptors => three consumed lines. */
    g_assert_cmpuint(q.sq_tail, ==, 3);
    ena_llq_doorbell(&t, &q);

    g_assert_cmpuint(ena_llq_poll_cdesc(&t, &q), ==, req_id);
    ena_llq_assert_no_cdesc(&t, &q);

    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Adversarial: req_id at the top of its 10-bit low field round-trips.        */
/* ------------------------------------------------------------------ */

/*
 * req_id is split across two descriptor words (low 10 bits in meta_ctrl, high
 * 6 in len_ctrl). The other LLQ tests only use small ids (<16) that never fill
 * the low field; 1023 == 0x3ff sets all ten low bits (the largest id a
 * 1024-deep ring can index). A shift/mask slip in the device's req_id
 * reassembly would corrupt the value here while passing every existing test.
 */
static void test_llq_req_id_field_width(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t req_id = 1023;
    const LLQDescSpec pkt[] = {
        { ENA_LLQ_TEST_PKT_LEN, req_id, true, true, true, false },
    };

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_256B, 256);
    ena_create_io_queue_llq(&t, ENA_LLQ_TEST_DEPTH, 256,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    ena_llq_push(&t, mem, &q, pkt, ARRAY_SIZE(pkt));
    ena_llq_doorbell(&t, &q);

    g_assert_cmpuint(ena_llq_poll_cdesc(&t, &q), ==, req_id);
    ena_llq_assert_no_cdesc(&t, &q);

    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Adversarial: a meta descriptor preceding the data descriptors must be      */
/* skipped (occupying a slot) without being mistaken for the packet's first   */
/* data descriptor.                                                           */
/* ------------------------------------------------------------------ */

/*
 * A meta descriptor (META_DESC bit set) leads the packet, carrying misleading
 * comp_req/req_id bits the device must ignore. The real first data descriptor
 * follows in the next slot with the true req_id. The device must consume the
 * meta slot, take comp_req/req_id from the data descriptor, and post one
 * completion with the data req_id. A device that latches the meta descriptor as
 * the packet head would report the wrong req_id (or suppress the completion).
 */
static void test_llq_meta_desc_skipped(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    QPCIBar mem;
    uint8_t dpe;
    const uint16_t data_req_id = 42;
    const uint16_t meta_decoy = 99;
    const LLQDescSpec pkt[] = {
        /* meta: bogus req_id, comp_req clear; must be ignored entirely. */
        { 0,                    meta_decoy,   false, false, false, true  },
        { ENA_LLQ_TEST_PKT_LEN, data_req_id,  true,  true,  true,  false },
    };

    ena_bringup(&t, obj, alloc);
    dpe = ena_llq_negotiate(&t, ENA_ADMIN_LLQ_ENTRY_SIZE_256B, 256);
    ena_create_io_queue_llq(&t, ENA_LLQ_TEST_DEPTH, 256,
                            ENA_ADMIN_LLQ_DESCS_BEFORE_HEADER_2, dpe, &q);
    mem = ena_llq_map_mem_bar(&t);

    ena_llq_push(&t, mem, &q, pkt, ARRAY_SIZE(pkt));
    ena_llq_doorbell(&t, &q);

    g_assert_cmpuint(ena_llq_poll_cdesc(&t, &q), ==, data_req_id);
    ena_llq_assert_no_cdesc(&t, &q);

    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */

static void ena_register_nodes(void)
{
    ena_qos_node_register("ena-llq");

    qos_add_test("llq/get-feature", "ena-llq", test_llq_get_feature, NULL);
    qos_add_test("llq/single-buffer", "ena-llq", test_llq_single_buffer, NULL);
    qos_add_test("llq/multi-line-single-completion", "ena-llq",
                 test_llq_multi_line_single_completion, NULL);
    qos_add_test("llq/burst-one-doorbell", "ena-llq",
                 test_llq_burst_one_doorbell, NULL);
    qos_add_test("llq/cq-phase-wrap", "ena-llq", test_llq_cq_phase_wrap, NULL);
    qos_add_test("llq/create-rejects", "ena-llq", test_llq_create_rejects, NULL);
    qos_add_test("llq/comp-req-suppressed", "ena-llq",
                 test_llq_comp_req_suppressed, NULL);
    qos_add_test("llq/single-stride-spill", "ena-llq",
                 test_llq_single_stride_spill, NULL);
    qos_add_test("llq/three-line-spill", "ena-llq",
                 test_llq_three_line_spill, NULL);
    qos_add_test("llq/req-id-field-width", "ena-llq",
                 test_llq_req_id_field_width, NULL);
    qos_add_test("llq/meta-desc-skipped", "ena-llq",
                 test_llq_meta_desc_skipped, NULL);
}

libqos_init(ena_register_nodes);
