/*
 * Shared helpers for the ENA TX datapath qtests: queue pair setup,
 * descriptor construction, completion polling and frame builders.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TESTS_ENA_TX_UTIL_H
#define TESTS_ENA_TX_UTIL_H

#include "libqtest.h"
#include "net/eth.h"
#include "tests/ena_qos.h"

#define ENA_TX_DESC_SIZE        16
#define ENA_LLQ_LINE_SIZE       128
#define ENA_LLQ_HEADER_OFF      32
#define ENA_MAX_CDESC_SIZE      32
#define ENA_TEST_MAC_DST        { 0x52, 0x54, 0x00, 0x00, 0x00, 0x01 }

typedef struct EnaTxQueue {
    uint16_t depth;
    uint8_t cq_entry_size;
    bool llq;
    uint16_t sq_idx;
    uint16_t cq_idx;
    uint32_t db_off;
    uint32_t unmask_off;
    uint32_t llq_off;
    uint64_t sq_ring;
    uint64_t cq_ring;
    uint16_t tail;
    uint16_t cq_head;
    bool cq_phase;
} EnaTxQueue;

static inline void ena_tx_enable_llq(QEna *d)
{
    struct ena_admin_set_feat_cmd cmd = {};

    cmd.feat_common.feature_id = ENA_ADMIN_LLQ;
    cmd.u.llq.header_location_ctrl_enabled = cpu_to_le16(ENA_ADMIN_INLINE_HEADER);
    cmd.u.llq.entry_size_ctrl_enabled = cpu_to_le16(ENA_ADMIN_LIST_ENTRY_SIZE_128B);
    cmd.u.llq.desc_num_before_header_enabled =
        cpu_to_le16(ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2);
    cmd.u.llq.descriptors_stride_ctrl_enabled =
        cpu_to_le16(ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_SUCCESS);
}

static inline void ena_txq_create(QEna *d, EnaTxQueue *q, uint16_t depth,
                                  uint8_t cq_words, uint32_t vector, bool llq)
{
    struct ena_admin_acq_create_cq_resp_desc cq;
    struct ena_admin_acq_create_sq_resp_desc sq;

    memset(q, 0, sizeof(*q));
    q->depth = depth;
    q->cq_entry_size = cq_words * 4;
    q->llq = llq;
    q->cq_phase = true;
    q->cq_ring = guest_alloc(d->alloc, depth * q->cq_entry_size);
    qtest_memset(d->dev.bus->qts, q->cq_ring, 0, depth * q->cq_entry_size);
    if (!llq) {
        q->sq_ring = guest_alloc(d->alloc, depth * ENA_TX_DESC_SIZE);
    }

    g_assert_cmpint(ena_create_cq(d, depth, cq_words, vector, q->cq_ring, &cq),
                    ==, ENA_ADMIN_SUCCESS);
    q->cq_idx = le16_to_cpu(cq.cq_idx);
    q->unmask_off = le32_to_cpu(cq.cq_interrupt_unmask_register_offset);
    g_assert_cmpint(ena_create_sq(d, true,
                                  llq ? ENA_ADMIN_PLACEMENT_POLICY_DEV :
                                        ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                  q->cq_idx, depth, q->sq_ring, &sq), ==,
                    ENA_ADMIN_SUCCESS);
    q->sq_idx = le16_to_cpu(sq.sq_idx);
    q->db_off = le32_to_cpu(sq.sq_doorbell_offset);
    q->llq_off = le32_to_cpu(sq.llq_descriptors_offset);
}

static inline void ena_tx_desc_fill(struct ena_eth_io_tx_desc *dsc,
                                    uint64_t addr, uint16_t len,
                                    uint16_t req_id, uint32_t ctrl_flags,
                                    uint32_t meta_ctrl, uint8_t hdr_len)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->len_ctrl = cpu_to_le32(len | ctrl_flags |
                                (((req_id >> 10) << ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT) &
                                 ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK));
    dsc->meta_ctrl = cpu_to_le32(meta_ctrl |
                                 (((req_id & 0x3ff) << ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT) &
                                  ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK));
    dsc->buff_addr_lo = cpu_to_le32(addr);
    dsc->buff_addr_hi_hdr_sz = cpu_to_le32(((addr >> 32) & ENA_ETH_IO_TX_DESC_ADDR_HI_MASK) |
                                           ((uint32_t)hdr_len << ENA_ETH_IO_TX_DESC_HEADER_LENGTH_SHIFT));
}

static inline void ena_tx_meta_fill(struct ena_eth_io_tx_meta_desc *m,
                                    uint16_t mss, uint8_t l3_hdr_len,
                                    uint8_t l3_hdr_off, uint8_t l4_hdr_words)
{
    memset(m, 0, sizeof(*m));
    m->len_ctrl = cpu_to_le32(ENA_ETH_IO_TX_META_DESC_META_DESC_MASK |
                              ENA_ETH_IO_TX_META_DESC_EXT_VALID_MASK |
                              ENA_ETH_IO_TX_META_DESC_ETH_META_TYPE_MASK |
                              ENA_ETH_IO_TX_META_DESC_META_STORE_MASK |
                              ENA_ETH_IO_TX_META_DESC_FIRST_MASK |
                              (((mss >> 10) << ENA_ETH_IO_TX_META_DESC_MSS_HI_SHIFT) &
                               ENA_ETH_IO_TX_META_DESC_MSS_HI_MASK));
    m->word2 = cpu_to_le32(l3_hdr_len |
                           ((uint32_t)l3_hdr_off << ENA_ETH_IO_TX_META_DESC_L3_HDR_OFF_SHIFT) |
                           ((uint32_t)l4_hdr_words << ENA_ETH_IO_TX_META_DESC_L4_HDR_LEN_IN_WORDS_SHIFT) |
                           (((uint32_t)(mss & 0x3ff) << ENA_ETH_IO_TX_META_DESC_MSS_LO_SHIFT) &
                            ENA_ETH_IO_TX_META_DESC_MSS_LO_MASK));
}

/* host placement: append one 16-byte descriptor to the ring */
static inline void ena_txq_push(QEna *d, EnaTxQueue *q, const void *desc)
{
    g_assert(!q->llq);
    qtest_memwrite(d->dev.bus->qts,
                   q->sq_ring + (q->tail & (q->depth - 1)) * ENA_TX_DESC_SIZE,
                   desc, ENA_TX_DESC_SIZE);
    q->tail++;
}

/* device placement: write one 128-byte line into the LLQ memory */
static inline void ena_txq_push_line(QEna *d, EnaTxQueue *q, const void *line)
{
    g_assert(q->llq);
    qpci_memwrite(&d->dev, d->mem,
                  q->llq_off + (q->tail & (q->depth - 1)) * ENA_LLQ_LINE_SIZE,
                  line, ENA_LLQ_LINE_SIZE);
    q->tail++;
}

static inline void ena_txq_doorbell(QEna *d, EnaTxQueue *q)
{
    ena_reg_write(d, q->db_off, q->tail);
}

static inline bool ena_txq_poll_cdesc(QEna *d, EnaTxQueue *q,
                                      struct ena_eth_io_tx_cdesc *c)
{
    uint8_t entry[ENA_MAX_CDESC_SIZE];

    qtest_memread(d->dev.bus->qts,
                  q->cq_ring + (q->cq_head & (q->depth - 1)) * q->cq_entry_size,
                  entry, q->cq_entry_size);
    memcpy(c, entry, sizeof(*c));
    if ((c->flags & ENA_ETH_IO_TX_CDESC_PHASE_MASK) != q->cq_phase) {
        return false;
    }
    q->cq_head++;
    if ((q->cq_head & (q->depth - 1)) == 0) {
        q->cq_phase = !q->cq_phase;
    }
    return true;
}

static inline void ena_txq_unmask(QEna *d, EnaTxQueue *q, uint32_t tx_delay,
                                  bool moderation_update)
{
    uint32_t val = ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK |
                   ((tx_delay << ENA_ETH_IO_INTR_REG_TX_INTR_DELAY_SHIFT) &
                    ENA_ETH_IO_INTR_REG_TX_INTR_DELAY_MASK);

    if (!moderation_update) {
        val |= ENA_ETH_IO_INTR_REG_NO_MODERATION_UPDATE_MASK;
    }
    ena_reg_write(d, q->unmask_off, val);
}

/* frame builders */

static inline uint16_t ena_csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum & 0xffff;
}

static inline uint32_t ena_csum_add(uint32_t sum, const uint8_t *p, size_t len)
{
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += (p[i] << 8) | p[i + 1];
    }
    if (i < len) {
        sum += p[i] << 8;
    }
    return sum;
}

/* checksum of the IPv4 header in a frame, as stored in network byte order */
static inline uint16_t ena_ipv4_csum(const uint8_t *frame)
{
    return ena_csum_fold(ena_csum_add(0, frame + ETH_HLEN, sizeof(struct ip_header)));
}

/* TCP/UDP checksum over pseudo header and L4 payload of an IPv4 frame */
static inline uint16_t ena_l4_csum(const uint8_t *frame, size_t frame_len)
{
    const uint8_t *ip = frame + ETH_HLEN;
    size_t l4_len = frame_len - ETH_HLEN - sizeof(struct ip_header);
    uint8_t pseudo[12];
    uint32_t sum;

    memcpy(pseudo, ip + 12, 8);
    pseudo[8] = 0;
    pseudo[9] = ip[9];
    pseudo[10] = l4_len >> 8;
    pseudo[11] = l4_len & 0xff;
    sum = ena_csum_add(0, pseudo, sizeof(pseudo));
    sum = ena_csum_add(sum, ip + sizeof(struct ip_header), l4_len);
    return ena_csum_fold(sum);
}

/* Ethernet frame with the IEEE experimental ethertype and a counting payload */
static inline size_t ena_build_eth(uint8_t *frame, size_t payload_len)
{
    static const uint8_t dst[] = ENA_TEST_MAC_DST;
    static const uint8_t src[] = ENA_TEST_MAC;
    size_t i;

    memcpy(frame, dst, 6);
    memcpy(frame + 6, src, 6);
    frame[12] = 0x88;
    frame[13] = 0xb5;
    for (i = 0; i < payload_len; i++) {
        frame[ETH_HLEN + i] = i & 0xff;
    }
    return ETH_HLEN + payload_len;
}

/* Ethernet/IPv4/UDP or TCP frame with zero checksums and a counting payload */
static inline size_t ena_build_ipv4(uint8_t *frame, uint8_t proto,
                                    size_t payload_len)
{
    size_t l4_hdr = proto == IP_PROTO_TCP ? sizeof(struct tcp_header) :
                                            sizeof(struct udp_header);
    size_t ip_len = sizeof(struct ip_header) + l4_hdr + payload_len;
    uint8_t *ip = frame + ETH_HLEN;
    uint8_t *l4 = ip + sizeof(struct ip_header);
    size_t i;

    ena_build_eth(frame, 0);
    frame[12] = 0x08;
    frame[13] = 0x00;
    memset(ip, 0, ip_len);
    ip[0] = 0x45;
    ip[2] = ip_len >> 8;
    ip[3] = ip_len & 0xff;
    ip[8] = 64;
    ip[9] = proto;
    ip[12] = 10; ip[13] = 0; ip[14] = 0; ip[15] = 1;
    ip[16] = 10; ip[17] = 0; ip[18] = 0; ip[19] = 2;
    l4[0] = 0x12; l4[1] = 0x34;
    l4[2] = 0x56; l4[3] = 0x78;
    if (proto == IP_PROTO_TCP) {
        l4[12] = (sizeof(struct tcp_header) / 4) << 4;
        l4[13] = 0x10;
    } else {
        l4[4] = (l4_hdr + payload_len) >> 8;
        l4[5] = (l4_hdr + payload_len) & 0xff;
    }
    for (i = 0; i < payload_len; i++) {
        l4[l4_hdr + i] = i & 0xff;
    }
    return ETH_HLEN + ip_len;
}

#endif
