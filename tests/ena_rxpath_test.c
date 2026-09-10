/*
 * QTest testcase for the ENA NIC: RX datapath, checksum reporting and RSS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "net/eth.h"
#include "net/checksum.h"
#include "tests/ena_qos.h"
#include "tests/ena_tx_util.h"

#define SQ_DB_BASE      0x1000
#define CQ_UNMASK_BASE  0x2000
#define RSS_TBL_SIZE    128
#define RSS_KEY_BYTES   40
#define SETTLE_STEPS    200
#define SETTLE_NS       1000
#define MODER_DELAY_US  5000

/* Standard Toeplitz key a DPDK application hands to rte_eth_dev_configure. */
static const uint8_t rss_app_key[RSS_KEY_BYTES] = {
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
    0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
    0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
    0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
    0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
};

typedef struct RxRing {
    QEna *d;
    int fd;
    uint64_t sq_base;
    uint64_t cq_base;
    uint64_t buf_base;
    uint16_t sq_idx;
    uint16_t cq_idx;
    uint16_t depth;
    uint16_t cq_depth;
    uint8_t entry_size;
    uint32_t buf_size;
    uint16_t tail;
    uint16_t next_req;
    uint16_t cq_head;
    bool cq_phase;
} RxRing;

static QTestState *rx_qts(RxRing *r)
{
    return r->d->dev.bus->qts;
}

static uint64_t rx_alloc(RxRing *r, QGuestAllocator *alloc, size_t size)
{
    uint64_t addr = guest_alloc(alloc, size);

    g_assert(addr);
    qtest_memset(rx_qts(r), addr, 0, size);
    return addr;
}

/* Creates one RX CQ/SQ pair plus a buffer area of depth * buf_size bytes. */
static void rx_ring_init(RxRing *r, QEna *d, void *data,
                         QGuestAllocator *alloc, uint16_t depth,
                         uint16_t cq_depth, uint8_t entry_words,
                         uint32_t msix_vector, uint32_t buf_size)
{
    struct ena_admin_acq_create_cq_resp_desc cq;
    struct ena_admin_acq_create_sq_resp_desc sq;

    memset(r, 0, sizeof(*r));
    r->d = d;
    r->fd = ena_backend_fd(data);
    r->depth = depth;
    r->cq_depth = cq_depth;
    r->entry_size = entry_words * 4;
    r->buf_size = buf_size;
    r->cq_phase = true;

    r->sq_base = rx_alloc(r, alloc, depth * sizeof(struct ena_eth_io_rx_desc));
    r->cq_base = rx_alloc(r, alloc, cq_depth * r->entry_size);
    r->buf_base = rx_alloc(r, alloc, (size_t)depth * buf_size);

    g_assert_cmpint(ena_create_cq(d, cq_depth, entry_words, msix_vector,
                                  r->cq_base, &cq), ==, ENA_ADMIN_SUCCESS);
    r->cq_idx = le16_to_cpu(cq.cq_idx);
    g_assert_cmpint(ena_create_sq(d, false, ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                  r->cq_idx, depth, r->sq_base, &sq), ==,
                    ENA_ADMIN_SUCCESS);
    r->sq_idx = le16_to_cpu(sq.sq_idx);
}

/* Posts count descriptors, one buffer each, and rings the doorbell. */
static void rx_post(RxRing *r, int count)
{
    struct ena_eth_io_rx_desc desc = {};
    int i;

    for (i = 0; i < count; i++) {
        uint16_t req_id = r->next_req++ % r->depth;
        uint64_t buf = r->buf_base + (uint64_t)req_id * r->buf_size;

        desc.length = cpu_to_le16(r->buf_size);
        desc.ctrl = ENA_ETH_IO_RX_DESC_FIRST_MASK |
                    ENA_ETH_IO_RX_DESC_LAST_MASK |
                    ENA_ETH_IO_RX_DESC_COMP_REQ_MASK;
        desc.req_id = cpu_to_le16(req_id);
        desc.buff_addr_lo = cpu_to_le32(buf);
        desc.buff_addr_hi = cpu_to_le16(buf >> 32);
        qtest_memwrite(rx_qts(r),
                       r->sq_base + (r->tail & (r->depth - 1)) * sizeof(desc),
                       &desc, sizeof(desc));
        r->tail++;
    }
    ena_reg_write(r->d, SQ_DB_BASE + r->sq_idx * 4, r->tail);
}

static uint64_t rx_buf_addr(RxRing *r, uint16_t req_id)
{
    return r->buf_base + (uint64_t)req_id * r->buf_size;
}

static bool rx_cq_next(RxRing *r, struct ena_eth_io_rx_cdesc_base *c)
{
    uint64_t addr = r->cq_base +
                    (uint64_t)(r->cq_head & (r->cq_depth - 1)) * r->entry_size;

    qtest_memread(rx_qts(r), addr, c, sizeof(*c));
    if (!!(le32_to_cpu(c->status) & ENA_ETH_IO_RX_CDESC_BASE_PHASE_MASK) !=
        r->cq_phase) {
        return false;
    }
    r->cq_head++;
    if ((r->cq_head & (r->cq_depth - 1)) == 0) {
        r->cq_phase = !r->cq_phase;
    }
    return true;
}

/* Lets the QEMU main loop drain the backend socket. */
static void rx_settle(RxRing *r)
{
    int i;

    for (i = 0; i < SETTLE_STEPS; i++) {
        qtest_clock_step(rx_qts(r), SETTLE_NS);
    }
}

static void rx_wait_cdesc(RxRing *r, struct ena_eth_io_rx_cdesc_base *c)
{
    int i;

    for (i = 0; i < SETTLE_STEPS; i++) {
        if (rx_cq_next(r, c)) {
            return;
        }
        qtest_clock_step(rx_qts(r), SETTLE_NS);
    }
    g_assert_not_reached();
}

static void rx_unmask(RxRing *r, uint32_t rx_delay_us)
{
    ena_reg_write(r->d, CQ_UNMASK_BASE + r->cq_idx * 4,
                  ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK | rx_delay_us);
}

/* frame construction */

static uint32_t csum_add(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    }
    if (i < len) {
        sum += (uint32_t)p[i] << 8;
    }
    return sum;
}

static uint16_t csum_fini(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum & 0xffff;
}

typedef struct Flow {
    uint32_t src;
    uint32_t dst;
    uint16_t sport;
    uint16_t dport;
} Flow;

/* Builds an Ethernet/IPv4/UDP frame carrying payload bytes of data. */
static size_t build_udp(uint8_t *buf, const Flow *f, size_t payload)
{
    static const uint8_t mac[ETH_ALEN] = ENA_TEST_MAC;
    struct eth_header *eth = (void *)buf;
    struct ip_header *ip = (void *)(buf + sizeof(*eth));
    udp_header *udp = (void *)(buf + sizeof(*eth) + sizeof(*ip));
    uint8_t *pl = (uint8_t *)(udp + 1);
    uint16_t udp_len = sizeof(*udp) + payload;
    uint32_t sum;
    size_t i;

    memcpy(eth->h_dest, mac, ETH_ALEN);
    memset(eth->h_source, 0xaa, ETH_ALEN);
    eth->h_proto = cpu_to_be16(ETH_P_IP);

    memset(ip, 0, sizeof(*ip));
    ip->ip_ver_len = 0x45;
    ip->ip_len = cpu_to_be16(sizeof(*ip) + udp_len);
    ip->ip_id = cpu_to_be16(0x1234);
    ip->ip_ttl = 64;
    ip->ip_p = IP_PROTO_UDP;
    ip->ip_src = cpu_to_be32(f->src);
    ip->ip_dst = cpu_to_be32(f->dst);
    ip->ip_sum = cpu_to_be16(csum_fini(csum_add(ip, sizeof(*ip), 0)));

    udp->uh_sport = cpu_to_be16(f->sport);
    udp->uh_dport = cpu_to_be16(f->dport);
    udp->uh_ulen = cpu_to_be16(udp_len);
    udp->uh_sum = 0;
    for (i = 0; i < payload; i++) {
        pl[i] = 0x40 + i;
    }

    sum = csum_add(&ip->ip_src, 8, IP_PROTO_UDP + udp_len);
    sum = csum_add(udp, udp_len, sum);
    udp->uh_sum = cpu_to_be16(csum_fini(sum));

    return sizeof(*eth) + sizeof(*ip) + udp_len;
}

static size_t build_arp(uint8_t *buf, size_t payload)
{
    static const uint8_t mac[ETH_ALEN] = ENA_TEST_MAC;
    struct eth_header *eth = (void *)buf;
    size_t i;

    memcpy(eth->h_dest, mac, ETH_ALEN);
    memset(eth->h_source, 0xaa, ETH_ALEN);
    eth->h_proto = cpu_to_be16(ETH_P_ARP);
    for (i = 0; i < payload; i++) {
        buf[sizeof(*eth) + i] = i;
    }
    return sizeof(*eth) + payload;
}

static uint32_t toeplitz(const uint8_t *input, size_t len, const uint8_t *key)
{
    net_toeplitz_key kd;
    uint32_t hash = 0;

    net_toeplitz_key_init(&kd, (uint8_t *)key);
    net_toeplitz_add(&hash, (uint8_t *)input, len, &kd);
    return hash;
}

static uint32_t toeplitz_ip4(const Flow *f, const uint8_t *key)
{
    uint8_t input[8];

    stl_be_p(input, f->src);
    stl_be_p(input + 4, f->dst);
    return toeplitz(input, sizeof(input), key);
}

static uint32_t toeplitz_udp4(const Flow *f, const uint8_t *key)
{
    uint8_t input[12];

    stl_be_p(input, f->src);
    stl_be_p(input + 4, f->dst);
    stw_be_p(input + 8, f->sport);
    stw_be_p(input + 10, f->dport);
    return toeplitz(input, sizeof(input), key);
}

/* admin helpers */

static void rx_set_mtu(QEna *d, uint32_t mtu)
{
    struct ena_admin_set_feat_cmd cmd = {};

    cmd.feat_common.feature_id = ENA_ADMIN_MTU;
    cmd.u.mtu.mtu = cpu_to_le32(mtu);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_SUCCESS);
}

static void rx_get_stats(QEna *d, uint32_t *pkts, uint32_t *bytes,
                         uint32_t *drops)
{
    struct ena_admin_aq_get_stats_cmd cmd = {};
    struct ena_admin_acq_get_stats_resp resp;

    cmd.aq_common_descriptor.opcode = ENA_ADMIN_GET_STATS;
    cmd.type = ENA_ADMIN_GET_STATS_TYPE_BASIC;
    cmd.scope = ENA_ADMIN_ETH_TRAFFIC;
    cmd.device_id = cpu_to_le16(0xffff);
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &resp, sizeof(resp)),
                    ==, ENA_ADMIN_SUCCESS);
    *pkts = le32_to_cpu(resp.u.basic_stats.rx_pkts_low);
    *bytes = le32_to_cpu(resp.u.basic_stats.rx_bytes_low);
    *drops = le32_to_cpu(resp.u.basic_stats.rx_drops_low);
}

/* Writes the application key the way ena_reorder_rss_hash_key does. */
static void rx_set_rss_key_init(QEna *d, uint64_t buf, const uint8_t *app_key,
                                uint32_t init_val)
{
    struct ena_admin_feature_rss_flow_hash_control hc = {};
    struct ena_admin_set_feat_cmd cmd = {};
    uint8_t reordered[RSS_KEY_BYTES];
    int i;

    for (i = 0; i < RSS_KEY_BYTES; i++) {
        reordered[i] = app_key[RSS_KEY_BYTES - 1 - i];
    }
    hc.key_parts = cpu_to_le32(ENA_ADMIN_RSS_KEY_PARTS);
    for (i = 0; i < ENA_ADMIN_RSS_KEY_PARTS; i++) {
        hc.key[i] = cpu_to_le32(ldl_le_p(reordered + i * 4));
    }
    qtest_memwrite(d->dev.bus->qts, buf, &hc, sizeof(hc));

    cmd.feat_common.feature_id = ENA_ADMIN_RSS_HASH_FUNCTION;
    cmd.u.flow_hash_func.selected_func = cpu_to_le32(BIT(ENA_ADMIN_TOEPLITZ));
    cmd.u.flow_hash_func.init_val = cpu_to_le32(init_val);
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(hc)), ==,
                    ENA_ADMIN_SUCCESS);
}

static void rx_set_rss_key(QEna *d, uint64_t buf, const uint8_t *app_key)
{
    rx_set_rss_key_init(d, buf, app_key, 0);
}

static void rx_set_ind_tbl(QEna *d, uint64_t buf, const uint16_t *cq_idx)
{
    struct ena_admin_rss_ind_table_entry tbl[RSS_TBL_SIZE];
    struct ena_admin_set_feat_cmd cmd = {};
    int i;

    for (i = 0; i < RSS_TBL_SIZE; i++) {
        tbl[i].cq_idx = cpu_to_le16(cq_idx[i]);
        tbl[i].reserved = 0;
    }
    qtest_memwrite(d->dev.bus->qts, buf, tbl, sizeof(tbl));

    cmd.feat_common.feature_id = ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG;
    cmd.u.ind_table.size = cpu_to_le16(7);
    cmd.u.ind_table.inline_index = cpu_to_le32(0xffffffff);
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(tbl)), ==,
                    ENA_ADMIN_SUCCESS);
}

static void rx_set_hash_fields(QEna *d, uint64_t buf, int proto,
                               uint16_t fields)
{
    struct ena_admin_feature_rss_hash_control hc = {};
    struct ena_admin_set_feat_cmd cmd = {};

    hc.selected_fields[proto].fields = cpu_to_le16(fields);
    qtest_memwrite(d->dev.bus->qts, buf, &hc, sizeof(hc));

    cmd.feat_common.feature_id = ENA_ADMIN_RSS_HASH_INPUT;
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(hc)), ==,
                    ENA_ADMIN_SUCCESS);
}

/* tests */

static void test_rx_single(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    uint8_t got[128];
    uint32_t status, pkts, bytes, drops;
    size_t len;

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    rx_post(&r, 1);

    len = build_udp(frame, &flow, 32);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);

    qtest_memread(rx_qts(&r), rx_buf_addr(&r, 0), got, len);
    g_assert(memcmp(got, frame, len) == 0);

    status = le32_to_cpu(c.status);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, len);
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 0);
    g_assert_cmpuint(le16_to_cpu(c.sub_qid), ==, r.sq_idx);
    g_assert_cmpuint(c.offset, ==, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK, !=, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK, !=, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_BUFFER_MASK, !=, 0);
    g_assert_cmpuint(status & ENA_ETH_IO_RX_CDESC_BASE_L3_PROTO_IDX_MASK, ==,
                     ENA_ETH_IO_L3_PROTO_IPV4);
    g_assert_cmpuint((status & ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_MASK) >>
                     ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_SHIFT, ==,
                     ENA_ETH_IO_L4_PROTO_UDP);

    /* one frame consumed exactly one descriptor */
    g_assert_false(rx_cq_next(&r, &c));

    rx_get_stats(d, &pkts, &bytes, &drops);
    g_assert_cmpuint(pkts, ==, 1);
    g_assert_cmpuint(bytes, ==, len);
    g_assert_cmpuint(drops, ==, 0);
}

static void test_rx_deferred(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    uint32_t pkts, bytes, drops;
    size_t len;

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);

    /* no descriptors posted: the frame stays in the netdev queue */
    len = build_udp(frame, &flow, 16);
    ena_backend_send(r.fd, frame, len);
    rx_settle(&r);
    g_assert_false(rx_cq_next(&r, &c));
    rx_get_stats(d, &pkts, &bytes, &drops);
    g_assert_cmpuint(pkts, ==, 0);
    g_assert_cmpuint(bytes, ==, 0);
    g_assert_cmpuint(drops, ==, 0);

    /* the doorbell releases it */
    rx_post(&r, 1);
    rx_wait_cdesc(&r, &c);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, len);
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 0);
}

static void test_rx_no_queue(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    uint32_t pkts, bytes, drops;
    size_t len;
    int fd = ena_backend_fd(data);
    int i;

    ena_bringup(d);
    len = build_udp(frame, &flow, 16);
    ena_backend_send(fd, frame, len);
    for (i = 0; i < SETTLE_STEPS; i++) {
        qtest_clock_step(d->dev.bus->qts, SETTLE_NS);
    }
    rx_get_stats(d, &pkts, &bytes, &drops);
    g_assert_cmpuint(pkts, ==, 0);
    g_assert_cmpuint(bytes, ==, 0);
    g_assert_cmpuint(drops, ==, 1);
}

static void test_rx_scatter(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[256];
    uint8_t got[256];
    uint32_t status;
    size_t len;

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 64);
    rx_post(&r, 2);

    len = build_udp(frame, &flow, 58);
    g_assert_cmpuint(len, ==, 100);
    ena_backend_send(r.fd, frame, len);

    rx_wait_cdesc(&r, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, 64);
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK, !=, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK, ==, 0);

    g_assert_true(rx_cq_next(&r, &c));
    status = le32_to_cpu(c.status);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, 36);
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 1);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK, ==, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK, !=, 0);

    qtest_memread(rx_qts(&r), rx_buf_addr(&r, 0), got, 64);
    g_assert(memcmp(got, frame, 64) == 0);
    qtest_memread(rx_qts(&r), rx_buf_addr(&r, 1), got, 36);
    g_assert(memcmp(got, frame + 64, 36) == 0);
}

static void test_rx_short_of_descs(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[512];
    uint32_t pkts, bytes, drops;
    size_t len;

    ena_bringup(d);
    /* a single posted 64-byte buffer cannot hold a 300-byte frame */
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 64);
    rx_post(&r, 1);

    len = build_udp(frame, &flow, 258);
    g_assert_cmpuint(len, ==, 300);
    ena_backend_send(r.fd, frame, len);
    rx_settle(&r);
    g_assert_false(rx_cq_next(&r, &c));
    rx_get_stats(d, &pkts, &bytes, &drops);
    g_assert_cmpuint(pkts, ==, 0);
    g_assert_cmpuint(bytes, ==, 0);
    g_assert_cmpuint(drops, ==, 1);

    /* the descriptor was not consumed: a fitting frame still lands in it */
    len = build_udp(frame, &flow, 16);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, 0);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, len);
}

static void test_rx_oversize(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[2048];
    uint32_t pkts, bytes, drops;
    size_t len;

    ena_bringup(d);
    rx_set_mtu(d, 1000);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    rx_post(&r, 4);

    /* MTU plus Ethernet header plus one VLAN tag is still accepted */
    len = build_udp(frame, &flow, 1018 - 14 - 28);
    g_assert_cmpuint(len, ==, 1018);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, len);

    len = build_udp(frame, &flow, 1019 - 14 - 28);
    g_assert_cmpuint(len, ==, 1019);
    ena_backend_send(r.fd, frame, len);
    rx_settle(&r);
    g_assert_false(rx_cq_next(&r, &c));

    rx_get_stats(d, &pkts, &bytes, &drops);
    g_assert_cmpuint(pkts, ==, 1);
    g_assert_cmpuint(bytes, ==, 1018);
    g_assert_cmpuint(drops, ==, 1);
}

static void test_rx_wrap(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    size_t len = build_udp(frame, &flow, 16);
    int round, i;

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);

    for (round = 0; round < 2; round++) {
        rx_post(&r, 16);
        for (i = 0; i < 16; i++) {
            bool phase = r.cq_phase;

            ena_backend_send(r.fd, frame, len);
            rx_wait_cdesc(&r, &c);
            g_assert_cmpuint(le16_to_cpu(c.req_id), ==, i);
            g_assert_cmpuint(!!(le32_to_cpu(c.status) &
                                ENA_ETH_IO_RX_CDESC_BASE_PHASE_MASK), ==,
                             phase);
        }
        /* the completion queue wrapped and flipped phase */
        g_assert_cmpuint(r.cq_head, ==, (round + 1) * 16);
        g_assert_cmpuint(r.cq_phase, ==, round % 2 != 0);
    }
}

static void test_rx_cq_8word(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_ext c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    size_t len;
    int i;

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 8, 1, 2048);
    rx_post(&r, 1);

    len = build_udp(frame, &flow, 16);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c.base);
    g_assert_cmpuint(le16_to_cpu(c.base.length), ==, len);

    /* the extended words carry the buffer address; timestamps stay zero */
    qtest_memread(rx_qts(&r), r.cq_base, &c, sizeof(c));
    g_assert_cmphex(le32_to_cpu(c.buff_addr_lo), ==, (uint32_t)rx_buf_addr(&r, 0));
    g_assert_cmphex(le16_to_cpu(c.buff_addr_hi), ==, rx_buf_addr(&r, 0) >> 32);
    for (i = 6; i < 8; i++) {
        g_assert_cmpuint(((uint32_t *)&c)[i], ==, 0);
    }
}

static void test_rx_intr_unmask(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    size_t len = build_udp(frame, &flow, 16);

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    ena_msix_setup(d, 1);
    rx_post(&r, 4);

    /* masked: the completion arrives without an interrupt */
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_false(ena_msix_fired(d, 1));

    /* unmasking delivers the pending interrupt */
    rx_unmask(&r, 0);
    g_assert_true(ena_msix_fired(d, 1));
    ena_msix_clear(d, 1);

    /* delivery re-masked the vector */
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_false(ena_msix_fired(d, 1));

    /* an unmask without a pending completion stays quiet */
    rx_unmask(&r, 0);
    g_assert_true(ena_msix_fired(d, 1));
    ena_msix_clear(d, 1);
    rx_unmask(&r, 0);
    g_assert_false(ena_msix_fired(d, 1));

    /* unmasked before the frame: the interrupt fires on the completion */
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_true(ena_msix_fired(d, 1));
}

static void test_rx_intr_moderation(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    size_t len = build_udp(frame, &flow, 16);

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    ena_msix_setup(d, 1);
    rx_post(&r, 4);

    rx_unmask(&r, MODER_DELAY_US);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_false(ena_msix_fired(d, 1));

    qtest_clock_step(rx_qts(&r), (MODER_DELAY_US - SETTLE_STEPS) * 1000);
    g_assert_false(ena_msix_fired(d, 1));
    qtest_clock_step(rx_qts(&r), MODER_DELAY_US * 1000);
    g_assert_true(ena_msix_fired(d, 1));
}

static void test_rx_intr_disabled(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    size_t len = build_udp(frame, &flow, 16);

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 0xffffffff, 2048);
    ena_msix_setup(d, 1);
    rx_post(&r, 2);

    rx_unmask(&r, 0);
    ena_msix_clear(d, ENA_TEST_ADMIN_VECTOR);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_false(ena_msix_fired(d, 1));
    g_assert_false(ena_msix_fired(d, ENA_TEST_ADMIN_VECTOR));
}

/* one TX frame with completion requested */
static void tx_send(QEna *d, EnaTxQueue *q, uint64_t buf, size_t len)
{
    struct ena_eth_io_tx_desc dsc;

    ena_tx_desc_fill(&dsc, buf, len, 0, ENA_ETH_IO_TX_DESC_FIRST_MASK |
                     ENA_ETH_IO_TX_DESC_LAST_MASK |
                     ENA_ETH_IO_TX_DESC_COMP_REQ_MASK, 0, 0);
    ena_txq_push(d, q, &dsc);
    ena_txq_doorbell(d, q);
}

/*
 * FreeBSD binds the RX and TX CQ of a queue pair to one vector and re-arms
 * it through the TX CQ register only (ena_datapath.c:117-120).
 */
static void test_rx_intr_shared_vector(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    EnaTxQueue q;
    struct ena_eth_io_rx_cdesc_base c;
    struct ena_eth_io_tx_cdesc tc;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    uint8_t tx_frame[64];
    size_t len = build_udp(frame, &flow, 16);
    size_t tx_len = ena_build_eth(tx_frame, 32);
    uint64_t tx_buf = guest_alloc(alloc, sizeof(tx_frame));
    uint32_t val;

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    ena_txq_create(d, &q, 16, 2, 1, false);
    g_assert_cmpuint(q.cq_idx, !=, r.cq_idx);
    ena_msix_setup(d, 1);
    rx_post(&r, 8);
    qtest_memwrite(rx_qts(&r), tx_buf, tx_frame, tx_len);

    /* the TX CQ register arms the vector for RX completions */
    ena_reg_write(d, q.unmask_off, ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_true(ena_msix_fired(d, 1));
    ena_msix_clear(d, 1);

    /* auto-masked: RX and TX completions pend on the vector */
    tx_send(d, &q, tx_buf, tx_len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &tc));
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_false(ena_msix_fired(d, 1));

    /* one re-arm delivers one interrupt for both */
    ena_reg_write(d, q.unmask_off, ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK);
    g_assert_true(ena_msix_fired(d, 1));
    ena_msix_clear(d, 1);
    ena_reg_write(d, q.unmask_off, ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK);
    g_assert_false(ena_msix_fired(d, 1));

    /* masking through the RX CQ register masks the vector for TX too */
    ena_reg_write(d, CQ_UNMASK_BASE + r.cq_idx * 4, 0);
    tx_send(d, &q, tx_buf, tx_len);
    g_assert_true(ena_txq_poll_cdesc(d, &q, &tc));
    g_assert_false(ena_msix_fired(d, 1));

    /* rx and tx delays apply per completion type on the same vector */
    val = ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK | MODER_DELAY_US |
          ((2 * MODER_DELAY_US) << ENA_ETH_IO_INTR_REG_TX_INTR_DELAY_SHIFT);
    ena_reg_write(d, q.unmask_off, val);
    g_assert_false(ena_msix_fired(d, 1));
    qtest_clock_step(rx_qts(&r), (2 * MODER_DELAY_US - 100) * 1000);
    g_assert_false(ena_msix_fired(d, 1));
    qtest_clock_step(rx_qts(&r), 200 * 1000);
    g_assert_true(ena_msix_fired(d, 1));
    ena_msix_clear(d, 1);

    ena_reg_write(d, q.unmask_off, val);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_false(ena_msix_fired(d, 1));
    qtest_clock_step(rx_qts(&r), (MODER_DELAY_US - SETTLE_STEPS) * 1000);
    g_assert_false(ena_msix_fired(d, 1));
    qtest_clock_step(rx_qts(&r), SETTLE_STEPS * 1000);
    g_assert_true(ena_msix_fired(d, 1));
}

static void test_rx_checksums(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    struct ip_header *ip = (void *)0;
    udp_header *udp = (void *)0;
    uint8_t frame[128];
    uint32_t status;
    size_t len;

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    rx_post(&r, 8);
    ip = (void *)(frame + sizeof(struct eth_header));
    udp = (void *)(frame + sizeof(struct eth_header) + sizeof(*ip));

    /* good IPv4/UDP */
    len = build_udp(frame, &flow, 16);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK, ==, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK,
                    !=, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK, ==, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_MASK, ==, 0);

    /* broken UDP checksum */
    udp->uh_sum = cpu_to_be16(be16_to_cpu(udp->uh_sum) ^ 0x0100);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK,
                    !=, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK, !=, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK, ==, 0);

    /* broken IPv4 header checksum */
    len = build_udp(frame, &flow, 16);
    ip->ip_sum = cpu_to_be16(be16_to_cpu(ip->ip_sum) ^ 0x0100);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK, !=, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK, ==, 0);

    /* a UDP datagram without a checksum is not checked */
    len = build_udp(frame, &flow, 16);
    udp->uh_sum = 0;
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK,
                    ==, 0);
    g_assert_cmpuint((status & ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_MASK) >>
                     ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_SHIFT, ==,
                     ENA_ETH_IO_L4_PROTO_UDP);

    /* non-IP frame */
    len = build_arp(frame, 32);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmpuint(status & ENA_ETH_IO_RX_CDESC_BASE_L3_PROTO_IDX_MASK, ==, 0);
    g_assert_cmpuint(status & ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_MASK, ==, 0);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK,
                    ==, 0);
    g_assert_cmpuint(le32_to_cpu(c.hash), ==, 0);
}

static void test_rx_rss(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing ra, rb;
    struct ena_admin_acq_create_cq_resp_desc spare;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow fa = { 0x0a000001, 0x0a000002, 1000, 2000 };
    const Flow fb = { 0xc0a80003, 0xc0a80004, 3000, 4000 };
    uint16_t tbl[RSS_TBL_SIZE];
    uint8_t frame[128];
    uint64_t ctrl;
    uint32_t ha, hb;
    size_t len;
    int i;

    ena_bringup(d);
    rx_ring_init(&ra, d, data, alloc, 16, 16, 4, 1, 2048);
    ctrl = rx_alloc(&ra, alloc, RSS_TBL_SIZE *
                    sizeof(struct ena_admin_rss_ind_table_entry));
    /* an unpaired CQ pushes the second queue's SQ and CQ indices apart */
    g_assert_cmpint(ena_create_cq(d, 16, 4, 3, ctrl, &spare), ==,
                    ENA_ADMIN_SUCCESS);
    rx_ring_init(&rb, d, data, alloc, 16, 16, 4, 2, 2048);
    g_assert_cmpuint(rb.sq_idx, !=, rb.cq_idx);

    rx_set_rss_key(d, ctrl, rss_app_key);
    ha = toeplitz_udp4(&fa, rss_app_key);
    hb = toeplitz_udp4(&fb, rss_app_key);
    g_assert_cmpuint(ha & (RSS_TBL_SIZE - 1), !=, hb & (RSS_TBL_SIZE - 1));

    /* entries name RX SQ indices */
    for (i = 0; i < RSS_TBL_SIZE; i++) {
        tbl[i] = rb.sq_idx;
    }
    tbl[ha & (RSS_TBL_SIZE - 1)] = ra.sq_idx;
    rx_set_ind_tbl(d, ctrl, tbl);

    rx_post(&ra, 2);
    rx_post(&rb, 2);

    len = build_udp(frame, &fa, 16);
    ena_backend_send(ra.fd, frame, len);
    rx_wait_cdesc(&ra, &c);
    g_assert_cmpuint(le32_to_cpu(c.hash), ==, ha);
    g_assert_cmpuint(le16_to_cpu(c.sub_qid), ==, ra.sq_idx);

    len = build_udp(frame, &fb, 16);
    ena_backend_send(rb.fd, frame, len);
    rx_wait_cdesc(&rb, &c);
    g_assert_cmpuint(le32_to_cpu(c.hash), ==, hb);
    g_assert_cmpuint(le16_to_cpu(c.sub_qid), ==, rb.sq_idx);

    /* neither queue saw the other queue's flow */
    g_assert_false(rx_cq_next(&ra, &c));
    g_assert_false(rx_cq_next(&rb, &c));
}

/* the hash starts from init_val: Toeplitz result XOR init_val */
static void test_rx_rss_init_val(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint64_t ctrl = guest_alloc(alloc, 4096);
    uint8_t frame[128];
    size_t len;

    ena_bringup(d);
    rx_set_rss_key_init(d, ctrl, rss_app_key, 0x12345678);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    rx_post(&r, 2);

    len = build_udp(frame, &flow, 16);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_cmpuint(le32_to_cpu(c.hash), ==,
                     0x12345678 ^ toeplitz_udp4(&flow, rss_app_key));
}

static void test_rx_rss_fields(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    uint64_t ctrl;
    size_t len = build_udp(frame, &flow, 16);

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    ctrl = rx_alloc(&r, alloc,
                    sizeof(struct ena_admin_feature_rss_hash_control));
    rx_set_rss_key(d, ctrl, rss_app_key);
    rx_post(&r, 4);

    /* dropping the L4 ports from UDP4 leaves an address-only hash */
    rx_set_hash_fields(d, ctrl, ENA_ADMIN_RSS_UDP4,
                       ENA_ADMIN_RSS_L3_SA | ENA_ADMIN_RSS_L3_DA);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_cmpuint(le32_to_cpu(c.hash), ==,
                     toeplitz_ip4(&flow, rss_app_key));

    /* clearing every field for UDP4 disables the hash */
    rx_set_hash_fields(d, ctrl, ENA_ADMIN_RSS_UDP4, 0);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_cmpuint(le32_to_cpu(c.hash), ==, 0);
}

static void test_rx_bad_desc(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    RxRing r;
    struct ena_eth_io_rx_desc desc = {};
    struct ena_eth_io_rx_cdesc_base c;
    const Flow flow = { 0x0a000001, 0x0a000002, 1000, 2000 };
    uint8_t frame[128];
    uint32_t pkts, bytes, drops;
    size_t len = build_udp(frame, &flow, 16);

    ena_bringup(d);
    rx_ring_init(&r, d, data, alloc, 16, 16, 4, 1, 2048);
    desc.length = cpu_to_le16(2048);
    desc.buff_addr_lo = cpu_to_le32(r.buf_base);
    desc.buff_addr_hi = cpu_to_le16(r.buf_base >> 32);

    /* req_id outside the ring */
    desc.ctrl = ENA_ETH_IO_RX_DESC_FIRST_MASK | ENA_ETH_IO_RX_DESC_LAST_MASK;
    desc.req_id = cpu_to_le16(16);
    qtest_memwrite(rx_qts(&r), r.sq_base, &desc, sizeof(desc));
    r.tail = 1;
    ena_reg_write(d, SQ_DB_BASE + r.sq_idx * 4, r.tail);
    ena_backend_send(r.fd, frame, len);
    rx_settle(&r);
    g_assert_false(rx_cq_next(&r, &c));

    /* descriptor without first|last */
    desc.ctrl = ENA_ETH_IO_RX_DESC_FIRST_MASK;
    desc.req_id = 0;
    qtest_memwrite(rx_qts(&r), r.sq_base, &desc, sizeof(desc));
    ena_reg_write(d, SQ_DB_BASE + r.sq_idx * 4, r.tail);
    ena_backend_send(r.fd, frame, len);
    rx_settle(&r);
    g_assert_false(rx_cq_next(&r, &c));

    rx_get_stats(d, &pkts, &bytes, &drops);
    g_assert_cmpuint(pkts, ==, 0);
    g_assert_cmpuint(bytes, ==, 0);
    g_assert_cmpuint(drops, ==, 2);

    /* a well formed descriptor is still accepted afterwards */
    desc.ctrl = ENA_ETH_IO_RX_DESC_FIRST_MASK | ENA_ETH_IO_RX_DESC_LAST_MASK;
    qtest_memwrite(rx_qts(&r), r.sq_base, &desc, sizeof(desc));
    ena_reg_write(d, SQ_DB_BASE + r.sq_idx * 4, r.tail);
    ena_backend_send(r.fd, frame, len);
    rx_wait_cdesc(&r, &c);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, len);
}

static void register_ena_rxpath_test(void)
{
    QOSGraphTestOptions opts = {
        .before = ena_test_before,
    };

    qos_add_test("rxpath/single", "ena", test_rx_single, &opts);
    qos_add_test("rxpath/deferred", "ena", test_rx_deferred, &opts);
    qos_add_test("rxpath/no-queue", "ena", test_rx_no_queue, &opts);
    qos_add_test("rxpath/scatter", "ena", test_rx_scatter, &opts);
    qos_add_test("rxpath/short-of-descs", "ena", test_rx_short_of_descs, &opts);
    qos_add_test("rxpath/oversize", "ena", test_rx_oversize, &opts);
    qos_add_test("rxpath/wrap", "ena", test_rx_wrap, &opts);
    qos_add_test("rxpath/cq-8word", "ena", test_rx_cq_8word, &opts);
    qos_add_test("rxpath/intr-unmask", "ena", test_rx_intr_unmask, &opts);
    qos_add_test("rxpath/intr-moderation", "ena", test_rx_intr_moderation,
                 &opts);
    qos_add_test("rxpath/intr-disabled", "ena", test_rx_intr_disabled, &opts);
    qos_add_test("rxpath/intr-shared-vector", "ena",
                 test_rx_intr_shared_vector, &opts);
    qos_add_test("rxpath/checksums", "ena", test_rx_checksums, &opts);
    qos_add_test("rxpath/rss", "ena", test_rx_rss, &opts);
    qos_add_test("rxpath/rss-fields", "ena", test_rx_rss_fields, &opts);
    qos_add_test("rxpath/rss-init-val", "ena", test_rx_rss_init_val, &opts);
    qos_add_test("rxpath/bad-desc", "ena", test_rx_bad_desc, &opts);
}

libqos_init(register_ena_rxpath_test);
