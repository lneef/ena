# TX Descriptors (TX SQ descriptor, TX meta descriptor, TX completion)

Wire formats the device must consume on a TX submission queue (SQ) and the
completion descriptor the device must write to a TX completion queue (CQ).
This page documents the on-wire bit layout from `ena_eth_io_defs.h` and how
the reference driver (`ena_com_prepare_tx`, `ena_com_create_meta`, and the
DPDK PMD `ena_tx_mbuf_prepare`) fills each field.

Reference driver: AWS DPDK PMD (`userspace/dpdk/ena`). Paths below are
relative to `~/ena/amzn-drivers/`. All bit/field definitions are from
`userspace/dpdk/ena/base/ena_defs/ena_eth_io_defs.h`.

For SQ/CQ creation, ring depth, LLQ vs host placement, doorbells and the
req_id-as-ring-index convention see [queue-setup.md](queue-setup.md). For the
LLQ bounce-buffer / push-header mechanics see that page's LLQ section.

All descriptors are little-endian 32-bit words. A "TX transaction" is the
ordered set of descriptors for one packet: an optional meta descriptor,
followed by `first` ... `last` data descriptors.

---

## 1. struct ena_eth_io_tx_desc (regular/data TX SQ descriptor)

16 bytes, four 32-bit words `len_ctrl`, `meta_ctrl`, `buff_addr_lo`,
`buff_addr_hi_hdr_sz` [ena_eth_io_defs.h:24-98].

### Word 0 — `len_ctrl` [ena_eth_io_defs.h:24-46, masks :282-294]

| Bits | Field | Mask/macro | Meaning |
|------|-------|-----------|---------|
| 15:0 | `length` | `LENGTH_MASK` GENMASK(15,0) | Buffer length in bytes for **this** descriptor's buffer. Must include packet trailers the device updates (E2E CRC, GMAC) but **not** the Push_Buffer length and **not** the 4-byte 802.3 FCS. |
| 21:16 | `req_id_hi` | `REQ_ID_HI_MASK` GENMASK(21,16), shift 16 | Request ID high part. Driver writes `req_id >> 10` here, so these bits carry req_id[15:10]. (Struct comment's "[15:10]" is the value range; the field is 6 bits wide.) |
| 22 | reserved22 | — | MBZ |
| 23 | `meta_desc` | `META_DESC_MASK` BIT(23) | MBZ on a data descriptor (this bit set => meta descriptor; see §2). The device uses bit 23 to discriminate data vs meta. |
| 24 | `phase` | `PHASE_MASK` BIT(24) | Producer phase bit (see §5). |
| 25 | reserved1 | — | MBZ |
| 26 | `first` | `FIRST_MASK` BIT(26) | First data descriptor of the transaction. |
| 27 | `last` | `LAST_MASK` BIT(27) | Last data descriptor of the transaction. |
| 28 | `comp_req` | `COMP_REQ_MASK` BIT(28) | Request a TX completion be posted after transmit. Valid only on the first descriptor. |
| 30:29 | reserved29 | — | MBZ |
| 31 | reserved31 | — | MBZ |

### Word 1 — `meta_ctrl` (per-packet offload control) [ena_eth_io_defs.h:48-78, masks :295-311]

| Bits | Field | Mask/macro | Meaning |
|------|-------|-----------|---------|
| 3:0 | `l3_proto_idx` | `L3_PROTO_IDX_MASK` GENMASK(3,0) | L3 protocol index (see enum §4-bis). Required when `l3_csum_en` or `tso_en` set. |
| 4 | `DF` | `DF_MASK` BIT(4) | IPv4 Don't-Fragment. Must be 0 if IPv4 and header DF==0; else must be 1. Always 1 for IPv6. |
| 6:5 | reserved5 | — | — |
| 7 | `tso_en` | `TSO_EN_MASK` BIT(7) | Enable TSO (TCP only). |
| 12:8 | `l4_proto_idx` | `L4_PROTO_IDX_MASK` GENMASK(12,8), shift 8 | L4 protocol index. Required when `l4_csum_en` or `tso_en` set. |
| 13 | `l3_csum_en` | `L3_CSUM_EN_MASK` BIT(13) | Enable IPv4 header checksum offload. |
| 14 | `l4_csum_en` | `L4_CSUM_EN_MASK` BIT(14) | Enable TCP/UDP checksum offload. |
| 15 | `ethernet_fcs_dis` | `ETHERNET_FCS_DIS_MASK` BIT(15) | When set, the controller does NOT append the 802.3 Ethernet FCS. |
| 16 | reserved16 | — | — |
| 17 | `l4_csum_partial` | `L4_CSUM_PARTIAL_MASK` BIT(17) | 0: device computes full L4 checksum (pseudo-header taken from packet L3 header). 1: device skips pseudo-header sum and uses the L4 checksum field as-is. For TSO the pseudo-header must exclude TCP length; partial is required for IPv6 with Routing Headers. |
| 20:18 | reserved18 | — | MBZ |
| 21 | reserved21 | — | MBZ |
| 31:22 | `req_id_lo` | `REQ_ID_LO_MASK` GENMASK(31,22), shift 22 | Request ID low part; carries req_id[9:0]. |

### Word 2 — `buff_addr_lo`

Bits 31:0 = low 32 bits of the buffer DMA address [ena_eth_io_defs.h:80].

### Word 3 — `buff_addr_hi_hdr_sz` [ena_eth_io_defs.h:82-97, masks :312-314]

| Bits | Field | Mask/macro | Meaning |
|------|-------|-----------|---------|
| 15:0 | `addr_hi` | `ADDR_HI_MASK` GENMASK(15,0) | Buffer pointer bits [47:32]. Driver masks paddr with `GENMASK_ULL(dma_addr_bits-1, 32)` before placing [ena_eth_com.c:597-602]. |
| 23:16 | reserved16_w2 | — | — |
| 31:24 | `header_length` | `HEADER_LENGTH_MASK` GENMASK(31,24), shift 24 | For LLQ: number of bytes written to the headers' memory (push buffer). For host queues: if TCP/UDP and longer than `max_header_size`, set to (L4 offset + L4 header size without options); otherwise 0. Must never exceed `max_header_size` (reported by the Max Queues feature descriptor). |

### How the driver fills it (`ena_com_prepare_tx`) [ena_eth_com.c:461-620]

- The **first data descriptor shares the same descriptor slot as the header**:
  `header_length` and `comp_req` and the offload `meta_ctrl` bits are written
  into the first data descriptor [ena_eth_com.c:520-571].
- `first` (BIT 26) is set on the first data descriptor **only when there is no
  meta descriptor**; when a meta descriptor precedes the packet, the meta
  descriptor carries `first`, not the data descriptor [ena_eth_com.c:526-527,
  385].
- `phase` is set to `io_sq->phase` on every data descriptor [ena_eth_com.c:533-535,
  589-591].
- `comp_req` (BIT 28) is always set on the first data descriptor
  [ena_eth_com.c:537].
- `req_id` is split across the first descriptor only: `req_id_lo`=req_id[9:0]
  into `meta_ctrl` [ena_eth_com.c:540-542], `req_id_hi`=req_id>>10 into
  `len_ctrl` [ena_eth_com.c:549-551] (see §4).
- The offload fields (`tso_en`, `l3_proto_idx`, `l4_proto_idx`, `l3_csum_en`,
  `l4_csum_en`, `l4_csum_partial`) are written into `meta_ctrl` **only when
  `ena_tx_ctx->meta_valid` is true** [ena_eth_com.c:553-571]. `DF` is written
  unconditionally [ena_eth_com.c:544-546].
- Each subsequent buffer gets its own descriptor with only `phase`, `length`,
  `buff_addr_lo`, `buff_addr_hi` set [ena_eth_com.c:573-604].
- `last` (BIT 27) is set on the final data descriptor [ena_eth_com.c:607].

Note: the setters in the header use `|=` and the driver `memset`s each
descriptor to 0 before filling [ena_eth_com.c:523, 587], so reserved bits are
zero on the wire.

---

## 2. struct ena_eth_io_tx_meta_desc (TX metadata SQ descriptor)

16 bytes, words `len_ctrl`, `word1`, `word2`, `reserved`
[ena_eth_io_defs.h:100-146]. Occupies one SQ descriptor slot in the same SQ,
emitted **before** the data descriptors of the packet it describes.

### Word 0 — `len_ctrl` [ena_eth_io_defs.h:100-128, masks :317-335]

| Bits | Field | Mask/macro | Meaning |
|------|-------|-----------|---------|
| 9:0 | `req_id_lo` | `REQ_ID_LO_MASK` GENMASK(9,0) | Request ID low part (req_id[9:0]). Note: NOT written by `ena_com_create_meta`; left 0 (the data desc carries req_id). |
| 11:10 | reserved10 | — | MBZ |
| 12 | reserved12 | — | MBZ |
| 13 | reserved13 | — | MBZ |
| 14 | `ext_valid` | `EXT_VALID_MASK` BIT(14) | If set, the offset fields in word2, `mss_hi` in word0, and bits [31:24] of word3 are valid. Always set by `ena_com_create_meta`. |
| 15 | reserved15 | — | — |
| 19:16 | `mss_hi` | `MSS_HI_MASK` GENMASK(19,16), shift 16 | MSS bits [13:10] (`mss >> 10`). |
| 20 | `eth_meta_type` | `ETH_META_TYPE_MASK` BIT(20) | 0: Tx Metadata Descriptor, 1: Extended Metadata Descriptor. Set to 1 by `ena_com_create_meta`. |
| 21 | `meta_store` | `META_STORE_MASK` BIT(21) | Store extended metadata in the queue's meta cache. Set to 1 by `ena_com_create_meta`. |
| 22 | reserved22 | — | MBZ |
| 23 | `meta_desc` | `META_DESC_MASK` BIT(23) | MBO (must-be-one) on a meta descriptor; this is what marks the slot as a meta descriptor. |
| 24 | `phase` | `PHASE_MASK` BIT(24) | Producer phase bit (see §5). |
| 25 | reserved25 | — | MBZ |
| 26 | `first` | `FIRST_MASK` BIT(26) | Set (the meta descriptor is the first descriptor of the transaction). |
| 27 | `last` | `LAST_MASK` BIT(27) | Not set by `ena_com_create_meta`. |
| 28 | `comp_req` | `COMP_REQ_MASK` BIT(28) | Not set by `ena_com_create_meta`. |
| 30:29 | reserved29 | — | MBZ |
| 31 | reserved31 | — | MBZ |

### Word 1 — `word1` [ena_eth_io_defs.h:130-133, mask :336]

| Bits | Field | Mask/macro | Meaning |
|------|-------|-----------|---------|
| 5:0 | `req_id_hi` | `REQ_ID_HI_MASK` GENMASK(5,0) | Request ID high part. Not written by `ena_com_create_meta` (left 0). |
| 31:6 | reserved6 | — | MBZ |

### Word 2 — `word2` [ena_eth_io_defs.h:135-143, masks :337-343]

| Bits | Field | Mask/macro | Meaning |
|------|-------|-----------|---------|
| 7:0 | `l3_hdr_len` | `L3_HDR_LEN_MASK` GENMASK(7,0) | L3 header length in bytes. Driver source = `mbuf->l3_len` [ena_ethdev.c:766]. |
| 15:8 | `l3_hdr_off` | `L3_HDR_OFF_MASK` GENMASK(15,8), shift 8 | L3 header offset (== L2 length). Driver source = `mbuf->l2_len` [ena_ethdev.c:767]. |
| 21:16 | `l4_hdr_len_in_words` | `L4_HDR_LEN_IN_WORDS_MASK` GENMASK(21,16), shift 16 | L4 header length **in 32-bit words**. Device assumes L4 starts at `l3_hdr_off + l3_hdr_len`. |
| 31:22 | `mss_lo` | `MSS_LO_MASK` GENMASK(31,22), shift 22 | MSS bits [9:0] (`mss & 0x3FF`). |

### Word 3 — `reserved` [ena_eth_io_defs.h:145]

Zeroed by the `memset` in `ena_com_create_meta` [ena_eth_com.c:361].

### How the driver fills it (`ena_com_create_meta`) [ena_eth_com.c:352-401]

```
memset(meta_desc, 0, 16);
len_ctrl |= META_DESC | EXT_VALID | ETH_META_TYPE | FIRST | META_STORE
len_ctrl |= phase<<24
len_ctrl |= (mss>>10) into MSS_HI
word2     = MSS_LO(mss) | l3_hdr_len | l3_hdr_off | l4_hdr_len_in_words
```
The 14-bit MSS is split: low 10 bits into `word2` MSS_LO, high 4 bits into
`len_ctrl` MSS_HI [ena_eth_com.c:368-376]. `req_id` fields are **not** written
in the meta descriptor by this driver path.

---

## 3. When a meta descriptor is emitted

The decision is in `ena_com_create_and_store_tx_meta_desc`
[ena_eth_com.c:403-427], gated by `ena_tx_ctx->meta_valid` and the per-SQ
`disable_meta_caching` flag.

`ena_tx_ctx->meta_valid` is set by the PMD `ena_tx_mbuf_prepare`
[ena_ethdev.c:714-776]:
- **true** when the mbuf has any offload requested AND the queue advertises
  matching offloads — i.e. TSO, L3 csum, or L4 (TCP/UDP) csum
  [ena_ethdev.c:721-769]. In that case `ena_meta.{mss,l3_hdr_len,
  l3_hdr_offset}` and (for TSO) `l4_hdr_len` are populated.
- **true** (with a zeroed `ena_meta`) when `disable_meta_caching` is set, even
  with no offloads [ena_ethdev.c:770-772].
- **false** otherwise (no meta descriptor) [ena_ethdev.c:773-774].

Meta-caching rule [ena_eth_com.c:409-426]:
- If `io_sq->disable_meta_caching` is set: always emit a fresh meta descriptor
  whenever `meta_valid` (no comparison) [ena_eth_com.c:412-415].
- Otherwise emit a meta descriptor **only when the meta changed** vs the cached
  copy. `ena_com_meta_desc_changed` returns false if `!meta_valid`, else
  `memcmp(cached_tx_meta, ena_meta, sizeof(struct ena_com_tx_meta)) != 0`
  [ena_eth_com.h:114-123]. On a change the new meta is cached
  (`memcpy(&io_sq->cached_tx_meta, ...)`) before emitting
  [ena_eth_com.c:417-422].

`struct ena_com_tx_meta` (the cached/compared unit): `{u16 mss; u16 l3_hdr_len;
u16 l3_hdr_offset; u16 l4_hdr_len /* words */}` [ena_com.h:80-85].

Offload combinations requiring meta: any TSO, IPv4 header checksum, or TCP/UDP
checksum offload (these set `meta_valid`). Without meta, the data descriptor's
`meta_ctrl` offload bits are left 0 and the device must not apply checksum/TSO
offloads [ena_eth_com.c:553-571 guarded by `meta_valid`].

`disable_meta_caching` provenance: it is an LLQ accel-mode capability
(`BIT(ENA_ADMIN_DISABLE_META_CACHING)` in `llq.accel_mode.u.get.supported_flags`)
read during init [ena_ethdev.c:2392-2398], stored in
`llq_info->disable_meta_caching` [ena_com.c:763-765], and copied to
`io_sq->disable_meta_caching` at queue create [ena_com.c:400-401]. It is only
meaningful for LLQ (DEV placement); host queues use `false`
[ena_ethdev.c:2396-2397]. `ena_com_is_doorbell_needed` also counts an extra
descriptor when caching is disabled or the meta changed [ena_eth_com.h:145-147].

---

## 4. req_id encoding (data desc vs meta desc)

`req_id` identifies the TX buffer slot and is the ring index the device echoes
back in the completion. It is a 16-bit value split across two fields of the
**first data descriptor**:

| Part | Bits in value | Located in | Field | Driver write |
|------|---------------|-----------|-------|--------------|
| low | req_id[9:0] | data desc word1 `meta_ctrl` 31:22 | `req_id_lo` | `req_id` into REQ_ID_LO [ena_eth_com.c:540-542] |
| high | req_id[15:10] | data desc word0 `len_ctrl` 21:16 | `req_id_hi` | `req_id >> 10` into REQ_ID_HI [ena_eth_com.c:549-551] |

So a device reconstructs `req_id = req_id_lo | (req_id_hi << 10)`. Although the
struct comments label `req_id_hi` as "[15:10]", the field is 6 bits wide
(GENMASK(21,16)); only as many high bits as the ring index needs are used (ring
depth <= 16 bits / req_id must be < q_depth).

The **meta descriptor also has `req_id_lo` (word0 9:0) and `req_id_hi`
(word1 5:0)** fields [ena_eth_io_defs.h:101, 130], but this driver's
`ena_com_create_meta` does NOT populate them — req_id is carried only on the
data descriptor. A device must therefore take req_id from the data (first)
descriptor of the transaction.

---

## 5. Phase bit semantics on TX SQ descriptors

- The driver maintains `io_sq->phase`, starting at 1, and writes it into every
  data descriptor and meta descriptor's `phase` bit (BIT 24)
  [ena_eth_com.c:533-535, 589-591, 381-383].
- `phase` is toggled when the SQ tail wraps around the ring:
  `if ((tail & (q_depth-1)) == 0) phase ^= 1` — for both host queues
  [ena_eth_com.c:266-267] and LLQ writes [ena_eth_com.c:133-134].
- Device consumption rule (mirroring how the driver reads CQs): a descriptor at
  the device's current SQ head is "owned"/valid only when its `phase` bit
  matches the device's expected phase; on each wrap the expected phase flips.
  This lets the device poll SQ memory (host placement) without a separate valid
  flag. For LLQ the descriptors arrive via the doorbell/bounce-buffer write, but
  the same phase value is present in the data.
- Doorbell: the driver rings the SQ doorbell with the new `tail`
  [ena_eth_com.h:162-181]; the device should consume descriptors up to that
  tail. (Phase additionally disambiguates wrap.)

---

## 6. struct ena_eth_io_tx_cdesc (TX completion descriptor)

8 bytes, written by the **device** to the TX CQ [ena_eth_io_defs.h:148-164,
masks :345-348].

| Offset | Field | Type | Meaning |
|--------|-------|------|---------|
| 0 | `req_id` | u16 | Request ID[15:0] of the completed packet — the value the driver placed in the data descriptor (§4). Driver validates `req_id < q_depth` [ena_eth_com.h:251-256]. |
| 2 | `status` | u8 | Per-packet status (0 = success in normal operation; non-zero reserved for error reporting). |
| 3 | `flags` | u8 | bit0 `phase` (`PHASE_MASK` BIT(0)); bits5:1 reserved; bits7:6 `mbz6` (`MBZ6_MASK` GENMASK(7,6), MBZ). |
| 4 | `sub_qid` | u16 | Sub-queue id the completion belongs to. |
| 6 | `sq_head_idx` | u16 | Current SQ head index (how far the device has consumed the SQ). |

### What the device must write / driver expectations

- **`phase`** (flags bit0): the device sets it to the CQ's current producer
  phase. The driver reads `flags`, extracts `phase` (shift 0), and treats the
  completion as not-yet-valid while `cdesc_phase != io_cq->phase`
  [ena_eth_com.h:229-239]. The driver's expected phase flips on CQ wrap
  [ena_eth_com.h:204-211]. So the device must produce completions with the
  phase that matches the CQ's current pass and flip phase on its own wrap.
- **`mbz6`** (flags bits7:6) must be zero. When the `ENA_ADMIN_CDESC_MBZ`
  capability is negotiated, the driver treats a non-zero `mbz6` as a corrupted
  descriptor and faults [ena_eth_com.h:241-247].
- **`req_id`** must echo the packet's req_id and be `< q_depth`; out-of-range
  is rejected as invalid [ena_eth_com.h:251-256].
- The driver reads `flags` first, checks phase, issues `dma_rmb()`, then reads
  `req_id` [ena_eth_com.h:229-251]; the device must therefore have the whole
  cdesc coherent before/at the moment it publishes the matching phase bit.
- One completion is posted per packet whose first descriptor had `comp_req`
  set; `sq_head_idx` advances the device's reported SQ consumption and
  `sub_qid` identifies the queue. The driver increments its CQ head once per
  consumed cdesc [ena_eth_com.h:258].

---

## Quick reference — discriminating descriptor types

- A TX SQ slot is a **meta descriptor** iff `len_ctrl` BIT(23) (`meta_desc`)
  is set; otherwise it is a **data descriptor** (where BIT(23) must be 0)
  [ena_eth_io_defs.h:33, 115; masks :286, :327].
- Transaction ordering on the SQ: `[meta?] data(first) [data ...] data(last)`,
  with `first`/`last` on whichever descriptors carry them per §1/§2.
