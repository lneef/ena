/*
 * ENA datapath IO-queue setup qtests (control path, polling mode).
 *
 * Covers building and tearing down a datapath queue pair through the admin
 * queue (docs/wiki/queue-setup.md): CREATE_CQ then CREATE_SQ ordering, the
 * device-assigned cq_idx/sq_idx handles and per-SQ doorbell offset, the
 * CQ-before-SQ pairing constraint, distinct handles across queues, slot reuse
 * after destruction, and the DESTROY_SQ/DESTROY_CQ teardown.
 *
 * Host placement only (no LLQ) and polling only (msix_vector = -1, no IRQ),
 * matching the assumptions of the current device emulation. Admin submit /
 * complete mechanics live in ena_admin_test.c; the create/destroy command
 * framing and queue-pair helpers in ena_test_queue.h.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#include "ena_test_queue.h"

#define ENA_QUEUE_TEST_DEPTH    256

/* A device-assigned index must be a plausible IO-queue handle. */
static void ena_assert_idx_sane(uint16_t idx)
{
    g_assert_cmpuint(idx, <, 256);
}

/* A returned SQ doorbell offset must be a non-zero, 4-byte-aligned BAR0
 * offset past the fixed register block (queue-setup.md §4). */
static void ena_assert_db_sane(uint32_t off)
{
    g_assert_cmpuint(off, !=, 0);
    g_assert_cmpuint(off & 0x3, ==, 0);
    g_assert_cmpuint(off, >=, 0x100);
}

static void ena_assert_ready(ENATestCtx *t)
{
    uint32_t sts = ena_reg_read(t, ENA_REGS_DEV_STS_OFF);

    g_assert_cmphex(sts & ENA_REGS_DEV_STS_READY_MASK, ==,
                    ENA_REGS_DEV_STS_READY_MASK);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_FATAL_ERROR_MASK, ==, 0);
}

/* ------------------------------------------------------------------ */
/* Create / destroy round-trip                                         */
/* ------------------------------------------------------------------ */

/*
 * Create a host-placement Tx queue pair: CREATE_CQ then CREATE_SQ must both
 * succeed, return sane device-assigned indices and a usable SQ doorbell
 * offset, leave the device healthy, and tear down cleanly.
 */
static void test_create_destroy_tx(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;

    ena_bringup(&t, obj, alloc);

    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                             ENA_QUEUE_TEST_DEPTH, &q);

    ena_assert_idx_sane(q.cq_idx);
    ena_assert_idx_sane(q.sq_idx);
    ena_assert_db_sane(q.sq_doorbell_off);
    ena_assert_ready(&t);

    ena_destroy_io_queue(&t, &q);
    ena_assert_ready(&t);
}

/* Same for a host-placement Rx queue pair (wider Rx cdesc). */
static void test_create_destroy_rx(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;

    ena_bringup(&t, obj, alloc);

    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_RX,
                             ENA_QUEUE_TEST_DEPTH, &q);

    ena_assert_idx_sane(q.cq_idx);
    ena_assert_idx_sane(q.sq_idx);
    ena_assert_db_sane(q.sq_doorbell_off);
    ena_assert_ready(&t);

    ena_destroy_io_queue(&t, &q);
    ena_assert_ready(&t);
}

/* ------------------------------------------------------------------ */
/* CQ-before-SQ pairing constraint (queue-setup.md §2)                 */
/* ------------------------------------------------------------------ */

/*
 * CREATE_SQ must reference a CQ that was already created. A CREATE_SQ naming a
 * CQ index that was never created must fail, while one naming the freshly
 * created CQ must succeed and bind to it.
 */
static void test_sq_requires_cq(void *obj, void *data,
                                QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENACreateCqResp cq_resp;
    ENACreateSqResp sq_resp;
    uint64_t cq_base, sq_base;
    uint16_t cq_idx;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    cq_base = guest_alloc(alloc, ENA_QUEUE_TEST_DEPTH * ENA_TX_CDESC_SIZE);
    sq_base = guest_alloc(alloc, ENA_QUEUE_TEST_DEPTH * ENA_TX_DESC_SIZE);
    qtest_memset(t.qts, cq_base, 0, ENA_QUEUE_TEST_DEPTH * ENA_TX_CDESC_SIZE);
    qtest_memset(t.qts, sq_base, 0, ENA_QUEUE_TEST_DEPTH * ENA_TX_DESC_SIZE);

    /* CREATE_SQ before any CQ exists must be rejected. */
    status = ena_try_create_sq_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                                    /*cq_idx=*/0, ENA_QUEUE_TEST_DEPTH,
                                    sq_base, &sq_resp);
    g_assert_cmpuint(status, !=, ENA_ADMIN_SUCCESS);

    /* Now create the CQ; the device echoes the granted depth. */
    status = ena_try_create_cq(&t, ENA_QUEUE_TEST_DEPTH, ENA_TX_CDESC_SIZE,
                               cq_base, &cq_resp);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(cq_resp.cq_actual_depth), ==,
                     ENA_QUEUE_TEST_DEPTH);
    cq_idx = le16_to_cpu(cq_resp.cq_idx);

    /* CREATE_SQ naming a non-existent CQ index still fails. */
    status = ena_try_create_sq_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                                    /*cq_idx=*/cq_idx + 0x20,
                                    ENA_QUEUE_TEST_DEPTH, sq_base, &sq_resp);
    g_assert_cmpuint(status, !=, ENA_ADMIN_SUCCESS);

    /* CREATE_SQ against the real CQ succeeds. */
    status = ena_try_create_sq_host(&t, ENA_ADMIN_SQ_DIRECTION_TX, cq_idx,
                                    ENA_QUEUE_TEST_DEPTH, sq_base, &sq_resp);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    ena_assert_db_sane(le32_to_cpu(sq_resp.sq_doorbell_offset));
}

/* ------------------------------------------------------------------ */
/* Distinct handles across simultaneously-live queues                  */
/* ------------------------------------------------------------------ */

/*
 * Two queue pairs created at once must get distinct CQ/SQ indices and distinct
 * SQ doorbell offsets, so the driver can drive each independently.
 */
static void test_distinct_handles(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue tx, rx;

    ena_bringup(&t, obj, alloc);

    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                             ENA_QUEUE_TEST_DEPTH, &tx);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_RX,
                             ENA_QUEUE_TEST_DEPTH, &rx);

    g_assert_cmpuint(tx.cq_idx, !=, rx.cq_idx);
    g_assert_cmpuint(tx.sq_idx, !=, rx.sq_idx);
    g_assert_cmpuint(tx.sq_doorbell_off, !=, rx.sq_doorbell_off);
    ena_assert_ready(&t);

    ena_destroy_io_queue(&t, &rx);
    ena_destroy_io_queue(&t, &tx);
}

/* ------------------------------------------------------------------ */
/* Destroying an unknown queue is rejected                             */
/* ------------------------------------------------------------------ */

/*
 * DESTROY_SQ / DESTROY_CQ for handles that were never created (or already
 * destroyed) must fail rather than silently succeed.
 */
static void test_destroy_unknown(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    ENATestCtx t;

    ena_bringup(&t, obj, alloc);

    g_assert_cmpuint(ena_try_destroy_sq(&t, /*sq_idx=*/7,
                                        ENA_ADMIN_SQ_DIRECTION_TX), !=,
                     ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(ena_try_destroy_cq(&t, /*cq_idx=*/7), !=,
                     ENA_ADMIN_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Slot reuse after destruction                                        */
/* ------------------------------------------------------------------ */

/*
 * After a queue pair is destroyed the device must be able to create another:
 * its handles are freed for reuse. A second create must succeed and the now
 * destroyed handles must no longer be destroyable.
 */
static void test_recreate_after_destroy(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;
    uint16_t old_sq, old_cq;

    ena_bringup(&t, obj, alloc);

    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                             ENA_QUEUE_TEST_DEPTH, &q);
    old_sq = q.sq_idx;
    old_cq = q.cq_idx;
    ena_destroy_io_queue(&t, &q);

    /* The destroyed handles are gone. */
    g_assert_cmpuint(ena_try_destroy_sq(&t, old_sq,
                                        ENA_ADMIN_SQ_DIRECTION_TX), !=,
                     ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(ena_try_destroy_cq(&t, old_cq), !=, ENA_ADMIN_SUCCESS);

    /* A fresh create succeeds. */
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                             ENA_QUEUE_TEST_DEPTH, &q);
    ena_assert_idx_sane(q.sq_idx);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* SQ doorbell acceptance                                              */
/* ------------------------------------------------------------------ */

/*
 * Writing the SQ tail to the doorbell offset the device returned must be
 * accepted without fatal error (queue-setup.md §4). No descriptors are posted
 * here — this only validates that the per-SQ doorbell register is live.
 */
static void test_sq_doorbell_accepted(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAQueue q;

    ena_bringup(&t, obj, alloc);

    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_TX,
                             ENA_QUEUE_TEST_DEPTH, &q);

    ena_reg_write(&t, q.sq_doorbell_off, 0);
    ena_assert_ready(&t);

    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */

static void ena_register_nodes(void)
{
    ena_qos_node_register("ena-queue");

    qos_add_test("queue/create-destroy-tx", "ena-queue",
                 test_create_destroy_tx, NULL);
    qos_add_test("queue/create-destroy-rx", "ena-queue",
                 test_create_destroy_rx, NULL);
    qos_add_test("queue/sq-requires-cq", "ena-queue",
                 test_sq_requires_cq, NULL);
    qos_add_test("queue/distinct-handles", "ena-queue",
                 test_distinct_handles, NULL);
    qos_add_test("queue/destroy-unknown", "ena-queue",
                 test_destroy_unknown, NULL);
    qos_add_test("queue/recreate-after-destroy", "ena-queue",
                 test_recreate_after_destroy, NULL);
    qos_add_test("queue/sq-doorbell-accepted", "ena-queue",
                 test_sq_doorbell_accepted, NULL);
}

libqos_init(ena_register_nodes);
