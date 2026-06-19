# Stateless Offloads (checksum, TSO, RX hash)

How the device advertises stateless-offload capabilities, how the reference
driver requests per-packet TX offloads, and what the device must compute/report
on RX. "Stateless" here = per-packet checksum insertion/verification, TCP
segmentation (TSO), and RX hashing — as opposed to stateful (connection) offload.

Reference driver: AWS DPDK PMD (`userspace/dpdk/ena`). Paths below are relative
to `~/ena/amzn-drivers/`. Bit definitions are from
`userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h` and
`userspace/dpdk/ena/base/ena_defs/ena_eth_io_defs.h`.

Cross-references:
- TX descriptor / meta-descriptor wire layout and the meta-emit decision:
  [tx-descriptors.md](tx-descriptors.md) (§1 data desc `meta_ctrl`, §2 meta
  desc, §3 when a meta descriptor is emitted). **This page does not duplicate
  the bit tables; it documents discovery + RX semantics + the device contract.**
- RX completion `status` bitfield (`l3_csum_err`, `l4_csum_err`,
  `l4_csum_checked`, `ipv4_frag`, proto idx, hash): [rx-descriptors.md](rx-descriptors.md) §2a.
- GET_FEATURE / SET_FEATURE mechanics and the STATELESS_OFFLOAD_CONFIG / MTU
  payloads: [admin-queue.md](admin-queue.md) §6.
- RSS / hash-key / hash-input configuration is **out of scope** here and is a
  separate future page; this page only covers the single RX_HASH enable bit and
  the 32-bit hash the device writes per packet.

---

## 1. Capability discovery — GET_FEATURE STATELESS_OFFLOAD_CONFIG

Feature id `ENA_ADMIN_STATELESS_OFFLOAD_CONFIG = 11` [ena_admin_defs.h:61]
(explicit). The driver issues **GET_FEATURE only**; it never SET_FEATUREs this
feature. The only two references in `ena_com.c` are both GET
[ena_com.c:2349-2350, 2737-2738] (explicit):
- During init `ena_com_get_dev_attr_feat` GETs it and `memcpy`s the response
  into `get_feat_ctx->offload` [ena_com.c:2349-2355] (explicit).
- `ena_com_get_offload_settings` is a standalone GET wrapper
  [ena_com.c:2731-2747] (explicit).

So a device must answer GET_FEATURE(id=11) and never expects a SET for it. The
host enables/disables offloads purely per-packet via TX descriptor bits (§2)
and per-queue via RX descriptor posting; there is no admin "enable offload X"
handshake (inferred from the absence of any SET path [ena_com.c full file]).

### Response payload — `struct ena_admin_feature_offload_desc`

3 × u32 = 12 bytes, returned in `get_resp.u.offload`
[ena_admin_defs.h:818-841] (explicit):

| u32 | Field | Meaning |
|-----|-------|---------|
| 0 | `tx` | TX-side supported offloads (bitmask below) |
| 1 | `rx_supported` | RX-side supported offloads (bitmask below) |
| 2 | `rx_enabled` | RX-side currently-enabled offloads |

#### `tx` bits [ena_admin_defs.h:819-830, masks :1330-1344] (explicit)

| Bit | Macro | Meaning |
|-----|-------|---------|
| 0 | `TX_L3_CSUM_IPV4` (BIT(0)) | Insert IPv4 header checksum |
| 1 | `TX_L4_IPV4_CSUM_PART` (BIT(1)) | TCP/UDP-over-IPv4 csum, **partial**: driver must pre-seed the L4 csum field with the pseudo-header sum |
| 2 | `TX_L4_IPV4_CSUM_FULL` (BIT(2)) | TCP/UDP-over-IPv4 csum, **full**: device computes pseudo-header too |
| 3 | `TX_L4_IPV6_CSUM_PART` (BIT(3)) | TCP/UDP-over-IPv6 csum, partial |
| 4 | `TX_L4_IPV6_CSUM_FULL` (BIT(4)) | TCP/UDP-over-IPv6 csum, full |
| 5 | `TSO_IPV4` (BIT(5)) | TCP segmentation offload for IPv4 |
| 6 | `TSO_IPV6` (BIT(6)) | TCP segmentation offload for IPv6 |
| 7 | `TSO_ECN` (BIT(7)) | TSO with ECN |

The struct comment notes the PART variants require the checksum field to be
initialized with the pseudo-header checksum [ena_admin_defs.h:820-825]
(explicit). PART vs FULL maps directly onto the TX descriptor `l4_csum_partial`
bit (§2 / [tx-descriptors.md](tx-descriptors.md) §1) (inferred from matching
field names + comment text [ena_admin_defs.h:820-825], [ena_eth_io_defs.h:57]).

#### `rx_supported` / `rx_enabled` bits [ena_admin_defs.h:832-840, masks :1345-1351] (explicit)

| Bit | Macro | Meaning |
|-----|-------|---------|
| 0 | `RX_L3_CSUM_IPV4` (BIT(0)) | IPv4 header checksum verification |
| 1 | `RX_L4_IPV4_CSUM` (BIT(1)) | TCP/UDP-over-IPv4 checksum verification |
| 2 | `RX_L4_IPV6_CSUM` (BIT(2)) | TCP/UDP-over-IPv6 checksum verification |
| 3 | `RX_HASH` (BIT(3)) | Per-packet hash (RSS) calculation |

### How the driver consumes the response — `ena_set_offloads`

[ena_ethdev.c:2206-2247] (explicit). It reads **`tx` and `rx_supported`** and
folds them into internal flags in `struct ena_offloads {u32 tx_offloads;
u32 rx_offloads;}` [ena_ethdev.h:282-284]:

- `tx`: `TSO_IPV4`→`ENA_IPV4_TSO`; `TX_L3_CSUM_IPV4`→`ENA_L3_IPV4_CSUM`;
  `TX_L4_IPV4_CSUM_FULL`→`ENA_L4_IPV4_CSUM`;
  `TX_L4_IPV4_CSUM_PART`→`ENA_L4_IPV4_CSUM_PARTIAL`;
  `TX_L4_IPV6_CSUM_FULL`→`ENA_L4_IPV6_CSUM`;
  `TX_L4_IPV6_CSUM_PART`→`ENA_L4_IPV6_CSUM_PARTIAL` [ena_ethdev.c:2209-2229]
  (explicit).
- `rx_supported`: `RX_L3_CSUM_IPV4`→`ENA_L3_IPV4_CSUM`;
  `RX_L4_IPV4_CSUM`→`ENA_L4_IPV4_CSUM`; `RX_L4_IPV6_CSUM`→`ENA_L4_IPV6_CSUM`;
  `RX_HASH`→`ENA_RX_RSS_HASH` [ena_ethdev.c:2231-2246] (explicit).

Internal HW-cap flag values: `ENA_L3_IPV4_CSUM 0x1`, `ENA_L4_IPV4_CSUM 0x2`,
`ENA_L4_IPV4_CSUM_PARTIAL 0x4`, `ENA_L4_IPV6_CSUM 0x8`,
`ENA_L4_IPV6_CSUM_PARTIAL 0x10`, `ENA_IPV4_TSO 0x20`, `ENA_RX_RSS_HASH 0x40`
[ena_ethdev.c:199-214] (explicit).

**`rx_enabled` (u32 index 2) is never read by the DPDK driver** — no reference
exists in `userspace/dpdk/ena/` outside the struct definition itself (grep:
only `ena_admin_defs.h`) (explicit, by absence). The driver treats
`rx_supported` as authoritative. The device should populate `rx_supported`
meaningfully; `rx_enabled` is informational and not relied upon by this driver.

### Init dependency — GET(11) is gated and fatal if absent

The GET goes through `ena_com_get_feature` → `ena_com_get_feature_ex`, which
first calls `ena_com_check_supported_feature_id` [ena_com.c:1044-1047,
1020-1031] (explicit): if bit 11 is **clear** in the device's
`supported_features`, no admin command is issued and the call returns
`ENA_COM_UNSUPPORTED`. In `ena_com_get_dev_attr_feat` the offload read is
followed by a bare `if (rc) return rc;` [ena_com.c:2351-2352] (explicit) — so a
clear bit **or** an UNSUPPORTED admin completion to GET(11) aborts
`ena_com_get_dev_attr_feat` → `ena_device_init` → probe. Unlike the HW_HINTS and
LLQ reads just below it (which `memset` to zero on `ENA_COM_UNSUPPORTED`
[ena_com.c:2360-2378]), the offload read has **no** zero-fallback. So "feature
absent" is **not** equivalent to "no offloads": a device must advertise bit 11
and answer GET(11), never omit it.

### Minimal / zero-offload device advertisement

To advertise **no** stateless offloads (the QEMU emulation's target):
- set bit 11 (`STATELESS_OFFLOAD_CONFIG`) in DEVICE_ATTRIBUTES
  `supported_features`;
- answer GET(11) with an all-zero `ena_admin_feature_offload_desc`
  (`tx = rx_supported = rx_enabled = 0`). `ena_set_offloads` then leaves
  `tx_offloads = rx_offloads = 0`, so no checksum/TSO is offered and the clear
  `RX_HASH` bit means `RTE_ETH_RX_OFFLOAD_RSS_HASH` is not advertised
  [ena_ethdev.c:2206-2247, 2556-2557] (explicit);
- leave RSS_HASH_FUNCTION(10) / RSS_HASH_INPUT(18) unadvertised (not read at
  init), `capabilities = 0`, and LLQ `accel_mode = 0`.

**RX scatter is not an offload here.** The PMD forces `scattered_rx = 1`
("cannot be turned off in HW") and ORs `RTE_ETH_RX_OFFLOAD_SCATTER` into the port
offloads with no feature/capability gate [ena_ethdev.c:2492-2495, 2559]
(explicit) — it is inherent to multi-buffer Rx, never advertised via an admin
feature, and needs no device-side advertisement.

Note: a *full* DPDK probe additionally GET-reads RSS_INDIRECTION_TABLE_CONFIG(12)
inside `ena_com_rss_init` and aborts if absent [ena_ethdev.c:2416-2420;
ena_com.c:1178-1198] (explicit). That is RSS-table plumbing, not a stateless
offload, and is out of scope for offload advertisement.

---

## 2. TX-side offload encoding (device contract)

The full bit layout of `ena_eth_io_tx_desc.meta_ctrl` (l3_csum_en, l4_csum_en,
l3_proto_idx, l4_proto_idx, df, tso_en, l4_csum_partial, ethernet_fcs_dis) and
of `ena_eth_io_tx_meta_desc` (mss lo/hi, l3_hdr_len, l3_hdr_off,
l4_hdr_len_in_words, ext_valid, meta_store, etc.) is in
[tx-descriptors.md](tx-descriptors.md) §1–§2 with masks/shifts. This section
states only the **device behavior** each bit demands and how the driver decides
to set them.

### Per-packet driver decision — `ena_tx_mbuf_prepare`

[ena_ethdev.c:714-776] (explicit). The `ena_com_tx_ctx` offload inputs are
`tso_enable`, `l3_csum_enable`, `l4_csum_enable`, `l4_csum_partial`, `df`,
`l3_proto`, `l4_proto`, `meta_valid` [ena_eth_com.h:33-47] (explicit). The PMD
fills them only when the mbuf requests an offload AND the queue advertises it
(`MBUF_OFFLOADS` ∩ `QUEUE_OFFLOADS`) [ena_ethdev.c:721-722; masks :176-182]:

- TSO: set `tso_enable=true` and `ena_meta.l4_hdr_len = GET_L4_HDR_LEN(mbuf)`
  (TCP data-offset field in 32-bit words) [ena_ethdev.c:724-729, macro :30-32]
  (explicit).
- L3 csum: `l3_csum_enable=true` for IP-csum requests [ena_ethdev.c:732-734].
- L3 proto + DF: IPv6 ⇒ `l3_proto=IPV6, df=1`; IPv4 ⇒ `l3_proto=IPV4`, `df=1`
  only if the packet is L4-NONFRAG [ena_ethdev.c:736-748] (explicit).
- L4 csum: TCP ⇒ `l4_proto=TCP,l4_csum_enable=true`; UDP ⇒
  `l4_proto=UDP,l4_csum_enable=true`; else `l4_proto=UNKNOWN,l4_csum_enable=
  false` [ena_ethdev.c:750-763] (explicit).
- Always sets `ena_meta.{mss=tso_segsz, l3_hdr_len=l3_len,
  l3_hdr_offset=l2_len}` and `meta_valid=true` [ena_ethdev.c:765-769] (explicit).

These ctx fields are written into the **data** descriptor's `meta_ctrl` (offload
bits) and the **meta** descriptor (mss/header geometry) only when
`meta_valid` is true [ena_eth_com.c:553-571] (explicit). See
[tx-descriptors.md](tx-descriptors.md) §3 for the full meta-emit / caching rule.

### Device must-do per bit (inferred from field semantics + struct comments)

- `l3_csum_en` (data desc): compute and insert the IPv4 header checksum. L3
  geometry comes from the meta descriptor (`l3_hdr_off`, `l3_hdr_len`)
  [ena_eth_io_defs.h:135-143] (explicit struct; behavior inferred).
- `l4_csum_en` + `l4_csum_partial=0` (**full**): device computes the TCP/UDP
  checksum **including** the pseudo-header, derived from the packet's L3 header
  [ena_eth_io_defs.h:64-67 comment] (explicit comment).
- `l4_csum_en` + `l4_csum_partial=1` (**partial**): device adds the L4 payload
  sum to the pre-seeded csum field but does **not** recompute the pseudo-header;
  the host pre-seeded it [ena_eth_io_defs.h:64-67] (explicit comment), matching
  the `TX_L4_*_CSUM_PART` capability bits (§1).
- `tso_en`: segment the TCP stream into MSS-sized segments. MSS = 14-bit value
  split `mss_lo`(meta word2 31:22) + `mss_hi`(meta word0 19:16); L4 header length
  is `l4_hdr_len_in_words`; L4 starts at `l3_hdr_off + l3_hdr_len`
  [ena_eth_io_defs.h:120, 135-147] (explicit layout; segmentation behavior
  inferred). For TSO the device must also update per-segment IP length/ID,
  TCP seq, and (with `TSO_ECN`) ECN flags — inferred from TSO semantics and the
  `tso_ecn` capability bit [ena_admin_defs.h:828] (inferred).
- `df`: IPv4 Don't-Fragment flag to stamp on emitted segments; always 1 for
  IPv6 [ena_ethdev.c:736-748] (explicit driver intent).

### Driver-unused TX offload bits (device-relevant)

- **`l4_csum_partial` is declared and plumbed but never set by this PMD.**
  `ena_tx_mbuf_prepare` never assigns `ena_tx_ctx->l4_csum_partial`
  [ena_ethdev.c:714-776], so `ENA_FIELD_PREP(...l4_csum_partial...)` always
  writes 0 [ena_eth_com.c:568-570] (explicit, by absence). The DPDK driver
  therefore always requests **full** L4 checksum even though it records the
  device's PART capability in `tx_offloads` (§1). A device implementing only
  FULL would satisfy this driver; PART support is advertised but unexercised
  here. (explicit, by absence)
- **`ethernet_fcs_dis` (data desc bit 15) is never set** by either the com layer
  or the PMD (grep: no `ethernet_fcs`/`ETHERNET_FCS_DIS` write in
  `ena_eth_com.c` or `ena_ethdev.c`) (explicit, by absence). The driver always
  lets the controller append the 802.3 FCS.

---

## 3. Meta-descriptor emission & LLQ implications

Fully documented in [tx-descriptors.md](tx-descriptors.md) §3. Summary relevant
to offloads:
- A meta descriptor is required to carry TSO/csum geometry; the driver emits one
  whenever `meta_valid` and (the meta changed OR `disable_meta_caching`)
  [ena_eth_com.c:403-427] (explicit).
- `disable_meta_caching` is an LLQ accel-mode capability bit
  (`BIT(ENA_ADMIN_DISABLE_META_CACHING)` in the LLQ feature's
  `accel_mode.u.get.supported_flags`) — only meaningful for LLQ (device-memory)
  placement; host queues use `false` [ena_ethdev.c:2392-2398],
  [ena_com.c:763-765, 400-401] (explicit). With caching enabled the device must
  remember the last meta per SQ; with it disabled a fresh meta precedes every
  offloaded packet. See [tx-descriptors.md](tx-descriptors.md) §3 and
  [llq.md](llq.md).

---

## 4. RX-side offload consumption (device contract)

The device writes results into the RX completion descriptor `status` word; bit
layout and the driver's read/ignore list are in [rx-descriptors.md](rx-descriptors.md)
§2a/§5. Extraction happens in `ena_com_rx_set_flags`
[ena_eth_com.c:429-455] (explicit). Semantics the device must honor:

| `status` bit | Macro [ena_eth_io_defs.h] | Device meaning |
|--------------|---------------------------|----------------|
| 4:0 `l3_proto_idx` | `L3_PROTO_IDX_MASK` GENMASK(4,0) :360 | Parsed L3 proto (IPv4=8, IPv6=11; enum [ena_eth_io_defs.h:9-15]). Driver maps to ptype [ena_ethdev.c:660-672]. |
| 12:8 `l4_proto_idx` | `L4_PROTO_IDX_MASK` GENMASK(12,8) :366 | Parsed L4 proto (TCP=12, UDP=13; enum :17-22). |
| 13 `l3_csum_err` | `L3_CSUM_ERR_MASK` BIT(13) :367 | Set if IPv4 header csum failed **or** the controller did not validate it. Valid only when l3_proto=IPv4 [ena_eth_io_defs.h:205-208] (explicit). Driver sets `IP_CKSUM_BAD` on set, else `IP_CKSUM_GOOD` [ena_ethdev.c:663-668]. |
| 14 `l4_csum_err` | `L4_CSUM_ERR_MASK` BIT(14) :369 | Set if L4 csum failed or was not validated. Valid only when l4_proto=TCP/UDP, `ipv4_frag` clear, **and** `l4_csum_checked` set [ena_eth_io_defs.h:209-214] (explicit). |
| 15 `ipv4_frag` | `IPV4_FRAG_MASK` BIT(15) :371 | Packet is an IPv4 fragment [ena_eth_io_defs.h:215]. Driver (`frag`) suppresses L4-csum interpretation when set [ena_ethdev.c:689] (explicit). |
| 16 `l4_csum_checked` | `L4_CSUM_CHECKED_MASK` BIT(16) :373 | Device actually verified the L4 csum (result in `l4_csum_err`). When clear the L4 csum status is unknown [ena_eth_io_defs.h:216-218] (explicit). Driver emits `L4_CKSUM_UNKNOWN` when clear [ena_ethdev.c:698-700]. |
| 25 `l3_csum2` | `L3_CSUM2_MASK` BIT(25) :379 | "Second checksum engine result" [ena_eth_io_defs.h:222]. **Never read by the driver** (grep: only setter/getter in defs, no datapath use) (explicit, by absence) — device may leave it 0. |

Rule the device must follow (from the comments): only set `l4_csum_err`
meaningfully together with `l4_csum_checked=1`; the driver treats an L4 result as
trustworthy only when `l4_csum_checked` is set and the packet is non-fragmented
TCP/UDP [ena_ethdev.c:688-700] (explicit). For `l3_csum_err` there is no
"checked" companion bit: the driver assumes the IPv4 result is always meaningful
when l3_proto=IPv4 [ena_ethdev.c:663-668] (explicit), so a device that does not
validate the IPv4 csum must report `l3_csum_err=1` (per the comment's
"didn't validate ⇒ set" rule [ena_eth_io_defs.h:205-208]) (explicit comment).

### RX hash + RX_HASH enable bit

- The 32-bit Toeplitz/RSS hash the device computed goes in the cdesc `hash`
  field (cdesc base offset 0x08) [ena_eth_io_defs.h:238-239] (explicit), copied
  to `ena_rx_ctx->hash` unconditionally [ena_eth_com.c:450] (explicit).
- The driver only **exposes** the hash to the application (sets
  `RTE_MBUF_F_RX_RSS_HASH`, `mbuf->hash.rss`) when the queue has
  `RTE_ETH_RX_OFFLOAD_RSS_HASH` enabled — which is gated on the `RX_HASH`
  capability bit (§1) being present [ena_ethdev.c:702-705, 2244-2246] (explicit).
- Note: that hash read happens only inside the TCP/UDP-non-frag branch
  [ena_ethdev.c:689-705] (explicit); for non-L4 packets this PMD does not read
  the hash. The device should still populate `hash` whenever `RX_HASH` is
  advertised (inferred — the field is always valid per the cdesc layout; the
  driver's gating is a PMD-side choice).
- **RSS table / hash key / hash-input configuration is out of scope** and will
  be a separate page; only the `RX_HASH` enable bit and the per-packet `hash`
  field are covered here.

---

## 5. MTU and TSO size constraints

### MTU — SET_FEATURE MTU (feature id 14)

`ena_com_set_dev_mtu` issues SET_FEATURE with `u.mtu.mtu = mtu` (L2-excluded
MTU), guarded by capability check on `ENA_ADMIN_MTU`
[ena_com.c:2699-2729] (explicit). The PMD's `.mtu_set` op routes through it
[ena_ethdev.c:1284-1304, 339] (explicit). Payload and mechanics are in
[admin-queue.md](admin-queue.md) §6 ("MTU (14)"). `max_mtu` comes from the
device-attributes feature (`dev_attr.max_mtu`) [ena_ethdev.c:2406] and bounds
`dev_info->max_mtu`/`max_rx_pktlen` [ena_ethdev.c:2638-2641] (explicit).
`ENA_MIN_MTU` is the lower bound [ena_ethdev.c:2640] (explicit).

### TX header-size / push-buffer limit

- `tx_max_header_size` bounds the bytes the host may push as the header (LLQ) or
  declare as `header_length`. Host/Ext placement: it comes from the
  MAX_QUEUES_EXT feature `max_tx_header_size`
  [ena_com.c:2327-2334] (explicit), then clamped to ≤ `SZ_256` per SQ at queue
  create [ena_com.c:2196-2197] (explicit). LLQ placement: it is derived from the
  LLQ descriptor-entry size minus the descriptor list size
  [ena_com.c:3454-3457] (explicit).
- The device must never receive a `header_length` exceeding this value; see
  [tx-descriptors.md](tx-descriptors.md) §1 word3 and [queue-setup.md](queue-setup.md).

### TSO segmentation parameters

- The TSO MSS, L3/L4 header lengths and offsets are carried in the meta
  descriptor (§2, [tx-descriptors.md](tx-descriptors.md) §2). There is no
  separate admin "max TSO size" feature read in this driver path; the device's
  TSO cap is purely the `TSO_IPV4/IPV6/ECN` bits of STATELESS_OFFLOAD_CONFIG
  (§1) plus the generic per-packet descriptor count / `max_packet_tx_descs`
  geometry from MAX_QUEUES* [ena_ethdev.c:1166-1168] (explicit). (The DPDK PMD
  does not impose an explicit max-TSO-size beyond mbuf/descriptor limits —
  inferred, by absence of a TSO-size constant in `userspace/dpdk/ena/`.)

---

## 6. Things the device may implement but this driver never exercises

(explicit, by absence unless noted)

- **SET_FEATURE STATELESS_OFFLOAD_CONFIG** — never issued; the feature is
  GET-only [ena_com.c:2349-2350, 2737-2738].
- **`rx_enabled`** (offload desc u32 #2) — never read; only `rx_supported` is
  used [ena_ethdev.c:2231-2246].
- **`l4_csum_partial`** TX bit — always 0; the PMD only ever requests *full* L4
  csum [ena_ethdev.c:714-776], [ena_eth_com.c:568-570]. PART caps are advertised
  but unused.
- **`ethernet_fcs_dis`** TX bit — never set; controller always appends FCS.
- **`l3_csum2`** RX bit (second checksum engine) — never read.
- **`TSO_ECN`** — advertised in `tx` bit 7 but the PMD does not surface an
  ECN-specific path; `tso_enable` is a single boolean [ena_ethdev.c:724-729]
  (inferred: ECN handling, if any, is implicit in the device's TSO engine).
- **TX-side L3/L4 protos FCOE/ROCE** (enum values 21/22/23) — the driver only
  emits IPv4/IPv6 and TCP/UDP/UNKNOWN proto indices [ena_ethdev.c:736-763],
  [ena_eth_io_defs.h:9-22].
