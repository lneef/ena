/*
 * ENA TX datapath qtests (host placement, polling mode, no LLQ, no IRQ).
 *
 * Exercises the device-side TX flow documented in docs/wiki/tx-path.md and
 * docs/wiki/tx-descriptors.md: the driver lays out TX SQ data descriptors for
 * a packet (first .. last, req_id + comp_req on the first descriptor), rings
 * the per-SQ doorbell with the new tail, and the device must DMA-read the
 * descriptors and post exactly one TX completion descriptor (cdesc) per packet
 * whose first descriptor had comp_req set. Completions are found purely by the
 * CQ phase bit (no CQ head doorbell, no MSI-X).
 *
 * Asserted contract (tx-path.md §4-§6): exactly what the driver's sole TX
 * completion consumer, ena_com_tx_comp_req_id_get (base/ena_eth_com.h), reads:
 *   - one cdesc per completed packet, echoing the packet's req_id;
 *   - cdesc.flags PHASE matches the CQ's current pass and flips on CQ wrap
 *     (the completion-validity signal);
 *   - cdesc.flags MBZ6 bits are zero;
 *   - a multi-descriptor packet yields a single completion, not one per desc;
 *   - many packets behind one doorbell each complete.
 *
 * The cdesc's status, sub_qid and sq_head_idx fields are deliberately NOT
 * asserted: the reference driver never reads them. SQ space is reclaimed from
 * the driver's own tx_info->tx_descs count via ena_com_comp_ack, not from
 * cdesc.sq_head_idx; there is no error gating on cdesc.status; sub_qid is
 * unused. Asserting them would over-specify device behaviour the driver cannot
 * observe.
 *
 * Offloads/TSO (the optional meta descriptor) are out of scope here per the
 * project's "core datapath first" ordering; see docs/wiki/offloads.md.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#include "ena_test_queue.h"

#define ENA_TX_TEST_DEPTH       256
#define ENA_TX_TEST_PKT_LEN     128

/* ------------------------------------------------------------------ */
/* TX SQ submission / CQ completion helpers                            */
/* ------------------------------------------------------------------ */

/*
 * Build and post one TX data descriptor at the SQ tail, stamping it with the
 * SQ's current producer phase, then advance the tail (flipping phase on ring
 * wrap), mirroring ena_com_prepare_tx + the host-queue tail update. req_id and
 * comp_req are only meaningful on the first descriptor of a packet; callers
 * pass req_id == 0 and comp_req == false for trailing descriptors, matching the
 * driver which writes those bits only into the first descriptor.
 */
static void ena_tx_post_data(ENATestCtx *t, ENAQueue *q, uint64_t buf,
                             uint16_t len, uint16_t req_id, bool first,
                             bool last, bool comp_req)
{
    struct ena_eth_io_tx_desc d;
    uint32_t idx = q->sq_tail & (q->depth - 1);
    uint32_t len_ctrl = 0, meta_ctrl = 0;

    len_ctrl |= len & ENA_ETH_IO_TX_DESC_LENGTH_MASK;
    len_ctrl |= ((uint32_t)q->sq_phase << ENA_ETH_IO_TX_DESC_PHASE_SHIFT) &
                ENA_ETH_IO_TX_DESC_PHASE_MASK;
    if (first) {
        len_ctrl |= ENA_ETH_IO_TX_DESC_FIRST_MASK;
    }
    if (last) {
        len_ctrl |= ENA_ETH_IO_TX_DESC_LAST_MASK;
    }
    if (comp_req) {
        len_ctrl |= ENA_ETH_IO_TX_DESC_COMP_REQ_MASK;
    }
    /* req_id split across the first descriptor (tx-descriptors.md §4). */
    len_ctrl |= ((uint32_t)(req_id >> 10) <<
                 ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT) &
                ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK;
    meta_ctrl |= ((uint32_t)req_id << ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT) &
                 ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK;

    d.len_ctrl = cpu_to_le32(len_ctrl);
    d.meta_ctrl = cpu_to_le32(meta_ctrl);
    d.buff_addr_lo = cpu_to_le32((uint32_t)buf);
    d.buff_addr_hi_hdr_sz =
        cpu_to_le32((uint32_t)(buf >> 32) & ENA_ETH_IO_TX_DESC_ADDR_HI_MASK);

    qtest_memwrite(t->qts, q->sq_base + (uint64_t)idx * ENA_TX_DESC_SIZE,
                   &d, sizeof(d));

    q->sq_tail++;
    if ((q->sq_tail & (q->depth - 1)) == 0) {
        q->sq_phase ^= 1;
    }
}

/* Post a single-buffer packet (first == last) requesting a completion. */
static void ena_tx_post_packet(ENATestCtx *t, ENAQueue *q, uint64_t buf,
                               uint16_t len, uint16_t req_id)
{
    ena_tx_post_data(t, q, buf, len, req_id,
                     /*first=*/true, /*last=*/true, /*comp_req=*/true);
}

/* Ring the SQ doorbell with the current absolute tail (tx-path.md §2). */
static void ena_tx_doorbell(ENATestCtx *t, ENAQueue *q)
{
    ena_reg_write(t, q->sq_doorbell_off, q->sq_tail);
}

/*
 * Poll the TX CQ for the next completion at q->cq_head and return its req_id,
 * mirroring ena_com_tx_comp_req_id_get: wait until the cdesc's phase bit
 * matches the expected CQ phase (validity signal), reject a non-zero MBZ6,
 * then advance the consumer (flipping phase on wrap). Fails (no hang) if the
 * device never posts the completion within the poll budget.
 */
static uint16_t ena_tx_poll_cdesc(ENATestCtx *t, ENAQueue *q)
{
    uint32_t idx = q->cq_head & (q->depth - 1);
    struct ena_eth_io_tx_cdesc c;
    int i;

    for (i = 0; i < ENA_TEST_POLL_RETRIES; i++) {
        qtest_memread(t->qts, q->cq_base + (uint64_t)idx * ENA_TX_CDESC_SIZE,
                      &c, sizeof(c));
        if ((c.flags & ENA_ETH_IO_TX_CDESC_PHASE_MASK) == q->cq_phase) {
            /* MBZ6 (flags[7:6]) must be zero (tx-path.md §6). */
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

/*
 * Assert the device has NOT posted a completion at the current CQ head: the
 * slot's phase bit must still differ from the expected phase. The qtest device
 * acts synchronously on the doorbell MMIO, so a missing completion is decided
 * on the first read.
 */
static void ena_tx_assert_no_cdesc(ENATestCtx *t, ENAQueue *q)
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

/* Allocate and zero a guest TX payload buffer. */
static uint64_t ena_tx_alloc_buf(ENATestCtx *t, size_t len)
{
    uint64_t buf = guest_alloc(t->alloc, len);

    qtest_memset(t->qts, buf, 0xa5, len);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Single-buffer packet -> one completion                              */
/* ------------------------------------------------------------------ */

/*
 * The smallest TX transaction: one data descriptor with first|last|comp_req.
 * The device must post exactly one cdesc echoing the packet's req_id, with the
 * CQ's initial phase and zero MBZ6 (both checked by ena_tx_poll_cdesc). No
 * second completion may appear.
 */
static void test_tx_single_buffer(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    uint64_t buf;
    const uint16_t req_id = 7;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                             ENA_TX_TEST_DEPTH, &q);
    buf = ena_tx_alloc_buf(&t, ENA_TX_TEST_PKT_LEN);

    ena_tx_post_packet(&t, &q, buf, ENA_TX_TEST_PKT_LEN, req_id);
    ena_tx_doorbell(&t, &q);

    g_assert_cmpuint(ena_tx_poll_cdesc(&t, &q), ==, req_id);

    ena_tx_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Multi-descriptor packet -> a single completion                      */
/* ------------------------------------------------------------------ */

/*
 * A single packet spanning three buffers: first (comp_req + req_id, no last),
 * a middle data descriptor, then last. The device must accumulate all three
 * descriptors as one packet and post exactly ONE completion carrying the first
 * descriptor's req_id -- not one completion per descriptor (tx-path.md §1, §4).
 */
static void test_tx_multi_buffer_single_completion(void *obj, void *data,
                                                   QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    uint64_t b0, b1, b2;
    const uint16_t req_id = 11;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                             ENA_TX_TEST_DEPTH, &q);
    b0 = ena_tx_alloc_buf(&t, ENA_TX_TEST_PKT_LEN);
    b1 = ena_tx_alloc_buf(&t, ENA_TX_TEST_PKT_LEN);
    b2 = ena_tx_alloc_buf(&t, ENA_TX_TEST_PKT_LEN);

    ena_tx_post_data(&t, &q, b0, ENA_TX_TEST_PKT_LEN, req_id,
                     /*first=*/true, /*last=*/false, /*comp_req=*/true);
    ena_tx_post_data(&t, &q, b1, ENA_TX_TEST_PKT_LEN, /*req_id=*/0,
                     /*first=*/false, /*last=*/false, /*comp_req=*/false);
    ena_tx_post_data(&t, &q, b2, ENA_TX_TEST_PKT_LEN, /*req_id=*/0,
                     /*first=*/false, /*last=*/true, /*comp_req=*/false);
    ena_tx_doorbell(&t, &q);

    g_assert_cmpuint(ena_tx_poll_cdesc(&t, &q), ==, req_id);

    /* Exactly one completion for the whole packet. */
    ena_tx_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Several packets behind one doorbell -> a completion each            */
/* ------------------------------------------------------------------ */

/*
 * The driver enqueues a whole burst and rings the doorbell once at the end
 * (tx-path.md §2). The device must process every SQ entry up to the tail and
 * post one completion per packet. Completions are matched to packets by req_id
 * (the driver does not assume CQ order equals submission order, tx-path.md §6),
 * so we assert the multiset of returned req_ids equals the submitted set.
 */
static void test_tx_burst_one_doorbell(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    uint64_t buf;
    const uint16_t req_ids[] = { 3, 1, 2 };
    const int n = ARRAY_SIZE(req_ids);
    bool seen[16] = { false };
    int i;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                             ENA_TX_TEST_DEPTH, &q);
    buf = ena_tx_alloc_buf(&t, ENA_TX_TEST_PKT_LEN);

    for (i = 0; i < n; i++) {
        ena_tx_post_packet(&t, &q, buf, ENA_TX_TEST_PKT_LEN, req_ids[i]);
    }
    ena_tx_doorbell(&t, &q);

    for (i = 0; i < n; i++) {
        uint16_t rid = ena_tx_poll_cdesc(&t, &q);

        g_assert_cmpuint(rid, <, ARRAY_SIZE(seen));
        g_assert_false(seen[rid]); /* no duplicate completions */
        seen[rid] = true;
    }
    for (i = 0; i < n; i++) {
        g_assert_true(seen[req_ids[i]]);
    }

    /* No extra completion beyond the three packets. */
    ena_tx_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* CQ phase bit tracking across a full ring wrap                       */
/* ------------------------------------------------------------------ */

/*
 * The CQ phase bit is the completion-validity signal: the device stamps the
 * current pass's phase on each cdesc and flips it every time its CQ producer
 * wraps the ring (tx-path.md §4-§5). Using a tiny depth, complete packets one
 * at a time across more than two full laps; ena_tx_poll_cdesc asserts every
 * cdesc carries the expected phase (flipping locally on wrap), so a device that
 * fails to flip -- or flips at the wrong slot -- is caught. We also confirm the
 * expected phase actually toggled.
 */
static void test_tx_cq_phase_wrap(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    uint64_t buf;
    const uint16_t depth = 4;
    const int iters = 2 * depth + 1; /* cross two wrap boundaries */
    int i;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX, depth, &q);
    buf = ena_tx_alloc_buf(&t, ENA_TX_TEST_PKT_LEN);

    for (i = 0; i < iters; i++) {
        uint16_t req_id = i % depth; /* keep req_id < q_depth */
        uint8_t phase_before = q.cq_phase;

        ena_tx_post_packet(&t, &q, buf, ENA_TX_TEST_PKT_LEN, req_id);
        ena_tx_doorbell(&t, &q);

        /* ena_tx_poll_cdesc asserts cdesc phase == expected. */
        g_assert_cmpuint(ena_tx_poll_cdesc(&t, &q), ==, req_id);

        /* The expected phase must flip exactly on the ring-wrap boundary. */
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

static void ena_register_nodes(void)
{
    ena_qos_node_register("ena-txpath");

    qos_add_test("txpath/single-buffer", "ena-txpath",
                 test_tx_single_buffer, NULL);
    qos_add_test("txpath/multi-buffer-single-completion", "ena-txpath",
                 test_tx_multi_buffer_single_completion, NULL);
    qos_add_test("txpath/burst-one-doorbell", "ena-txpath",
                 test_tx_burst_one_doorbell, NULL);
    qos_add_test("txpath/cq-phase-wrap", "ena-txpath",
                 test_tx_cq_phase_wrap, NULL);
}

libqos_init(ena_register_nodes);
