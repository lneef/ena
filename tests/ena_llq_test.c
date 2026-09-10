/*
 * QTest testcase for the ENA NIC: low latency queues (TX submission queues
 * placed in device memory, BAR 2).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "tests/ena_qos.h"
#include "tests/ena_tx_util.h"

#define NO_VECTOR   0xffffffff
#define LINE_DESCS  8
#define LARGE_LINE  256
#define LARGE_HDR   (LARGE_LINE - ENA_LLQ_HEADER_OFF)
#define LARGE_DESCS (LARGE_LINE / ENA_TX_DESC_SIZE)

typedef struct EnaLlqLine {
    uint8_t bytes[ENA_LLQ_LINE_SIZE];
} EnaLlqLine;

static void line_put_desc(EnaLlqLine *l, unsigned slot, const void *desc)
{
    g_assert(slot < 2);
    memcpy(l->bytes + slot * ENA_TX_DESC_SIZE, desc, ENA_TX_DESC_SIZE);
}

static void line_put_header(EnaLlqLine *l, const void *hdr, size_t len)
{
    g_assert(len <= ENA_LLQ_LINE_SIZE - ENA_LLQ_HEADER_OFF);
    memcpy(l->bytes + ENA_LLQ_HEADER_OFF, hdr, len);
}

/* descriptors after the first two go to following lines, eight per line */
static void spill_put_desc(EnaLlqLine *l, unsigned slot, const void *desc)
{
    g_assert(slot < LINE_DESCS);
    memcpy(l->bytes + slot * ENA_TX_DESC_SIZE, desc, ENA_TX_DESC_SIZE);
}

static void expect_frame(int fd, const uint8_t *frame, size_t len)
{
    uint8_t rx[4096];
    ssize_t got = ena_backend_recv(fd, rx, sizeof(rx));

    g_assert_cmpint(got, ==, len);
    g_assert_cmpmem(rx, len, frame, len);
}

static void setup(QEna *d, EnaTxQueue *q, uint16_t depth)
{
    ena_bringup(d);
    ena_tx_enable_llq(d);
    ena_txq_create(d, q, depth, 2, NO_VECTOR, true);
    g_assert_cmpuint(q->llq_off, ==, q->sq_idx * 1024 * ENA_LLQ_LINE_SIZE);
}

/* whole frame pushed as header: one descriptor with length 0 */
static void test_header_only(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    EnaLlqLine l = {};
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[96];
    size_t len = ena_build_eth(frame, 96 - ETH_HLEN);

    setup(d, &q, 1024);
    ena_tx_desc_fill(&dsc, 0, 0, 5, ENA_ETH_IO_TX_DESC_FIRST_MASK |
                     ENA_ETH_IO_TX_DESC_LAST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, len);
    line_put_desc(&l, 0, &dsc);
    line_put_header(&l, frame, len);
    ena_txq_push_line(d, &q, &l);
    ena_txq_doorbell(d, &q);

    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 5);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 1);
    g_assert_cmpuint(le16_to_cpu(c.sub_qid), ==, q.sq_idx);
}

/* pushed header followed by a host buffer */
static void test_header_and_buffer(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    EnaLlqLine l = {};
    struct ena_eth_io_tx_meta_desc m;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[600];
    size_t len = ena_build_eth(frame, 600 - ETH_HLEN);
    uint64_t buf = guest_alloc(alloc, len);

    setup(d, &q, 1024);
    qtest_memwrite(d->dev.bus->qts, buf + 96, frame + 96, len - 96);

    /* meta first (as the driver does with meta caching disabled) */
    ena_tx_meta_fill(&m, 0, 20, 14, 5);
    line_put_desc(&l, 0, &m);
    ena_tx_desc_fill(&dsc, buf + 96, len - 96, 21, ENA_ETH_IO_TX_DESC_LAST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, 96);
    line_put_desc(&l, 1, &dsc);
    line_put_header(&l, frame, 96);
    ena_txq_push_line(d, &q, &l);
    ena_txq_doorbell(d, &q);

    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 21);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 1);
}

/* meta + three buffers: descriptors spill into a second line */
static void test_spill_lines(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    EnaLlqLine l0 = {}, l1 = {};
    struct ena_eth_io_tx_meta_desc m;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[1000];
    size_t len = ena_build_eth(frame, 1000 - ETH_HLEN);
    uint64_t buf = guest_alloc(alloc, len);

    setup(d, &q, 1024);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    ena_tx_meta_fill(&m, 0, 20, 14, 5);
    line_put_desc(&l0, 0, &m);
    ena_tx_desc_fill(&dsc, buf + 64, 300, 77, ENA_ETH_IO_TX_DESC_COMP_REQ_MASK,
                     0, 64);
    line_put_desc(&l0, 1, &dsc);
    line_put_header(&l0, frame, 64);
    ena_tx_desc_fill(&dsc, buf + 364, 400, 0, 0, 0, 0);
    spill_put_desc(&l1, 0, &dsc);
    ena_tx_desc_fill(&dsc, buf + 764, len - 764, 0, ENA_ETH_IO_TX_DESC_LAST_MASK, 0, 0);
    spill_put_desc(&l1, 1, &dsc);
    ena_txq_push_line(d, &q, &l0);
    ena_txq_push_line(d, &q, &l1);
    ena_txq_doorbell(d, &q);

    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 77);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 2);
    g_assert_false(ena_txq_poll_cdesc(d, &q, &c));
}

/* two packets in one doorbell, then wrap of a 16-line ring */
static void test_burst_and_wrap(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[80];
    size_t len = ena_build_eth(frame, 80 - ETH_HLEN);
    int i;

    setup(d, &q, 16);
    for (i = 0; i < 40; i += 2) {
        EnaLlqLine l = {};
        int k;

        for (k = 0; k < 2; k++) {
            frame[ETH_HLEN] = i + k;
            ena_tx_desc_fill(&dsc, 0, 0, i + k, ENA_ETH_IO_TX_DESC_FIRST_MASK |
                             ENA_ETH_IO_TX_DESC_LAST_MASK |
                             ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, len);
            line_put_desc(&l, 0, &dsc);
            line_put_header(&l, frame, len);
            ena_txq_push_line(d, &q, &l);
        }
        ena_txq_doorbell(d, &q);
        for (k = 0; k < 2; k++) {
            uint8_t rx[128];

            g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)),
                            ==, len);
            g_assert_cmpuint(rx[ETH_HLEN], ==, i + k);
            g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
            g_assert_cmpuint(le16_to_cpu(c.req_id), ==, i + k);
            g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, (uint16_t)(i + k + 1));
            g_assert_cmpuint(c.flags & ENA_ETH_IO_TX_CDESC_PHASE_MASK, ==,
                             (((i + k) / 16) & 1) == 0);
        }
    }
    g_assert_false(ena_txq_poll_cdesc(d, &q, &c));
}

/* a header longer than the 96 bytes an entry can hold is rejected */
static void test_header_too_long(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    EnaLlqLine l = {};
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t rx[128];

    setup(d, &q, 1024);
    ena_tx_desc_fill(&dsc, 0, 0, 1, ENA_ETH_IO_TX_DESC_FIRST_MASK |
                     ENA_ETH_IO_TX_DESC_LAST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, 120);
    line_put_desc(&l, 0, &dsc);
    ena_txq_push_line(d, &q, &l);
    ena_txq_doorbell(d, &q);
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, -1);
    /* the entry is consumed and completed, the frame is dropped */
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 1);
}

/* 256-byte entries: header up to 224 bytes, 16 descriptors per spill entry */
static void setup_large(QEna *d, EnaTxQueue *q)
{
    struct ena_admin_get_feat_resp resp;

    ena_bringup(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LLQ, 1, 0, 0, &resp), ==,
                    ENA_ADMIN_SUCCESS);
    g_assert_cmphex(le16_to_cpu(resp.u.llq.entry_size_ctrl_supported) &
                    ENA_ADMIN_LIST_ENTRY_SIZE_256B, !=, 0);
    ena_tx_enable_llq_size(d, ENA_ADMIN_LIST_ENTRY_SIZE_256B);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LLQ, 1, 0, 0, &resp), ==,
                    ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(resp.u.llq.entry_size_ctrl_enabled), ==,
                     ENA_ADMIN_LIST_ENTRY_SIZE_256B);
    /* no wide BAR: 1024 entries of 256 bytes do not fit, the driver halves */
    g_assert_cmpuint(le16_to_cpu(resp.u.llq.max_wide_llq_depth), ==, 0);
    ena_txq_create(d, q, 512, 2, NO_VECTOR, true);
    q->line_size = LARGE_LINE;
}

static void test_large_depth_limit(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_acq_create_cq_resp_desc cq;
    struct ena_admin_acq_create_sq_resp_desc sq;
    uint64_t ring = guest_alloc(alloc, 1024 * 16);

    ena_bringup(d);
    ena_tx_enable_llq_size(d, ENA_ADMIN_LIST_ENTRY_SIZE_256B);
    g_assert_cmpint(ena_create_cq(d, 1024, 2, NO_VECTOR, ring, &cq), ==,
                    ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_DEV,
                                  le16_to_cpu(cq.cq_idx), 1024, 0, &sq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_DEV,
                                  le16_to_cpu(cq.cq_idx), 512, 0, &sq), ==,
                    ENA_ADMIN_SUCCESS);
}

static void test_large_header_only(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t line[LARGE_LINE] = {};
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[LARGE_HDR];
    size_t len = ena_build_eth(frame, LARGE_HDR - ETH_HLEN);

    setup_large(d, &q);
    ena_tx_desc_fill(&dsc, 0, 0, 11, ENA_ETH_IO_TX_DESC_FIRST_MASK |
                     ENA_ETH_IO_TX_DESC_LAST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, len);
    memcpy(line, &dsc, ENA_TX_DESC_SIZE);
    memcpy(line + ENA_LLQ_HEADER_OFF, frame, len);
    ena_txq_push_line(d, &q, line);
    ena_txq_doorbell(d, &q);

    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 11);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 1);
}

/* meta + 16 buffers: 2 slots in the first entry, 15 in one 256-byte spill entry */
static void test_large_spill(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t l0[LARGE_LINE] = {}, l1[LARGE_LINE] = {};
    struct ena_eth_io_tx_meta_desc m;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[LARGE_HDR + 16 * 20];
    size_t len = ena_build_eth(frame, sizeof(frame) - ETH_HLEN);
    uint64_t buf = guest_alloc(alloc, len);
    int i;

    setup_large(d, &q);
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    ena_tx_meta_fill(&m, 0, 20, 14, 5);
    memcpy(l0, &m, ENA_TX_DESC_SIZE);
    ena_tx_desc_fill(&dsc, buf + LARGE_HDR, 20, 99, ENA_ETH_IO_TX_DESC_COMP_REQ_MASK,
                     0, LARGE_HDR);
    memcpy(l0 + ENA_TX_DESC_SIZE, &dsc, ENA_TX_DESC_SIZE);
    memcpy(l0 + ENA_LLQ_HEADER_OFF, frame, LARGE_HDR);
    for (i = 1; i < 16; i++) {
        ena_tx_desc_fill(&dsc, buf + LARGE_HDR + i * 20, 20, 0,
                         i == 15 ? ENA_ETH_IO_TX_DESC_LAST_MASK : 0, 0, 0);
        memcpy(l1 + (i - 1) * ENA_TX_DESC_SIZE, &dsc, ENA_TX_DESC_SIZE);
    }
    ena_txq_push_line(d, &q, l0);
    ena_txq_push_line(d, &q, l1);
    ena_txq_doorbell(d, &q);

    expect_frame(ena_backend_fd(data), frame, len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 99);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 2);
    g_assert_false(ena_txq_poll_cdesc(d, &q, &c));
}

/* a 225-byte header does not fit a 256-byte entry */
static void test_large_header_too_long(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t line[LARGE_LINE] = {};
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t rx[256];

    setup_large(d, &q);
    ena_tx_desc_fill(&dsc, 0, 0, 1, ENA_ETH_IO_TX_DESC_FIRST_MASK |
                     ENA_ETH_IO_TX_DESC_LAST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, LARGE_HDR + 1);
    memcpy(line, &dsc, ENA_TX_DESC_SIZE);
    ena_txq_push_line(d, &q, line);
    ena_txq_doorbell(d, &q);
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, -1);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
}

static void register_ena_llq_test(void)
{
    QOSGraphTestOptions opts = {
        .before = ena_test_before,
    };

    qos_add_test("llq/header-only", "ena", test_header_only, &opts);
    qos_add_test("llq/header-and-buffer", "ena", test_header_and_buffer, &opts);
    qos_add_test("llq/spill-lines", "ena", test_spill_lines, &opts);
    qos_add_test("llq/burst-and-wrap", "ena", test_burst_and_wrap, &opts);
    qos_add_test("llq/header-too-long", "ena", test_header_too_long, &opts);
    qos_add_test("llq/large-depth-limit", "ena", test_large_depth_limit, &opts);
    qos_add_test("llq/large-header-only", "ena", test_large_header_only, &opts);
    qos_add_test("llq/large-spill", "ena", test_large_spill, &opts);
    qos_add_test("llq/large-header-too-long", "ena", test_large_header_too_long,
                 &opts);
}

libqos_init(register_ena_llq_test);
