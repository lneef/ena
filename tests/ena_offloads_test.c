/*
 * QTest testcase for the ENA NIC: TX stateless offloads (IPv4 header
 * checksum, TCP/UDP checksum, TSO). RX offload tests live in
 * ena_rxpath_test.c.
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
#define SINGLE      (ENA_ETH_IO_TX_DESC_FIRST_MASK | ENA_ETH_IO_TX_DESC_LAST_MASK | \
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK)
#define IPV4_CSUM   (ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK | ENA_ETH_IO_L3_PROTO_IPV4)
#define L4_CSUM(p)  (ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK | \
                     ((p) << ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT))

static uint16_t rd16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static size_t send_frame(QEna *d, EnaTxQueue *q, QGuestAllocator *alloc,
                         const uint8_t *frame, size_t len, uint32_t meta_ctrl)
{
    struct ena_eth_io_tx_desc dsc;
    uint64_t buf = guest_alloc(alloc, len);

    qtest_memwrite(d->dev.bus->qts, buf, frame, len);
    ena_tx_desc_fill(&dsc, buf, len, 1, SINGLE, meta_ctrl, 0);
    ena_txq_push(d, q, &dsc);
    ena_txq_doorbell(d, q);
    return len;
}

static void test_ipv4_udp_csum(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t frame[256], rx[256];
    size_t len = ena_build_ipv4(frame, IP_PROTO_UDP, 100);
    const uint8_t *ip = rx + ETH_HLEN;
    const uint8_t *udp = ip + sizeof(struct ip_header);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);

    send_frame(d, &q, alloc, frame, len, IPV4_CSUM | L4_CSUM(ENA_ETH_IO_L4_PROTO_UDP));
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, len);
    /* checksum fields were zero in the source frame and are now valid */
    g_assert_cmphex(rd16(ip + 10), !=, 0);
    g_assert_cmphex(ena_ipv4_csum(rx), ==, 0);
    g_assert_cmphex(rd16(udp + 6), !=, 0);
    g_assert_cmphex(ena_l4_csum(rx, len), ==, 0);
    /* payload untouched */
    g_assert_cmpmem(rx + ETH_HLEN + 28, len - ETH_HLEN - 28,
                    frame + ETH_HLEN + 28, len - ETH_HLEN - 28);
}

static void test_ipv4_tcp_csum(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t frame[256], rx[256];
    size_t len = ena_build_ipv4(frame, IP_PROTO_TCP, 100);
    const uint8_t *ip = rx + ETH_HLEN;
    const uint8_t *tcp = ip + sizeof(struct ip_header);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);

    send_frame(d, &q, alloc, frame, len, IPV4_CSUM | L4_CSUM(ENA_ETH_IO_L4_PROTO_TCP));
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, len);
    g_assert_cmphex(ena_ipv4_csum(rx), ==, 0);
    g_assert_cmphex(rd16(tcp + 16), !=, 0);
    g_assert_cmphex(ena_l4_csum(rx, len), ==, 0);
}

static void test_l3_only(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t frame[256], rx[256];
    size_t len = ena_build_ipv4(frame, IP_PROTO_UDP, 40);
    const uint8_t *udp = rx + ETH_HLEN + sizeof(struct ip_header);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);

    send_frame(d, &q, alloc, frame, len, IPV4_CSUM);
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, len);
    g_assert_cmphex(ena_ipv4_csum(rx), ==, 0);
    /* the L4 checksum stays untouched (zero) */
    g_assert_cmphex(rd16(udp + 6), ==, 0);
}

static void test_no_offload(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t frame[256], rx[256];
    size_t len = ena_build_ipv4(frame, IP_PROTO_UDP, 40);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);

    send_frame(d, &q, alloc, frame, len, 0);
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, len);
    g_assert_cmpmem(rx, len, frame, len);
}

static uint32_t tx_drops(QEna *d)
{
    struct ena_admin_aq_get_stats_cmd cmd = {};
    struct ena_admin_acq_get_stats_resp resp;

    cmd.aq_common_descriptor.opcode = ENA_ADMIN_GET_STATS;
    cmd.type = ENA_ADMIN_GET_STATS_TYPE_BASIC;
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &resp, sizeof(resp)),
                    ==, ENA_ADMIN_SUCCESS);
    return le32_to_cpu(resp.u.basic_stats.tx_drops_low);
}

/* The IPv4 checksum covers the header as written; padding is not folded in. */
static void test_padded_l3_csum(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    uint8_t frame[64] = {}, rx[64];
    size_t len = ena_build_ipv4(frame, IP_PROTO_UDP, 4);

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);

    send_frame(d, &q, alloc, frame, 60, IPV4_CSUM);
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, 60);
    g_assert_cmpuint(rd16(rx + ETH_HLEN + 2), ==, len - ETH_HLEN);
    g_assert_cmphex(ena_ipv4_csum(rx), ==, 0);
    g_assert_cmpmem(rx + ETH_HLEN + sizeof(struct ip_header), 60 - ETH_HLEN - 20,
                    frame + ETH_HLEN + sizeof(struct ip_header), 60 - ETH_HLEN - 20);
}

/* Offload bits that do not fit the frame are ignored or drop the packet. */
static void test_offload_mismatch(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_meta_desc m;
    uint8_t frame[256], rx[256];
    size_t len;

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);

    /* checksum offloads on a non-IP frame: sent verbatim */
    len = ena_build_eth(frame, 64);
    send_frame(d, &q, alloc, frame, len,
               IPV4_CSUM | L4_CSUM(ENA_ETH_IO_L4_PROTO_UDP));
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, len);
    g_assert_cmpmem(rx, len, frame, len);
    g_assert_cmpuint(tx_drops(d), ==, 0);

    /* TSO on a UDP frame is dropped and counted */
    len = ena_build_ipv4(frame, IP_PROTO_UDP, 100);
    ena_tx_meta_fill(&m, 50, sizeof(struct ip_header), ETH_HLEN, 2);
    ena_txq_push(d, &q, &m);
    send_frame(d, &q, alloc, frame, len, ENA_ETH_IO_TX_DESC_TSO_EN_MASK |
               IPV4_CSUM | L4_CSUM(ENA_ETH_IO_L4_PROTO_TCP));
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, -1);
    g_assert_cmpuint(tx_drops(d), ==, 1);

    /* a frame over the MTU without TSO is dropped and counted */
    {
        g_autofree uint8_t *big = g_malloc0(9000 + ETH_HLEN + 64);
        size_t big_len = ena_build_eth(big, 9000 + 50);

        send_frame(d, &q, alloc, big, big_len, 0);
        g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)),
                        ==, -1);
        g_assert_cmpuint(tx_drops(d), ==, 2);
    }
}

static void test_tso(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    EnaTxQueue q;
    struct ena_eth_io_tx_meta_desc m;
    struct ena_eth_io_tx_desc dsc;
    struct ena_eth_io_tx_cdesc c;
    uint8_t frame[4096], rx[4096];
    size_t payload = 2500, mss = 1000;
    size_t len = ena_build_ipv4(frame, IP_PROTO_TCP, payload);
    size_t hdr = ETH_HLEN + sizeof(struct ip_header) + sizeof(struct tcp_header);
    uint64_t buf = guest_alloc(alloc, len);
    size_t sent = 0;
    uint32_t seq0;
    int seg;

    ena_bringup(d);
    ena_txq_create(d, &q, 1024, 2, NO_VECTOR, false);
    /* initial sequence number 0x1000 */
    frame[hdr - sizeof(struct tcp_header) + 4] = 0;
    frame[hdr - sizeof(struct tcp_header) + 5] = 0;
    frame[hdr - sizeof(struct tcp_header) + 6] = 0x10;
    frame[hdr - sizeof(struct tcp_header) + 7] = 0;
    qtest_memwrite(d->dev.bus->qts, buf, frame, len);

    ena_tx_meta_fill(&m, mss, sizeof(struct ip_header), ETH_HLEN,
                     sizeof(struct tcp_header) / 4);
    ena_txq_push(d, &q, &m);
    ena_tx_desc_fill(&dsc, buf, len, 42, ENA_ETH_IO_TX_DESC_LAST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK,
                     ENA_ETH_IO_TX_DESC_TSO_EN_MASK | IPV4_CSUM |
                     L4_CSUM(ENA_ETH_IO_L4_PROTO_TCP), 0);
    ena_txq_push(d, &q, &dsc);
    ena_txq_doorbell(d, &q);

    /* three segments: 1000, 1000, 500 bytes of payload */
    for (seg = 0; seg < 3; seg++) {
        ssize_t got = ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx));
        size_t seg_payload = MIN(mss, payload - sent);
        const uint8_t *ip = rx + ETH_HLEN;
        const uint8_t *tcp = ip + sizeof(struct ip_header);

        g_assert_cmpint(got, ==, hdr + seg_payload);
        g_assert_cmpuint(rd16(ip + 2), ==, got - ETH_HLEN);
        g_assert_cmphex(ena_ipv4_csum(rx), ==, 0);
        g_assert_cmphex(ena_l4_csum(rx, got), ==, 0);
        seq0 = ((uint32_t)rd16(tcp + 4) << 16) | rd16(tcp + 6);
        g_assert_cmpuint(seq0, ==, 0x1000 + sent);
        g_assert_cmpmem(rx + hdr, seg_payload, frame + hdr + sent, seg_payload);
        sent += seg_payload;
    }
    g_assert_cmpint(ena_backend_recv(ena_backend_fd(data), rx, sizeof(rx)), ==, -1);

    /* one completion for the whole packet */
    g_assert_true(ena_txq_poll_cdesc(d, &q, &c));
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 42);
    g_assert_cmpuint(le16_to_cpu(c.sq_head_idx), ==, 2);
}

static void register_ena_offloads_test(void)
{
    QOSGraphTestOptions opts = {
        .before = ena_test_before,
    };

    qos_add_test("offloads/tx-ipv4-udp-csum", "ena", test_ipv4_udp_csum, &opts);
    qos_add_test("offloads/tx-ipv4-tcp-csum", "ena", test_ipv4_tcp_csum, &opts);
    qos_add_test("offloads/tx-l3-only", "ena", test_l3_only, &opts);
    qos_add_test("offloads/tx-no-offload", "ena", test_no_offload, &opts);
    qos_add_test("offloads/tx-tso", "ena", test_tso, &opts);
    qos_add_test("offloads/tx-padded-l3-csum", "ena", test_padded_l3_csum, &opts);
    qos_add_test("offloads/tx-offload-mismatch", "ena", test_offload_mismatch,
                 &opts);
}

libqos_init(register_ena_offloads_test);
