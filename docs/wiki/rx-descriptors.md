# RX Datapath Descriptor Layouts

Scope: the on-the-wire (DMA) layouts of the RX submission descriptor and RX
completion descriptor, the bit fields, the enums in the completion status word,
and exactly which fields the reference DPDK driver writes/reads. This is what a
device emulation must produce/consume.

Cross-references (do not duplicate):
- Queue/ring sizing, phase-bit ownership model, doorbells: [queue-setup.md](queue-setup.md)
- CREATE_CQ / CREATE_SQ admin commands and their fields: [admin-queue.md](admin-queue.md)
- Device init / capability negotiation: [device-init.md](device-init.md)
- BAR0 registers and doorbells: [registers.md](registers.md)

All struct/field/mask citations are from the DPDK PMD ena_com tree unless noted.
Primary defs header:
`amzn-drivers/userspace/dpdk/ena/base/ena_defs/ena_eth_io_defs.h`.
The kernel
(`amzn-drivers/kernel/linux/common/ena_com/ena_eth_io_defs.h`) and FreeBSD copies
are byte-identical for these structs (verified same offsets/masks).

Claim tags: **[explicit]** = directly stated/encoded in cited source.
**[inferred]** = deduced from cited code, no single line states it.

---

## 1. RX submission descriptor — `ena_eth_io_rx_desc`

16 bytes, host-memory SQ (RX SQ is always a HOST placement ring — regular queue,
never LLQ). **[explicit]** The driver only ever uses
`get_sq_desc_regular_queue()` for RX
[ena_eth_com.c:711].

Struct definition [ena_eth_io_defs.h:166-195]:

| Offset | Width | Field | Notes |
|-------:|------:|-------|-------|
| 0x00 | u16 | `length` | Buffer length in bytes. **0 means 64 KiB.** [ena_eth_io_defs.h:167-168] |
| 0x02 | u8  | `reserved2` | MBZ [ena_eth_io_defs.h:170-171] |
| 0x03 | u8  | `ctrl` | bitfield, see below [ena_eth_io_defs.h:173-182] |
| 0x04 | u16 | `req_id` | Request ID echoed back in the completion [ena_eth_io_defs.h:184] |
| 0x06 | u16 | `reserved6` | MBZ [ena_eth_io_defs.h:186-187] |
| 0x08 | u32 | `buff_addr_lo` | Buffer phys addr [31:0] [ena_eth_io_defs.h:189] |
| 0x0C | u16 | `buff_addr_hi` | Buffer phys addr [47:32] [ena_eth_io_defs.h:191] |
| 0x0E | u16 | `reserved16_w3` | MBZ [ena_eth_io_defs.h:193-194] |

### `ctrl` byte (offset 0x03) bit fields
The masks below are defined relative to the `ctrl` **byte**, not a 32-bit word
[ena_eth_io_defs.h:350-357]:

| Bit | Mask macro | Meaning |
|----:|------------|---------|
| 0 | `ENA_ETH_IO_RX_DESC_PHASE_MASK` = BIT(0) | phase bit |
| 1 | (reserved1) | MBZ |
| 2 | `ENA_ETH_IO_RX_DESC_FIRST_MASK` = BIT(2) | first descriptor in transaction |
| 3 | `ENA_ETH_IO_RX_DESC_LAST_MASK` = BIT(3) | last descriptor in transaction |
| 4 | `ENA_ETH_IO_RX_DESC_COMP_REQ_MASK` = BIT(4) | request completion |
| 5 | (reserved5) | MBO (must-be-one per the comment) [ena_eth_io_defs.h:179] |
| 7:6 | (reserved6) | MBZ |

Note: the header comment marks bit 5 as MBO ("must be one"), but the driver does
**not** set it when posting a buffer (see below). **[explicit]** the driver's
write list omits bit 5 [ena_eth_com.c:717-722].

### What the driver writes when posting one RX buffer
`ena_com_add_single_rx_desc()` [ena_eth_com.c:698-735]:

1. `memset(desc, 0, 16)` — zeroes the whole descriptor first [ena_eth_com.c:713]. **[explicit]**
2. `desc->length = ena_buf->len` [ena_eth_com.c:715]. **[explicit]**
3. `desc->ctrl = FIRST | LAST | COMP_REQ | phase` [ena_eth_com.c:717-722]. **[explicit]**
   Each posted RX buffer is a self-contained single-descriptor transaction:
   FIRST and LAST are **both** always set, COMP_REQ always set, and the phase bit
   is taken from `io_sq->phase`.
4. `desc->req_id = req_id` [ena_eth_com.c:724]. **[explicit]**
5. `desc->buff_addr_lo = (u32)paddr` [ena_eth_com.c:730]. **[explicit]**
6. `desc->buff_addr_hi = paddr[dma_addr_bits-1:32]` [ena_eth_com.c:731-732]. **[explicit]**

Left zero by the driver (from the memset, never written): `reserved2`,
`ctrl` bit 1, bit 5 (despite "MBO" comment), bits 7:6, and `reserved6`,
`reserved16_w3`. **[explicit]** (memset + the explicit write list above).

Consequence for emulation: the device must validate/echo `req_id`, must read the
phase bit to detect ownership, and should treat every RX SQ descriptor as a
single buffer (the driver never chains RX SQ descriptors — it posts one buffer
per descriptor). **[inferred]** from ena_eth_com.c:698-735 (FIRST|LAST always set,
one buffer per call, tail advanced by one).

Tail/phase advance: posting advances `io_sq->tail` by 1 and flips
`io_sq->phase` on wrap [ena_eth_com.c:734, 261-270]. The device is told via the
RX SQ doorbell register (see [registers.md](registers.md)).

---

## 2. RX completion descriptor

### 2a. Base form — `ena_eth_io_rx_cdesc_base` (16 bytes / 4 words)

This is the **only** completion form the driver configures and reads (see §2c).
Definition [ena_eth_io_defs.h:200-246]:

| Offset | Width | Field | Notes |
|-------:|------:|-------|-------|
| 0x00 | u32 | `status` | bitfield, see below |
| 0x04 | u16 | `length` | bytes in this buffer/descriptor [ena_eth_io_defs.h:234] |
| 0x06 | u16 | `req_id` | echoes the SQ descriptor's `req_id` [ena_eth_io_defs.h:236] |
| 0x08 | u32 | `hash` | 32-bit RSS hash result [ena_eth_io_defs.h:238-239] |
| 0x0C | u16 | `sub_qid` | sub-queue id [ena_eth_io_defs.h:241] |
| 0x0E | u8  | `offset` | data start offset inside the buffer [ena_eth_io_defs.h:243] |
| 0x0F | u8  | `reserved` | [ena_eth_io_defs.h:245] |

#### `status` word (offset 0x00) bit fields
Masks/shifts [ena_eth_io_defs.h:359-386]:

| Bits | Field | Mask macro | Notes |
|-----:|-------|------------|-------|
| 4:0 | `l3_proto_idx` | `..._L3_PROTO_IDX_MASK` = GENMASK(4,0) | L3 proto enum (§3) |
| 6:5 | `src_vlan_cnt` | shift 5, GENMASK(6,5) | number of VLAN tags |
| 7 | `mbz7` | BIT(7) | MBZ; checked when CDESC_MBZ cap set |
| 12:8 | `l4_proto_idx` | shift 8, GENMASK(12,8) | L4 proto enum (§3) |
| 13 | `l3_csum_err` | BIT(13) | set = L3 csum error OR not validated; valid only if l3_proto_idx==IPv4 [ena_eth_io_defs.h:205-208] |
| 14 | `l4_csum_err` | BIT(14) | valid only if l4_proto_idx==TCP/UDP, ipv4_frag clear, AND l4_csum_checked set [ena_eth_io_defs.h:209-214] |
| 15 | `ipv4_frag` | BIT(15) | IPv4 fragment |
| 16 | `l4_csum_checked` | BIT(16) | L4 csum was verified (OK or error); if clear, csum status unknown [ena_eth_io_defs.h:216-218] |
| 17 | `mbz17` | BIT(17) | MBZ; checked when CDESC_MBZ cap set |
| 23:18 | `reserved18` | — | reserved |
| 24 | `phase` | BIT(24) | ownership phase bit |
| 25 | `l3_csum2` | BIT(25) | second checksum engine result |
| 26 | `first` | BIT(26) | first cdesc of packet |
| 27 | `last` | BIT(27) | last cdesc of packet |
| 29:28 | `reserved28` | — | reserved |
| 30 | `buffer` | BIT(30) | 0 = metadata descriptor, 1 = buffer descriptor used |
| 31 | `reserved31` | — | reserved |

Important: the completion `phase` is bit **24** of the 32-bit `status` word
[ena_eth_io_defs.h:377-378], whereas the *submission* descriptor phase is bit 0
of the `ctrl` **byte**. These are different positions; an emulator must not
confuse them. **[explicit]**

All ethernet parsing fields in `status` are valid only when `last==1` — stated
in the struct comment [ena_eth_io_defs.h:197-199]. **[explicit]**

### 2b. Extended form — `ena_eth_io_rx_cdesc_ext` (32 bytes / 8 words)

Definition [ena_eth_io_defs.h:248-261]: embeds the 16-byte base, then adds
`buff_addr_lo` (u32), `buff_addr_hi` (u16), `reserved16` (u16),
`reserved_w6` (u32), `reserved_w7` (u32). This lets the *device* carry the buffer
address in the completion. There are **no** generated get/set accessors for the
extended fields in the defs header (only base accessors exist,
[ena_eth_io_defs.h:792-933]). **[explicit]**

### 2c. Which entry size the driver configures, and ext usage

`ena_com_init_io_cq()` sets, for RX:
`cdesc_entry_size_in_bytes = sizeof(ena_eth_io_rx_cdesc_base)` = 16 bytes
[ena_com.c:423-427]. The comment literally says "Use the basic completion
descriptor for Rx". **[explicit]**

CREATE_CQ encodes this as words: `cq_caps_2 |= (cdesc_entry_size_in_bytes / 4) &
ENA_ADMIN_AQ_CREATE_CQ_CMD_CQ_ENTRY_SIZE_WORDS_MASK` → value **4** (4 words = 16
bytes) for RX [ena_com.c:1425-1426]. See CREATE_CQ in [admin-queue.md](admin-queue.md).

Conclusion: **the reference DPDK driver never uses the extended RX cdesc.** It
always negotiates a 4-word (16-byte) RX completion ring and reads only
`ena_eth_io_rx_cdesc_base`. **[explicit]** (init sets base size; ring is walked as
`rx_cdesc_base` everywhere, e.g. [ena_eth_com.c:8-33, 280-287]). An emulator
should therefore produce 16-byte base cdescs and put the buffer address only in
the SQ descriptor, never in the cdesc. **[inferred]** from the above.

---

## 3. Completion status enums

`l3_proto_idx` values — `enum ena_eth_io_l3_proto_index` [ena_eth_io_defs.h:9-15]:

| Name | Value |
|------|------:|
| `ENA_ETH_IO_L3_PROTO_UNKNOWN` | 0 |
| `ENA_ETH_IO_L3_PROTO_IPV4` | 8 |
| `ENA_ETH_IO_L3_PROTO_IPV6` | 11 |
| `ENA_ETH_IO_L3_PROTO_FCOE` | 21 |
| `ENA_ETH_IO_L3_PROTO_ROCE` | 22 |

`l4_proto_idx` values — `enum ena_eth_io_l4_proto_index` [ena_eth_io_defs.h:17-22]:

| Name | Value |
|------|------:|
| `ENA_ETH_IO_L4_PROTO_UNKNOWN` | 0 |
| `ENA_ETH_IO_L4_PROTO_TCP` | 12 |
| `ENA_ETH_IO_L4_PROTO_UDP` | 13 |
| `ENA_ETH_IO_L4_PROTO_ROUTEABLE_ROCE` | 23 |

Note `l3_proto_idx` is a 5-bit field (0..31) and `l4_proto_idx` is 5 bits, both
wide enough for the listed values. **[explicit]**

---

## 4. Multi-buffer (multi-descriptor) received packet

A received packet that spans N buffers produces N consecutive base cdescs.
Driver assembly logic in `ena_com_cdesc_rx_pkt_get()` [ena_eth_com.c:289-350]:

- The driver walks cdescs starting at `cur_rx_pkt_cdesc_start_idx`, consuming
  one cdesc per iteration, until it finds one with `last==1`
  [ena_eth_com.c:298-330]. **[explicit]**
- `first` (status bit 26) must be set **only** on the first cdesc: if `first` is
  set while `count != 0` the driver treats the completion as corrupt and faults
  [ena_eth_com.c:306-314]. So a valid multi-cdesc packet has FIRST on cdesc[0]
  only and LAST on cdesc[N-1] only. **[explicit]**
- If the CDESC_MBZ capability (`ENA_ADMIN_CDESC_MBZ` = 4,
  [ena_admin_defs.h:88]) is advertised, any set bit in `mbz7` (bit 7) or `mbz17`
  (bit 17) of any cdesc faults the packet [ena_eth_com.c:316-323]. **[explicit]**
- Length semantics: **per-cdesc length is per-buffer**, not cumulative. Each
  buffer's bytes are taken from that cdesc's own `length`:
  `ena_buf[i].len = cdesc->length` for every i [ena_eth_com.c:660-661]. **[explicit]**
- req_id semantics: **each cdesc carries its own `req_id`** matching the
  SQ-posted buffer it consumed: `ena_buf[i].req_id = cdesc->req_id`
  [ena_eth_com.c:662]. Each req_id is range-checked `< q_depth`; out of range →
  `ENA_COM_EIO` [ena_eth_com.c:663-664]. **[explicit]**
- Parsing/flags (l3/l4 proto, csum, hash, frag) are taken from the **last**
  cdesc only: `ena_com_rx_set_flags(ena_rx_ctx, cdesc)` is called after the loop
  with `cdesc` pointing at the last descriptor [ena_eth_com.c:680-681, 429-455].
  This matches the "valid only when last=1" rule from §2a. **[explicit]**
- `pkt_offset` is taken from the **first** cdesc's `offset`
  [ena_eth_com.c:657-658] and is used downstream as the data start offset in the
  first buffer [ena_ethdev.c:2835-2839]. **[explicit]**

After assembling the packet the driver advances the SQ head by the number of
cdescs: `io_sq->next_to_comp += nb_hw_desc` [ena_eth_com.c:674]. **[explicit]**

Phase handling while walking: `ena_com_get_next_rx_cdesc()` returns NULL when the
cdesc phase != expected `io_cq->phase`; on consuming each cdesc the head is
advanced via `ena_com_cq_inc_head()` and the CQ phase flips on wrap
[ena_eth_com.c:8-33, 325]. See phase model in [queue-setup.md](queue-setup.md).
**[explicit]**

---

## 5. cdesc fields the driver reads vs. ignores

This matters for emulation correctness: fields flagged "ignored" are device-written
but the reference driver never consumes them, so an emulator may set them to a
benign/zero value without breaking this driver (but should still honor MBZ rules).

Fields the driver **reads**:

| Field | Where | Use |
|-------|-------|-----|
| `status.phase` (bit 24) | [ena_eth_com.c:20-22] | ownership detection |
| `status.first` (bit 26) | [ena_eth_com.c:306-308] | corruption check |
| `status.last` (bit 27) | [ena_eth_com.c:327-329] | packet boundary |
| `status.mbz7` / `status.mbz17` | [ena_eth_com.c:316-318] | corruption check (only when CDESC_MBZ cap set) |
| `status.l3_proto_idx` (4:0) | [ena_eth_com.c:432-433] | rx ctx |
| `status.l4_proto_idx` (12:8) | [ena_eth_com.c:434-437] | rx ctx |
| `status.l3_csum_err` (13) | [ena_eth_com.c:438-441] | rx ctx |
| `status.l4_csum_err` (14) | [ena_eth_com.c:442-445] | rx ctx |
| `status.l4_csum_checked` (16) | [ena_eth_com.c:446-449] | rx ctx |
| `status.ipv4_frag` (15) | [ena_eth_com.c:451-454] | rx ctx |
| `length` | [ena_eth_com.c:661] | per-buffer length |
| `req_id` | [ena_eth_com.c:662-664] | buffer lookup + validation |
| `hash` | [ena_eth_com.c:450] | RSS hash to mbuf |
| `offset` (first cdesc) | [ena_eth_com.c:658] | data start offset |

Fields the device writes but the driver **ignores** in the datapath:

| Field | Status |
|-------|--------|
| `status.src_vlan_cnt` (6:5) | **ignored** — never read in ena_eth_com.c rx path. **[inferred]** (absent from ena_com_rx_set_flags and ena_com_rx_pkt; see [ena_eth_com.c:429-696]) |
| `status.l3_csum2` (bit 25) | **ignored** — never read. **[inferred]** (same regions) |
| `status.buffer` (bit 30) | **ignored** by datapath — driver does not branch on metadata-vs-buffer here. **[inferred]** (not referenced in [ena_eth_com.c:289-696]) |
| `sub_qid` (0x0C) | **ignored** — never read on the RX cdesc path. **[inferred]** (no read in ena_eth_com.c rx functions) |
| `reserved` (0x0F), `reserved18`, `reserved28`, `reserved31` | ignored (reserved) |

Caveat on "ignored": these claims are negative (absence of a read). They are
**[inferred]** from the fact that the only RX-cdesc consumers in the ena_com
layer are `ena_com_cdesc_rx_pkt_get`, `ena_com_rx_pkt`, and
`ena_com_rx_set_flags` ([ena_eth_com.c:289-350, 429-455, 622-696]) and none of
them touch those fields. A higher driver layer could in principle read them, but
the DPDK `ena_ethdev.c` RX loop only consumes the `ena_rx_ctx` produced by the
ena_com layer (proto/csum/hash/frag/pkt_offset/descs), confirming src_vlan_cnt,
l3_csum2, buffer-bit and sub_qid are unused. **[inferred]**

Emulation guidance (from the above, [inferred]): the device must set
`status.phase`, `first`, `last`, `length`, `req_id`, and (when last) the
proto/csum/hash/frag fields; it must keep `mbz7`/`mbz17` clear if it ever
advertises CDESC_MBZ; it should set `offset` on the first cdesc; it may safely
leave `src_vlan_cnt`, `l3_csum2`, `sub_qid`, and the buffer bit zero for this
driver.
