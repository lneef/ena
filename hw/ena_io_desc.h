/*
 * ENA datapath descriptor wire formats (TX SQ, TX meta, TX CQ,
 * RX SQ, RX CQ).
 *
 * Layouts per docs/wiki/tx-descriptors.md and docs/wiki/rx-descriptors.md,
 * taken from the reference ena_eth_io_defs.h. All descriptors are
 * little-endian 32-bit words.
 */
#ifndef ENA_IO_DESC_H
#define ENA_IO_DESC_H

#include <stdint.h>

/* L3/L4 protocol indices used in TX meta_ctrl and RX cdesc status */
enum ena_eth_io_l3_proto_index {
    ENA_ETH_IO_L3_PROTO_UNKNOWN = 0,
    ENA_ETH_IO_L3_PROTO_IPV4    = 8,
    ENA_ETH_IO_L3_PROTO_IPV6    = 11,
    ENA_ETH_IO_L3_PROTO_FCOE    = 21,
    ENA_ETH_IO_L3_PROTO_ROCE    = 22,
};

enum ena_eth_io_l4_proto_index {
    ENA_ETH_IO_L4_PROTO_UNKNOWN         = 0,
    ENA_ETH_IO_L4_PROTO_TCP             = 12,
    ENA_ETH_IO_L4_PROTO_UDP             = 13,
    ENA_ETH_IO_L4_PROTO_ROUTEABLE_ROCE  = 23,
};

/*
 * TX SQ data descriptor, 16 bytes. A TX transaction is
 * [meta?] data(first) [data ...] data(last); len_ctrl bit 23 = 0 marks a
 * data descriptor. req_id = req_id_lo | (req_id_hi << 10), carried on the
 * first data descriptor only.
 */
struct ena_eth_io_tx_desc {
    uint32_t len_ctrl;
    uint32_t meta_ctrl;
    uint32_t buff_addr_lo;
    uint32_t buff_addr_hi_hdr_sz;
};

/* tx_desc.len_ctrl */
#define ENA_ETH_IO_TX_DESC_LENGTH_MASK          0x0000ffff
#define ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT      16
#define ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK       0x003f0000 /* req_id[15:10] */
#define ENA_ETH_IO_TX_DESC_META_DESC_SHIFT      23
#define ENA_ETH_IO_TX_DESC_META_DESC_MASK       0x00800000
#define ENA_ETH_IO_TX_DESC_PHASE_SHIFT          24
#define ENA_ETH_IO_TX_DESC_PHASE_MASK           0x01000000
#define ENA_ETH_IO_TX_DESC_FIRST_SHIFT          26
#define ENA_ETH_IO_TX_DESC_FIRST_MASK           0x04000000
#define ENA_ETH_IO_TX_DESC_LAST_SHIFT           27
#define ENA_ETH_IO_TX_DESC_LAST_MASK            0x08000000
#define ENA_ETH_IO_TX_DESC_COMP_REQ_SHIFT       28
#define ENA_ETH_IO_TX_DESC_COMP_REQ_MASK        0x10000000

/* tx_desc.meta_ctrl */
#define ENA_ETH_IO_TX_DESC_L3_PROTO_IDX_MASK    0x0000000f
#define ENA_ETH_IO_TX_DESC_DF_SHIFT             4
#define ENA_ETH_IO_TX_DESC_DF_MASK              0x00000010
#define ENA_ETH_IO_TX_DESC_TSO_EN_SHIFT         7
#define ENA_ETH_IO_TX_DESC_TSO_EN_MASK          0x00000080
#define ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT   8
#define ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_MASK    0x00001f00
#define ENA_ETH_IO_TX_DESC_L3_CSUM_EN_SHIFT     13
#define ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK      0x00002000
#define ENA_ETH_IO_TX_DESC_L4_CSUM_EN_SHIFT     14
#define ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK      0x00004000
#define ENA_ETH_IO_TX_DESC_ETHERNET_FCS_DIS_SHIFT 15
#define ENA_ETH_IO_TX_DESC_ETHERNET_FCS_DIS_MASK 0x00008000
#define ENA_ETH_IO_TX_DESC_L4_CSUM_PARTIAL_SHIFT 17
#define ENA_ETH_IO_TX_DESC_L4_CSUM_PARTIAL_MASK 0x00020000
#define ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT      22
#define ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK       0xffc00000 /* req_id[9:0] */

/* tx_desc.buff_addr_hi_hdr_sz */
#define ENA_ETH_IO_TX_DESC_ADDR_HI_MASK         0x0000ffff /* paddr[47:32] */
#define ENA_ETH_IO_TX_DESC_HEADER_LENGTH_SHIFT  24
#define ENA_ETH_IO_TX_DESC_HEADER_LENGTH_MASK   0xff000000

/*
 * TX meta descriptor, 16 bytes, same SQ, emitted before the data
 * descriptors of the packet it describes; len_ctrl bit 23 = 1 marks it.
 * MSS is split: mss_lo = mss[9:0] (word2), mss_hi = mss[13:10] (len_ctrl).
 * req_id fields exist but are not populated by the reference driver.
 */
struct ena_eth_io_tx_meta_desc {
    uint32_t len_ctrl;
    uint32_t word1;
    uint32_t word2;
    uint32_t reserved;
};

/* tx_meta_desc.len_ctrl */
#define ENA_ETH_IO_TX_META_DESC_REQ_ID_LO_MASK      0x000003ff
#define ENA_ETH_IO_TX_META_DESC_EXT_VALID_SHIFT     14
#define ENA_ETH_IO_TX_META_DESC_EXT_VALID_MASK      0x00004000
#define ENA_ETH_IO_TX_META_DESC_MSS_HI_SHIFT        16
#define ENA_ETH_IO_TX_META_DESC_MSS_HI_MASK         0x000f0000
#define ENA_ETH_IO_TX_META_DESC_ETH_META_TYPE_SHIFT 20
#define ENA_ETH_IO_TX_META_DESC_ETH_META_TYPE_MASK  0x00100000
#define ENA_ETH_IO_TX_META_DESC_META_STORE_SHIFT    21
#define ENA_ETH_IO_TX_META_DESC_META_STORE_MASK     0x00200000
#define ENA_ETH_IO_TX_META_DESC_META_DESC_SHIFT     23
#define ENA_ETH_IO_TX_META_DESC_META_DESC_MASK      0x00800000
#define ENA_ETH_IO_TX_META_DESC_PHASE_SHIFT         24
#define ENA_ETH_IO_TX_META_DESC_PHASE_MASK          0x01000000
#define ENA_ETH_IO_TX_META_DESC_FIRST_SHIFT         26
#define ENA_ETH_IO_TX_META_DESC_FIRST_MASK          0x04000000
#define ENA_ETH_IO_TX_META_DESC_LAST_SHIFT          27
#define ENA_ETH_IO_TX_META_DESC_LAST_MASK           0x08000000
#define ENA_ETH_IO_TX_META_DESC_COMP_REQ_SHIFT      28
#define ENA_ETH_IO_TX_META_DESC_COMP_REQ_MASK       0x10000000

/* tx_meta_desc.word1 */
#define ENA_ETH_IO_TX_META_DESC_REQ_ID_HI_MASK      0x0000003f

/* tx_meta_desc.word2 */
#define ENA_ETH_IO_TX_META_DESC_L3_HDR_LEN_MASK     0x000000ff
#define ENA_ETH_IO_TX_META_DESC_L3_HDR_OFF_SHIFT    8
#define ENA_ETH_IO_TX_META_DESC_L3_HDR_OFF_MASK     0x0000ff00
#define ENA_ETH_IO_TX_META_DESC_L4_HDR_LEN_IN_WORDS_SHIFT 16
#define ENA_ETH_IO_TX_META_DESC_L4_HDR_LEN_IN_WORDS_MASK  0x003f0000
#define ENA_ETH_IO_TX_META_DESC_MSS_LO_SHIFT        22
#define ENA_ETH_IO_TX_META_DESC_MSS_LO_MASK         0xffc00000

/*
 * TX completion descriptor, 8 bytes, device-written to the TX CQ.
 * One per packet whose first data descriptor had comp_req set; req_id
 * echoes the packet's req_id and must be < q_depth.
 */
struct ena_eth_io_tx_cdesc {
    uint16_t req_id;
    uint8_t  status;
    uint8_t  flags;
    uint16_t sub_qid;
    uint16_t sq_head_idx;
};

/* tx_cdesc.flags */
#define ENA_ETH_IO_TX_CDESC_PHASE_MASK          0x01
#define ENA_ETH_IO_TX_CDESC_MBZ6_SHIFT          6
#define ENA_ETH_IO_TX_CDESC_MBZ6_MASK           0xc0

/*
 * RX SQ descriptor, 16 bytes, always a host-memory ring. The driver posts
 * one buffer per descriptor with FIRST|LAST|COMP_REQ set in ctrl.
 * length == 0 means 64 KiB.
 */
struct ena_eth_io_rx_desc {
    uint16_t length;
    uint8_t  reserved2;
    uint8_t  ctrl;
    uint16_t req_id;
    uint16_t reserved6;
    uint32_t buff_addr_lo;
    uint16_t buff_addr_hi;
    uint16_t reserved16_w3;
};

/* rx_desc.ctrl (masks relative to the ctrl byte) */
#define ENA_ETH_IO_RX_DESC_PHASE_MASK           0x01
#define ENA_ETH_IO_RX_DESC_FIRST_SHIFT          2
#define ENA_ETH_IO_RX_DESC_FIRST_MASK           0x04
#define ENA_ETH_IO_RX_DESC_LAST_SHIFT           3
#define ENA_ETH_IO_RX_DESC_LAST_MASK            0x08
#define ENA_ETH_IO_RX_DESC_COMP_REQ_SHIFT       4
#define ENA_ETH_IO_RX_DESC_COMP_REQ_MASK        0x10

/*
 * RX completion descriptor (base form), 16 bytes, device-written.
 * The reference driver only ever configures this 4-word form (the 32-byte
 * ext form is unused). Parsing fields in status are valid only on the
 * cdesc with last=1; a multi-buffer packet writes N consecutive cdescs,
 * each carrying its own per-buffer length and req_id.
 */
struct ena_eth_io_rx_cdesc_base {
    uint32_t status;
    uint16_t length;
    uint16_t req_id;
    uint32_t hash;
    uint16_t sub_qid;
    uint8_t  offset;
    uint8_t  reserved;
};

/* rx_cdesc_base.status; note phase is bit 24 here, unlike the RX SQ
 * descriptor where it is bit 0 of the ctrl byte. */
#define ENA_ETH_IO_RX_CDESC_BASE_L3_PROTO_IDX_MASK      0x0000001f
#define ENA_ETH_IO_RX_CDESC_BASE_SRC_VLAN_CNT_SHIFT     5
#define ENA_ETH_IO_RX_CDESC_BASE_SRC_VLAN_CNT_MASK      0x00000060
#define ENA_ETH_IO_RX_CDESC_BASE_MBZ7_SHIFT             7
#define ENA_ETH_IO_RX_CDESC_BASE_MBZ7_MASK              0x00000080
#define ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_SHIFT     8
#define ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_MASK      0x00001f00
#define ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_SHIFT      13
#define ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK       0x00002000
#define ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_SHIFT      14
#define ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK       0x00004000
#define ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_SHIFT        15
#define ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_MASK         0x00008000
#define ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_SHIFT  16
#define ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK   0x00010000
#define ENA_ETH_IO_RX_CDESC_BASE_MBZ17_SHIFT            17
#define ENA_ETH_IO_RX_CDESC_BASE_MBZ17_MASK             0x00020000
#define ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT            24
#define ENA_ETH_IO_RX_CDESC_BASE_PHASE_MASK             0x01000000
#define ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM2_SHIFT         25
#define ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM2_MASK          0x02000000
#define ENA_ETH_IO_RX_CDESC_BASE_FIRST_SHIFT            26
#define ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK             0x04000000
#define ENA_ETH_IO_RX_CDESC_BASE_LAST_SHIFT             27
#define ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK              0x08000000
#define ENA_ETH_IO_RX_CDESC_BASE_BUFFER_SHIFT           30
#define ENA_ETH_IO_RX_CDESC_BASE_BUFFER_MASK            0x40000000

#endif /* ENA_IO_DESC_H */
