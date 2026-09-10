/*
 * QEMU Amazon Elastic Network Adapter (ENA) emulation: RX datapath
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/log.h"
#include "net/eth.h"
#include "hw/net/net_rx_pkt.h"
#include "hw/ena.h"

/* Largest accepted frame: MTU plus Ethernet header and one VLAN tag. */
#define ENA_RX_MAX_FRAME (ENA_MAX_MTU + ETH_HLEN + 4)

#define ENA_RSS_L4_PORTS (ENA_ADMIN_RSS_L4_DP | ENA_ADMIN_RSS_L4_SP)

/* Toeplitz key byte stream: key words in reverse order, each big-endian. */
static void ena_rss_key(const EnaState *s, uint8_t *key)
{
    int i;

    for (i = 0; i < ENA_ADMIN_RSS_KEY_PARTS; i++) {
        stl_be_p(key + i * 4, s->rss.key[ENA_ADMIN_RSS_KEY_PARTS - 1 - i]);
    }
}

static int ena_rss_proto(bool hasip4, bool frag, EthL4HdrProto l4)
{
    if (hasip4) {
        if (frag) {
            return ENA_ADMIN_RSS_IP4_FRAG;
        }
        switch (l4) {
        case ETH_L4_HDR_PROTO_TCP:
            return ENA_ADMIN_RSS_TCP4;
        case ETH_L4_HDR_PROTO_UDP:
            return ENA_ADMIN_RSS_UDP4;
        default:
            return ENA_ADMIN_RSS_IP4;
        }
    }
    switch (l4) {
    case ETH_L4_HDR_PROTO_TCP:
        return ENA_ADMIN_RSS_TCP6;
    case ETH_L4_HDR_PROTO_UDP:
        return ENA_ADMIN_RSS_UDP6;
    default:
        return ENA_ADMIN_RSS_IP6;
    }
}

/* Non-IP frames and protocols without selected fields hash to 0. */
static uint32_t ena_rss_hash(EnaState *s)
{
    uint8_t key[ENA_ADMIN_RSS_KEY_PARTS * 4];
    NetRxPktRssType type;
    EthL4HdrProto l4;
    bool hasip4, hasip6, frag;
    uint16_t fields;

    net_rx_pkt_get_protocols(s->rx_pkt, &hasip4, &hasip6, &l4);
    if (!hasip4 && !hasip6) {
        return 0;
    }
    frag = hasip4 && net_rx_pkt_get_ip4_info(s->rx_pkt)->fragment;

    fields = s->rss.fields[ena_rss_proto(hasip4, frag, l4)];
    if (fields == 0) {
        return 0;
    }
    if (!(fields & ENA_RSS_L4_PORTS)) {
        l4 = ETH_L4_HDR_PROTO_INVALID;
    }

    if (hasip4) {
        type = l4 == ETH_L4_HDR_PROTO_TCP ? NetPktRssIpV4Tcp :
               l4 == ETH_L4_HDR_PROTO_UDP ? NetPktRssIpV4Udp : NetPktRssIpV4;
    } else {
        type = l4 == ETH_L4_HDR_PROTO_TCP ? NetPktRssIpV6Tcp :
               l4 == ETH_L4_HDR_PROTO_UDP ? NetPktRssIpV6Udp : NetPktRssIpV6;
    }

    ena_rss_key(s, key);
    return s->rss.init_val ^ net_rx_pkt_calc_rss_hash(s->rx_pkt, type, key);
}

/*
 * Indirection table entries hold RX SQ indices; the ABI field is named
 * cq_idx. Unusable entries fall back to the first used RX SQ.
 */
static EnaSq *ena_rx_sq(EnaState *s, uint32_t hash)
{
    uint16_t idx = s->rss.ind_tbl[hash & (ENA_RSS_IND_TBL_SIZE - 1)];
    int i;

    if (idx < ENA_MAX_SQ && s->sq[idx].used && !s->sq[idx].is_tx) {
        return &s->sq[idx];
    }
    for (i = 0; i < ENA_MAX_SQ; i++) {
        if (s->sq[i].used && !s->sq[i].is_tx) {
            return &s->sq[i];
        }
    }
    return NULL;
}

/* Parsing results of the attached frame as completion status bits. */
static uint32_t ena_rx_status(EnaState *s)
{
    uint32_t status = ENA_ETH_IO_RX_CDESC_BASE_BUFFER_MASK;
    EthL4HdrProto l4;
    bool hasip4, hasip6, valid;

    net_rx_pkt_get_protocols(s->rx_pkt, &hasip4, &hasip6, &l4);
    if (hasip4) {
        status |= ENA_ETH_IO_L3_PROTO_IPV4;
        if (net_rx_pkt_get_ip4_info(s->rx_pkt)->fragment) {
            status |= ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_MASK;
        }
        if (net_rx_pkt_validate_l3_csum(s->rx_pkt, &valid) && !valid) {
            status |= ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK;
        }
    } else if (hasip6) {
        status |= ENA_ETH_IO_L3_PROTO_IPV6;
    }

    if (l4 == ETH_L4_HDR_PROTO_TCP || l4 == ETH_L4_HDR_PROTO_UDP) {
        uint32_t idx = l4 == ETH_L4_HDR_PROTO_TCP ? ENA_ETH_IO_L4_PROTO_TCP :
                                                    ENA_ETH_IO_L4_PROTO_UDP;

        status |= idx << ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_SHIFT;
        if (net_rx_pkt_validate_l4_csum(s->rx_pkt, &valid)) {
            status |= ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK;
            if (!valid) {
                status |= ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK;
            }
        }
    }
    return status;
}

static uint32_t ena_rx_desc_size(const struct ena_eth_io_rx_desc *d)
{
    uint16_t len = le16_to_cpu(d->length);

    return len ? len : 65536;
}

static uint64_t ena_rx_desc_addr(const struct ena_eth_io_rx_desc *d)
{
    return le32_to_cpu(d->buff_addr_lo) |
           ((uint64_t)le16_to_cpu(d->buff_addr_hi) << 32);
}

static bool ena_rx_desc_valid(const EnaSq *sq,
                              const struct ena_eth_io_rx_desc *d)
{
    uint8_t need = ENA_ETH_IO_RX_DESC_FIRST_MASK | ENA_ETH_IO_RX_DESC_LAST_MASK |
                   ENA_ETH_IO_RX_DESC_COMP_REQ_MASK;

    if ((d->ctrl & need) != need) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ena: RX descriptor ctrl 0x%x lacks first|last|comp_req\n",
                      d->ctrl);
        return false;
    }
    if (le16_to_cpu(d->req_id) >= sq->depth) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: RX descriptor req_id %u >= %u\n",
                      le16_to_cpu(d->req_id), sq->depth);
        return false;
    }
    return true;
}

/* Reads the descriptors covering len bytes; returns their count or -1. */
static int ena_rx_collect(EnaState *s, EnaSq *sq, size_t len,
                          struct ena_eth_io_rx_desc *desc)
{
    uint16_t avail = sq->tail - sq->head;
    size_t cap = 0;
    int n = 0;

    while (cap < len) {
        if (n == avail || n == ENA_MAX_PKT_DESCS) {
            return -1;
        }
        if (!ena_dma_read(s, sq->base +
                          (uint64_t)((sq->head + n) & (sq->depth - 1)) *
                          sizeof(*desc), &desc[n], sizeof(*desc)) ||
            !ena_rx_desc_valid(sq, &desc[n])) {
            /* the bad descriptor is consumed so the queue recovers */
            sq->head += n + 1;
            return -1;
        }
        cap += ena_rx_desc_size(&desc[n]);
        n++;
    }
    return n;
}

static void ena_rx_push_cdesc(EnaState *s, EnaCq *cq, uint32_t status,
                              uint16_t length, uint16_t req_id, uint32_t hash,
                              uint16_t sub_qid, uint64_t addr)
{
    struct ena_eth_io_rx_cdesc_ext c = {};

    assert(cq->entry_size <= sizeof(c));
    if (cq->phase) {
        status |= ENA_ETH_IO_RX_CDESC_BASE_PHASE_MASK;
    }
    c.base.status = cpu_to_le32(status);
    c.base.length = cpu_to_le16(length);
    c.base.req_id = cpu_to_le16(req_id);
    c.base.hash = cpu_to_le32(hash);
    c.base.sub_qid = cpu_to_le16(sub_qid);
    c.buff_addr_lo = cpu_to_le32(addr);
    c.buff_addr_hi = cpu_to_le16(addr >> 32);
    ena_cq_push(s, cq, &c);
}

void ena_rx_doorbell(EnaState *s, EnaSq *sq)
{
    assert(!sq->is_tx);
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

bool ena_rx_can_receive(EnaState *s)
{
    bool have_rx = false;
    int i;

    for (i = 0; i < ENA_MAX_SQ; i++) {
        EnaSq *sq = &s->sq[i];

        if (!sq->used || sq->is_tx) {
            continue;
        }
        if (sq->head != sq->tail) {
            return true;
        }
        have_rx = true;
    }
    return !have_rx;
}

ssize_t ena_rx_receive_iov(EnaState *s, const struct iovec *iov, int iovcnt)
{
    struct ena_eth_io_rx_desc desc[ENA_MAX_PKT_DESCS];
    uint8_t frame[ENA_RX_MAX_FRAME];
    size_t len = iov_size(iov, iovcnt);
    size_t off = 0;
    uint32_t status, hash;
    EnaSq *sq;
    EnaCq *cq;
    int n, i;

    if (len > s->mtu + ETH_HLEN + 4) {
        s->rx_drops++;
        return len;
    }

    iov_to_buf(iov, iovcnt, 0, frame, len);
    /* The fabric only delivers frames for our MAC, broadcast and multicast. */
    if (len < ETH_HLEN ||
        (!(frame[0] & 1) && memcmp(frame, s->conf.macaddr.a, ETH_ALEN))) {
        return len;
    }
    net_rx_pkt_attach_data(s->rx_pkt, frame, len, false);
    hash = ena_rss_hash(s);

    sq = ena_rx_sq(s, hash);
    if (!sq || !s->cq[sq->cq_idx].used) {
        s->rx_drops++;
        return len;
    }

    n = ena_rx_collect(s, sq, len, desc);
    if (n < 0) {
        s->rx_drops++;
        return len;
    }

    status = ena_rx_status(s);
    cq = &s->cq[sq->cq_idx];
    for (i = 0; i < n; i++) {
        uint32_t chunk = MIN(ena_rx_desc_size(&desc[i]), len - off);
        uint32_t bits = status;

        ena_dma_write(s, ena_rx_desc_addr(&desc[i]), frame + off, chunk);
        off += chunk;
        if (i == 0) {
            bits |= ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK;
        }
        if (i == n - 1) {
            bits |= ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK;
        }
        ena_rx_push_cdesc(s, cq, bits, chunk, le16_to_cpu(desc[i].req_id),
                          hash, sq - s->sq, ena_rx_desc_addr(&desc[i]));
    }
    assert(off == len);

    sq->head += n;
    s->rx_pkts++;
    s->rx_bytes += len;
    ena_cq_intr(s, cq, false);
    return len;
}
