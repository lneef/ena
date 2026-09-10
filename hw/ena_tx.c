/*
 * QEMU Amazon Elastic Network Adapter (ENA) emulation: TX datapath
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/pci/pci.h"
#include "net/eth.h"
#include "net/checksum.h"
#include "hw/net/net_tx_pkt.h"
#include "hw/ena.h"

#define ENA_TX_DESC_SIZE        sizeof(struct ena_eth_io_tx_desc)
#define ENA_LLQ_HEADER_OFF      (ENA_LLQ_DESCS_BEFORE_HEADER * ENA_TX_DESC_SIZE)
#define ENA_LLQ_MAX_HEADER      (ENA_LLQ_LARGE_ENTRY_SIZE - ENA_LLQ_HEADER_OFF)
#define ENA_TX_MAX_PKT          (65536 + ENA_LLQ_MAX_HEADER)

/* One packet: the descriptor slots from sq->head up to the LAST descriptor. */
typedef struct EnaTxPkt {
    struct ena_eth_io_tx_desc desc[ENA_MAX_PKT_DESCS];
    unsigned ndesc;
    unsigned slots;
    bool have_meta;
    EnaTxMeta meta;
} EnaTxPkt;

/* Descriptors per LLQ entry after the first entry of a packet */
static unsigned ena_llq_descs_per_entry(const EnaSq *sq)
{
    return sq->entry_size / ENA_TX_DESC_SIZE;
}

/* LLQ entry holding a descriptor slot, relative to the first entry of a packet */
static unsigned ena_llq_slot_line(const EnaSq *sq, unsigned slot)
{
    if (slot < ENA_LLQ_DESCS_BEFORE_HEADER) {
        return 0;
    }
    return 1 + (slot - ENA_LLQ_DESCS_BEFORE_HEADER) / ena_llq_descs_per_entry(sq);
}

static unsigned ena_llq_slot_off(const EnaSq *sq, unsigned slot)
{
    if (slot < ENA_LLQ_DESCS_BEFORE_HEADER) {
        return slot * ENA_TX_DESC_SIZE;
    }
    return ((slot - ENA_LLQ_DESCS_BEFORE_HEADER) % ena_llq_descs_per_entry(sq)) *
           ENA_TX_DESC_SIZE;
}

/* SQ entries a packet of the given slot count occupies */
static unsigned ena_tx_pkt_entries(const EnaSq *sq, unsigned slots)
{
    assert(slots > 0);
    return sq->llq ? ena_llq_slot_line(sq, slots - 1) + 1 : slots;
}

static void ena_tx_read_slot(EnaState *s, const EnaSq *sq, unsigned slot,
                             void *out)
{
    uint16_t mask = sq->depth - 1;

    if (sq->llq) {
        uint16_t line = (sq->head + ena_llq_slot_line(sq, slot)) & mask;

        memcpy(out, ena_llq_mem(s, sq) + line * sq->entry_size +
                    ena_llq_slot_off(sq, slot), ENA_TX_DESC_SIZE);
    } else {
        ena_dma_read(s, sq->base + ((sq->head + slot) & mask) * ENA_TX_DESC_SIZE,
                     out, ENA_TX_DESC_SIZE);
    }
}

/* Header pushed with the packet at sq->head */
static const uint8_t *ena_tx_llq_header(EnaState *s, const EnaSq *sq)
{
    uint16_t line = sq->head & (sq->depth - 1);

    return ena_llq_mem(s, sq) + line * sq->entry_size + ENA_LLQ_HEADER_OFF;
}

static void ena_tx_free_frag(void *context, void *base, size_t len)
{
}

static void ena_tx_read_meta(EnaTxPkt *p,
                             const struct ena_eth_io_tx_meta_desc *m)
{
    uint32_t len_ctrl = le32_to_cpu(m->len_ctrl);
    uint32_t word2 = le32_to_cpu(m->word2);

    p->have_meta = true;
    p->meta.mss = ((word2 & ENA_ETH_IO_TX_META_DESC_MSS_LO_MASK) >>
                   ENA_ETH_IO_TX_META_DESC_MSS_LO_SHIFT) |
                  (((len_ctrl & ENA_ETH_IO_TX_META_DESC_MSS_HI_MASK) >>
                    ENA_ETH_IO_TX_META_DESC_MSS_HI_SHIFT) << 10);
    p->meta.l3_hdr_len = word2 & ENA_ETH_IO_TX_META_DESC_L3_HDR_LEN_MASK;
    p->meta.l3_hdr_off = (word2 & ENA_ETH_IO_TX_META_DESC_L3_HDR_OFF_MASK) >>
                         ENA_ETH_IO_TX_META_DESC_L3_HDR_OFF_SHIFT;
    p->meta.l4_hdr_len_words =
        (word2 & ENA_ETH_IO_TX_META_DESC_L4_HDR_LEN_IN_WORDS_MASK) >>
        ENA_ETH_IO_TX_META_DESC_L4_HDR_LEN_IN_WORDS_SHIFT;
}

/* Descriptor slot the driver has published ahead of the doorbell */
static bool ena_tx_slot_posted(const EnaSq *sq, unsigned slot, uint16_t avail)
{
    return (sq->llq ? ena_llq_slot_line(sq, slot) : slot) < avail;
}

static bool ena_tx_parse(EnaState *s, const EnaSq *sq, EnaTxPkt *p)
{
    uint16_t avail = sq->tail - sq->head;
    unsigned slot;

    memset(p, 0, sizeof(*p));
    /* up to ENA_MAX_PKT_DESCS buffers plus one meta descriptor */
    for (slot = 0; slot < ENA_MAX_PKT_DESCS + 1; slot++) {
        struct ena_eth_io_tx_desc d;
        uint32_t len_ctrl;

        if (!ena_tx_slot_posted(sq, slot, avail)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ena: tx packet reaches past the sq doorbell\n");
            return false;
        }
        ena_tx_read_slot(s, sq, slot, &d);
        len_ctrl = le32_to_cpu(d.len_ctrl);

        if (len_ctrl & ENA_ETH_IO_TX_DESC_META_DESC_MASK) {
            if (slot != 0 || !(len_ctrl & ENA_ETH_IO_TX_META_DESC_EXT_VALID_MASK)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "ena: tx meta descriptor not first or not ext_valid\n");
                return false;
            }
            ena_tx_read_meta(p, (const void *)&d);
            continue;
        }
        if (p->ndesc == 0 && !p->have_meta &&
            !(len_ctrl & ENA_ETH_IO_TX_DESC_FIRST_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ena: tx packet without a first descriptor\n");
            return false;
        }
        if (p->ndesc == ENA_MAX_PKT_DESCS) {
            break;
        }
        p->desc[p->ndesc++] = d;
        if (len_ctrl & ENA_ETH_IO_TX_DESC_LAST_MASK) {
            p->slots = slot + 1;
            return true;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR, "ena: tx packet exceeds %u descriptors\n",
                  ENA_MAX_PKT_DESCS);
    return false;
}

#define ENA_TX_OFFLOAD_MASK \
    (ENA_ETH_IO_TX_DESC_TSO_EN_MASK | ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK | \
     ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK)

/* L3/L4 protocols of the linear frame; IPv6 extension headers are not walked */
static void ena_tx_frame_protos(const uint8_t *buf, size_t len, bool *ip4,
                                uint8_t *l4)
{
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
    size_t l2;
    uint16_t proto;

    *ip4 = false;
    *l4 = 0;
    if (len < ETH_HLEN + 4) {
        return;
    }
    l2 = eth_get_l2_hdr_length(buf);
    proto = eth_get_l3_proto(&iov, 1, l2);
    if (proto == ETH_P_IP && len >= l2 + sizeof(struct ip_header)) {
        const struct ip_header *ip = (const void *)(buf + l2);
        size_t ihl = IP_HDR_GET_LEN(buf + l2);

        if (IP_HEADER_VERSION(ip) == 4 && ihl >= sizeof(*ip) && len >= l2 + ihl) {
            *ip4 = true;
            *l4 = ip->ip_p;
        }
    } else if (proto == ETH_P_IPV6 && len >= l2 + sizeof(struct ip6_header)) {
        const struct ip6_header *ip6 = (const void *)(buf + l2);

        *l4 = ip6->ip6_nxt;
    }
}

/* IPv4 header checksum in place; ip_len stays as the driver wrote it */
static void ena_tx_ip4_csum(uint8_t *buf)
{
    uint8_t *ip = buf + eth_get_l2_hdr_length(buf);
    struct ip_header *hdr = (void *)ip;

    hdr->ip_sum = 0;
    hdr->ip_sum = cpu_to_be16(net_raw_checksum(ip, IP_HDR_GET_LEN(ip)));
}

static bool ena_tx_offloads(struct NetTxPkt *pkt, uint32_t offloads,
                            uint16_t mss)
{
    if (offloads & ENA_ETH_IO_TX_DESC_TSO_EN_MASK) {
        if (!net_tx_pkt_build_vheader(pkt, true, true, mss)) {
            return false;
        }
        net_tx_pkt_update_ip_checksums(pkt);
        return true;
    }
    return net_tx_pkt_build_vheader(pkt, false, true, 0);
}

static uint32_t ena_tx_desc_len(const struct ena_eth_io_tx_desc *d)
{
    return le32_to_cpu(d->len_ctrl) & ENA_ETH_IO_TX_DESC_LENGTH_MASK;
}

static uint64_t ena_tx_desc_addr(const struct ena_eth_io_tx_desc *d)
{
    uint32_t hi = le32_to_cpu(d->buff_addr_hi_hdr_sz);

    return le32_to_cpu(d->buff_addr_lo) |
           ((uint64_t)(hi & ENA_ETH_IO_TX_DESC_ADDR_HI_MASK) << 32);
}

/* Copies the pushed header and the buffers, then hands the frame to the backend */
static void ena_tx_xmit(EnaState *s, const EnaSq *sq, const EnaTxPkt *p)
{
    struct NetTxPkt *pkt = s->tx_pkt;
    uint32_t meta_ctrl = le32_to_cpu(p->desc[0].meta_ctrl);
    uint32_t hdr_len = (le32_to_cpu(p->desc[0].buff_addr_hi_hdr_sz) &
                        ENA_ETH_IO_TX_DESC_HEADER_LENGTH_MASK) >>
                       ENA_ETH_IO_TX_DESC_HEADER_LENGTH_SHIFT;
    g_autofree uint8_t *buf = NULL;
    size_t total = 0;
    uint32_t offloads;
    uint8_t l4;
    bool ip4;
    unsigned i;

    if (!sq->llq && hdr_len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ena: tx header_length %u with host placement\n", hdr_len);
        return;
    }
    if (hdr_len > sq->entry_size - ENA_LLQ_HEADER_OFF) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: tx pushed header of %u bytes\n",
                      hdr_len);
        return;
    }
    total = hdr_len;
    for (i = 0; i < p->ndesc; i++) {
        total += ena_tx_desc_len(&p->desc[i]);
    }
    if (total == 0 || total > ENA_TX_MAX_PKT) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: tx packet of %zu bytes\n", total);
        return;
    }

    buf = g_malloc0(total);
    if (hdr_len) {
        memcpy(buf, ena_tx_llq_header(s, sq), hdr_len);
    }
    total = hdr_len;
    for (i = 0; i < p->ndesc; i++) {
        uint32_t len = ena_tx_desc_len(&p->desc[i]);

        if (!ena_dma_read(s, ena_tx_desc_addr(&p->desc[i]), buf + total, len)) {
            qemu_log_mask(LOG_GUEST_ERROR, "ena: tx buffer read failed\n");
            s->tx_drops++;
            return;
        }
        total += len;
    }

    /* The fabric drops frames with a foreign source MAC. */
    if (total < ETH_HLEN ||
        memcmp(buf + ETH_ALEN, s->conf.macaddr.a, ETH_ALEN)) {
        return;
    }
    offloads = meta_ctrl & ENA_TX_OFFLOAD_MASK;
    if (!(offloads & ENA_ETH_IO_TX_DESC_TSO_EN_MASK) &&
        total > s->mtu + ETH_HLEN + 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: tx frame of %zu bytes over mtu %u\n",
                      total, s->mtu);
        s->tx_drops++;
        return;
    }
    ena_tx_frame_protos(buf, total, &ip4, &l4);
    if ((offloads & ENA_ETH_IO_TX_DESC_TSO_EN_MASK) &&
        (sq->meta.mss == 0 || l4 != IP_PROTO_TCP)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: tso with mss %u on l4 proto %u\n",
                      sq->meta.mss, l4);
        s->tx_drops++;
        return;
    }
    if ((offloads & ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK) && !ip4) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: l3 checksum on a non-IPv4 frame\n");
        offloads &= ~ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK;
    }
    if ((offloads & ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK) &&
        l4 != IP_PROTO_TCP && l4 != IP_PROTO_UDP) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: l4 checksum on l4 proto %u\n", l4);
        offloads &= ~ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK;
    }
    if ((offloads & ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK) &&
        !(offloads & ENA_ETH_IO_TX_DESC_TSO_EN_MASK)) {
        ena_tx_ip4_csum(buf);
        offloads &= ~ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK;
    }
    /* frames without L4 work go out verbatim */
    if (!offloads) {
        qemu_send_packet(qemu_get_queue(s->nic), buf, total);
        s->tx_pkts++;
        s->tx_bytes += total;
        return;
    }

    net_tx_pkt_add_raw_fragment(pkt, buf, total);
    if (!net_tx_pkt_parse(pkt) || !ena_tx_offloads(pkt, offloads, sq->meta.mss)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: offload 0x%x on an unparsable frame\n",
                      offloads);
        s->tx_drops++;
    } else if (net_tx_pkt_send(pkt, qemu_get_queue(s->nic))) {
        s->tx_pkts++;
        s->tx_bytes += net_tx_pkt_get_total_len(pkt);
    }
    net_tx_pkt_reset(pkt, ena_tx_free_frag, NULL);
}

static void ena_tx_complete(EnaState *s, const EnaSq *sq, uint16_t req_id)
{
    EnaCq *cq = &s->cq[sq->cq_idx];
    uint8_t entry[ENA_MAX_CQ_ENTRY_SIZE] = {};
    struct ena_eth_io_tx_cdesc *cdesc = (void *)entry;

    assert(cq->entry_size <= sizeof(entry));
    cdesc->req_id = cpu_to_le16(req_id);
    cdesc->status = 0;
    cdesc->flags = cq->phase ? ENA_ETH_IO_TX_CDESC_PHASE_MASK : 0;
    cdesc->sub_qid = cpu_to_le16(sq - s->sq);
    cdesc->sq_head_idx = cpu_to_le16(sq->head);
    ena_cq_push(s, cq, entry);
    ena_cq_intr(s, cq, true);
}

void ena_tx_doorbell(EnaState *s, EnaSq *sq)
{
    assert(sq->is_tx);

    if (!s->cq[sq->cq_idx].used) {
        return;
    }
    while (sq->head != sq->tail) {
        EnaTxPkt p;
        uint32_t len_ctrl, meta_ctrl;
        uint16_t req_id;

        /* a malformed packet discards the posted burst */
        if (!ena_tx_parse(s, sq, &p) || p.ndesc == 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "ena: tx burst discarded\n");
            s->tx_drops++;
            sq->head = sq->tail;
            return;
        }
        if (p.have_meta) {
            sq->meta = p.meta;
        }

        len_ctrl = le32_to_cpu(p.desc[0].len_ctrl);
        meta_ctrl = le32_to_cpu(p.desc[0].meta_ctrl);
        req_id = ((meta_ctrl & ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK) >>
                  ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT) |
                 (((len_ctrl & ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK) >>
                   ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT) << 10);

        ena_tx_xmit(s, sq, &p);
        sq->head += ena_tx_pkt_entries(sq, p.slots);
        if (len_ctrl & ENA_ETH_IO_TX_DESC_COMP_REQ_MASK) {
            ena_tx_complete(s, sq, req_id);
        }
    }
}
