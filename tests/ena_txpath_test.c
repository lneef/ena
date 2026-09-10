/*
 * QTest testcase for the ENA NIC: TX datapath with host-placed submission
 * queues, completions and completion interrupts.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "tests/ena_qos.h"
#include "tests/ena_tx_util.h"

#define TX_VECTOR   1
#define NO_VECTOR   0xffffffff
#define SINGLE      (ENA_ETH_IO_TX_DESC_FIRST_MASK | ENA_ETH_IO_TX_DESC_LAST_MASK | \
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK)

/* one frame in one descriptor, completion requested */
static void send_single(QEna *d, EnaTxQueue *q, uint64_t buf, size_t len,
                        uint16_t req_id)
{
    struct ena_eth_io_tx_desc dsc;

    ena_tx_desc_fill(&dsc, buf, len, req_id, SINGLE, 0, 0);
    ena_txq_push(d, q, &dsc);
    ena_txq_doorbell(d, q);
}

static void expect_frame(int fd, const uint8_t *frame, size_t len)
{
    uint8_t rx[4096];
    ssize_t got = ena_backend_recv(fd, rx, sizeof(rx));

    g_assert_cmpint(got, ==, len);
    g_assert_cmpmem(rx, len, frame, len);
}

static void test_single_frame(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[128];
    size_t len = ena_build_eth(frame, 64);
    uint64_t buf = guest_alloc(alloc, len);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);
    g_assert_false(ena_txq_poll_cdesc(d, &q, &c));

    send_single(d, &q, buf, len, 7);
    expect_frame(ena_backend_fd(data), frame, len);

    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 7);
    g_assert_cmpuint(c.status, ==, 0);
    g_assert_cmphex(c.flags & ENA_ETH_IO_TX_CDESC_MBZ6_MASK, ==, 0);
    g_assert_cmpuint(le16_to_cpu(c.sub_qid), ==, q.sq_idx);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 1);
    g_assert_false(ena_txq_poll_cdesc(d, &q, &c));
}

static void test_multi_desc(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[300];
    size_t len = ena_build_eth(frame, 300 - ETH_HLEN);
    uint64_t buf = guest_alloc(alloc, len);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    /* three buffers: 14 + 100 + rest; completion requested on the first */
    ena_tx_desc_fill(&dsc, buf, 14, 1000, ENA_ETH_IO_TX_DESC_FIRST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, 0);
    ena_txq_push(d, &q, &dsc);
    ena_tx_desc_fill(&dsc, buf + 14, 100, 0, 0, 0, 0);
    ena_txq_push(d, &q, &dsc);
    ena_tx_desc_fill(&dsc, buf + 114, len - 114, 0, ENA_ETH_IO_TX_DESC_LAST_MASK, 0, 0);
    ena_txq_push(d, &q, &dsc);
    ena_txq_doorbell(d, &q);

    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 1000);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 3);
}

static void test_meta_and_no_completion(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_meta_desc m;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[128];
    size_t len = ena_build_eth(frame, 50);
    uint64_t buf = guest_alloc(alloc, len);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    /* meta descriptor carries FIRST, the data descriptor does not */
    ena_tx_meta_fill(&m, 1400, 20, 14, 5);
    ena_txq_push(d, &q, &m);
    ena_tx_desc_fill(&dsc, buf, len, 3, ENA_ETH_IO_TX_DESC_LAST_MASK, 0, 0);
    ena_txq_push(d, &q, &dsc);
    ena_txq_doorbell(d, &q);
    expect_frame(ena_backend_fd(data), frame, len);
    /* no comp_req: frame sent, no completion written */
    g_assert_false(ena_txq_poll_cdesc(d, &q, &c));

    /* the cached meta applies to the next packet, which requests completion */
    send_single(d, &q, buf, len, 4);
    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 4);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 3);
}

static void test_ring_wrap(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[128];
    size_t len = ena_build_eth(frame, 60);
    uint64_t buf = guest_alloc(alloc, len);
    int i;

    ena_bringup(d);
    ena_txq_create(d, &q, 16, 4, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    for (i = 0; i < 40; i++) {
        send_single(d, &q, buf, len, i & 15);
        expect_frame(ena_backend_fd(data), frame, len);
        g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
        g_assert_cmpuint(le16_to_cpu(c.req_id), ==, i & 15);
        g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, (uint16_t)(i + 1));
        /* phase bit: 1 for the first 16 entries, then alternating */
        g_assert_cmpuint(c.flags & ENA_ETH_IO_TX_CDESC_PHASE_MASK, ==,
                         ((i / 16) & 1) == 0);
    }
    g_assert_false(ena_txq_poll_cdesc(d, &q, &c));
}

static void test_interrupt(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[128];
    size_t len = ena_build_eth(frame, 60);
    uint64_t buf = guest_alloc(alloc, len);

    ena_bringup(d);
    ena_msix_setup(d, TX_VECTOR);
    ena_txq_create(d, &q, 1024, 2, TX_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    /* masked at creation: completion is written, no interrupt */
    send_single(d, &q, buf, len, 1);
    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_false(ena_msix_fired(d, TX_VECTOR));

    /* unmask with a pending completion fires immediately */
    ena_txq_unmask(d, &q, 0, false);
    g_assert_true(ena_msix_fired(d, TX_VECTOR));
    ena_msix_clear(d, TX_VECTOR);

    /* delivery auto-masks: the next completion waits for the next unmask */
    send_single(d, &q, buf, len, 2);
    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_false(ena_msix_fired(d, TX_VECTOR));
    ena_txq_unmask(d, &q, 0, false);
    g_assert_true(ena_msix_fired(d, TX_VECTOR));
    ena_msix_clear(d, TX_VECTOR);

    /* unmasked and idle: a completion fires right away */
    ena_txq_unmask(d, &q, 0, false);
    g_assert_false(ena_msix_fired(d, TX_VECTOR));
    send_single(d, &q, buf, len, 3);
    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_msix_fired(d, TX_VECTOR));
}

static void test_interrupt_moderation(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t frame[128];
    size_t len = ena_build_eth(frame, 60);
    uint64_t buf = guest_alloc(alloc, len);

    ena_bringup(d);
    ena_msix_setup(d, TX_VECTOR);
    ena_txq_create(d, &q, 1024, 2, TX_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    /* tx delay of 200 us: the interrupt is held back until the delay expires */
    ena_txq_unmask(d, &q, 200, true);
    send_single(d, &q, buf, len, 1);
    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_false(ena_msix_fired(d, TX_VECTOR));
    qtest_clock_step(d->dev.bus->qts, 100 * 1000);
    g_assert_false(ena_msix_fired(d, TX_VECTOR));
    qtest_clock_step(d->dev.bus->qts, 101 * 1000);
    g_assert_true(ena_msix_fired(d, TX_VECTOR));
    ena_msix_clear(d, TX_VECTOR);

    /* no_moderation_update keeps the programmed delay */
    ena_txq_unmask(d, &q, 0, false);
    send_single(d, &q, buf, len, 2);
    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_false(ena_msix_fired(d, TX_VECTOR));
    qtest_clock_step(d->dev.bus->qts, 201 * 1000);
    g_assert_true(ena_msix_fired(d, TX_VECTOR));
    ena_msix_clear(d, TX_VECTOR);

    /* a moderation update to zero delay restores immediate delivery */
    ena_txq_unmask(d, &q, 0, true);
    send_single(d, &q, buf, len, 3);
    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_msix_fired(d, TX_VECTOR));
}

static void test_no_vector(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[128];
    size_t len = ena_build_eth(frame, 60);
    uint64_t buf = guest_alloc(alloc, len);

    ena_bringup(d);
    ena_msix_setup(d, TX_VECTOR);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);
    ena_txq_unmask(d, &q, 0, false);
    send_single(d, &q, buf, len, 1);
    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_false(ena_msix_fired(d, TX_VECTOR));
}

static void test_stats(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_admin_aq_get_stats_cmd cmd = {};
    struct ena_admin_acq_get_stats_resp resp;
    uint8_t frame[128];
    size_t len = ena_build_eth(frame, 86);
    uint64_t buf = guest_alloc(alloc, len);
    int i;

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);
    for (i = 0; i < 5; i++) {
        send_single(d, &q, buf, len, i);
        expect_frame(ena_backend_fd(data), frame, len);
    }

    cmd.aq_common_descriptor.opcode = ENA_ADMIN_GET_STATS;
    cmd.type = ENA_ADMIN_GET_STATS_TYPE_BASIC;
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &resp, sizeof(resp)),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le32_to_cpu(resp.u.basic_stats.tx_pkts_low), ==, 5);
    g_assert_cmpuint(le32_to_cpu(resp.u.basic_stats.tx_bytes_low), ==, 5 * len);
    g_assert_cmpuint(le32_to_cpu(resp.u.basic_stats.rx_pkts_low), ==, 0);
}

static void test_malformed(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[128];
    uint8_t rx[128];
    size_t len = ena_build_eth(frame, 60);
    uint64_t buf = guest_alloc(alloc, len);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    /* a descriptor without FIRST and without a meta descriptor is dropped */
    ena_tx_desc_fill(&dsc, buf, len, 9, ENA_ETH_IO_TX_DESC_LAST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, 0);
    ena_txq_push(d, &q, &dsc);
    ena_txq_doorbell(d, &q);
    g_assert_false(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, -1);
}

/* A foreign source MAC is completed by the NIC but dropped by the fabric. */
static void test_source_mac_filter(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[128];
    uint8_t rx[128];
    size_t len = ena_build_eth(frame, 64);
    uint64_t buf = guest_alloc(alloc, len);

    memset(frame + ETH_ALEN, 0xaa, ETH_ALEN);
    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    send_single(d, &q, buf, len, 3);
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, -1);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 3);
}

/* Descriptor uses outside the driver contract are completed but not sent. */
static void test_unsupported_flags(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[128];
    uint8_t rx[128];
    size_t len = ena_build_eth(frame, 64);
    uint64_t buf = guest_alloc(alloc, len);
    int fd = ena_backend_fd(data);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    /* header_length is an LLQ field */
    ena_tx_desc_fill(&dsc, buf, len, 1, SINGLE, 0, 14);
    ena_txq_push(d, &q, &dsc);
    ena_txq_doorbell(d, &q);
    g_assert_cmpint(ena_backend_recv(fd, rx, sizeof(rx)), ==, -1);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 1);

    /* TSO needs a TCP packet and a meta descriptor with a non-zero MSS */
    ena_tx_desc_fill(&dsc, buf, len, 2, SINGLE,
                     ENA_ETH_IO_TX_DESC_TSO_EN_MASK |
                     (ENA_ETH_IO_L4_PROTO_TCP <<
                      ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT), 0);
    ena_txq_push(d, &q, &dsc);
    ena_txq_doorbell(d, &q);
    g_assert_cmpint(ena_backend_recv(fd, rx, sizeof(rx)), ==, -1);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 2);

    /* the queue keeps working */
    send_single(d, &q, buf, len, 3);
    expect_frame(fd, frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 3);
}

static void register_ena_txpath_test(void)
{
    QOSGraphTestOptions opts = {
        .before = ena_test_before,
    };

    qos_add_test("txpath/single-frame", "ena", test_single_frame, &opts);
    qos_add_test("txpath/source-mac-filter", "ena", test_source_mac_filter,
                 &opts);
    qos_add_test("txpath/unsupported-flags", "ena", test_unsupported_flags,
                 &opts);
    qos_add_test("txpath/multi-desc", "ena", test_multi_desc, &opts);
    qos_add_test("txpath/meta-and-no-completion", "ena",
                 test_meta_and_no_completion, &opts);
    qos_add_test("txpath/ring-wrap", "ena", test_ring_wrap, &opts);
    qos_add_test("txpath/interrupt", "ena", test_interrupt, &opts);
    qos_add_test("txpath/interrupt-moderation", "ena",
                 test_interrupt_moderation, &opts);
    qos_add_test("txpath/no-vector", "ena", test_no_vector, &opts);
    qos_add_test("txpath/stats", "ena", test_stats, &opts);
    qos_add_test("txpath/malformed", "ena", test_malformed, &opts);
}

libqos_init(register_ena_txpath_test);
