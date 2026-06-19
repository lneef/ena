# LLQ (Low Latency Queue) — Configuration and Operation

How the driver negotiates LLQ, where LLQ memory lives, and exactly how a Tx
packet is laid out and pushed into device memory. From the device point of
view this page describes (a) what the LLQ get/set-feature handshake must
carry, (b) what the CREATE_SQ response offsets mean, and (c) the byte-level
contract for a pushed LLQ entry that the device must parse.

Reference: AWS common ena_com (`kernel/linux/common/ena_com`) plus the DPDK
PMD (`userspace/dpdk/ena`). Paths below are relative to `amzn-drivers/`.
The common ena_com sources are mirrored verbatim under
`userspace/dpdk/ena/base/`; line numbers cited here are from the
`kernel/linux/common/ena_com` copy unless a path says otherwise.

CREATE_SQ command/response field layout and ordering live in
[queue-setup.md](queue-setup.md) §4; this page covers only the LLQ-specific
semantics.

---

## 1. What LLQ is (device contract in one sentence)

With placement policy `ENA_ADMIN_PLACEMENT_POLICY_DEV` (3) the Tx submission
queue's descriptor ring **and** the per-packet headers live in **device
memory** (a PCI BAR), not in host memory. The driver writes each SQ entry by
copying it across the bus into device memory ("pushing"), and the packet L2-L4
header is carried **inline** in that pushed entry instead of being DMA-read
from host memory. This removes one host->device DMA round trip per packet,
hence "low latency".
[kernel/linux/common/ena_com/ena_admin_defs.h:L96-L102] (explicit)
```c
enum ena_admin_placement_policy_type {
    ENA_ADMIN_PLACEMENT_POLICY_HOST = 1, /* descriptors+headers in OS memory */
    ENA_ADMIN_PLACEMENT_POLICY_DEV  = 3, /* descriptors+headers in device mem */
};
```

LLQ applies to **Tx SQs only** (`mem_queue_type` is the Tx placement type;
Rx and all CQs are always host memory). (explicit — `tx_mem_queue_type` set
in `ena_com_config_dev_mode` [ena_com.c:L3496,L3512])

---

## 2. LLQ feature negotiation (GET feature, ENA_ADMIN_LLQ = 4)

Feature id [ena_admin_defs.h:L58]: `ENA_ADMIN_LLQ = 4`.
Feature version enum [ena_admin_defs.h:L78-L83]:
`ENA_ADMIN_LLQ_FEATURE_VERSION_0_LEGACY = 0`, `..._VERSION_1 = 1`.

### GET response: `struct ena_admin_feature_llq_desc`
[kernel/linux/common/ena_com/ena_admin_defs.h:L672-L731] (explicit)
```c
struct ena_admin_feature_llq_desc {
    u32 max_llq_num;                       /* # LLQs the device supports;
                                              0 => no LLQ, host mode forced */
    u32 max_llq_depth;                     /* max LLQ ring depth (entries) */

    u16 header_location_ctrl_supported;    /* bitfield enum llq_header_location */
    u16 header_location_ctrl_enabled;      /* (driver echoes choice on SET) */

    u16 entry_size_ctrl_supported;         /* bitfield enum llq_ring_entry_size */
    u16 entry_size_ctrl_enabled;

    u16 desc_num_before_header_supported;  /* bitfield enum
                                              llq_num_descs_before_header */
    u16 desc_num_before_header_enabled;

    u16 descriptors_stride_ctrl_supported; /* bitfield enum llq_stride_ctrl */
    u16 descriptors_stride_ctrl_enabled;

    u8  feature_version;                   /* device feature version */
    u8  entry_size_recommended;            /* recommended entry size,
                                              enum llq_ring_entry_size; GET only */
    u16 max_wide_llq_depth;                /* max depth of "wide" llq, or 0 */

    struct ena_admin_accel_mode_req accel_mode; /* accelerated-LLQ requirements */
};
```

### Sub-enums (all bitfields where noted)

Header location [ena_admin_defs.h:L610-L615] (explicit):
```c
ENA_ADMIN_INLINE_HEADER = 1,  /* header carried in the descriptor list entry */
ENA_ADMIN_HEADER_RING   = 2,  /* header in a separate ring => 16B desc entry */
```

Ring entry size (size of one pushed LLQ entry, in bytes)
[ena_admin_defs.h:L617-L621] (explicit):
```c
ENA_ADMIN_LIST_ENTRY_SIZE_128B = 1,
ENA_ADMIN_LIST_ENTRY_SIZE_192B = 2,
ENA_ADMIN_LIST_ENTRY_SIZE_256B = 4,
```

Descriptors before header — max #descriptors that precede the header in the
**first** entry of a packet [ena_admin_defs.h:L623-L629] (explicit):
```c
ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_0/1/2/4/8 = 0/1/2/4/8
```

Stride control — how descriptors *after* the first entry are laid out
(inline-header mode only) [ena_admin_defs.h:L631-L640] (explicit):
```c
ENA_ADMIN_SINGLE_DESC_PER_ENTRY    = 1,
ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY = 2,
```
Header comment: "packet descriptor list entry always starts with one or more
descriptors, followed by a header. The rest of the descriptors are located in
the beginning of the subsequent entry. Stride refers to how the rest of the
descriptors are placed." (explicit)

### Accel mode (accelerated LLQ requirements)
[ena_admin_defs.h:L642-L670] (explicit)
```c
enum ena_admin_accel_mode_feat {
    ENA_ADMIN_DISABLE_META_CACHING = 0,
    ENA_ADMIN_LIMIT_TX_BURST       = 1,
};
struct ena_admin_accel_mode_get {            /* device -> driver (GET) */
    u16 supported_flags;                      /* bitfield of accel_mode_feat */
    u16 max_tx_burst_size;                    /* max burst between 2 doorbells,
                                                 in BYTES */
};
struct ena_admin_accel_mode_set {            /* driver -> device (SET) */
    u16 enabled_flags;                        /* bitfield of accel_mode_feat */
    u16 reserved;
};
struct ena_admin_accel_mode_req {            /* union, raw[2] */
    union { u32 raw[2]; struct ena_admin_accel_mode_get get;
            struct ena_admin_accel_mode_set set; } u;
};
```
Note `max_tx_burst_size` is in **bytes** and is converted to entries by the
driver (see §3). (explicit, comment L651)

---

## 3. Driver-side config: `ena_com_config_llq_info` → derived `llq_info`

`ena_com_config_dev_mode` is the entry point. If `max_llq_num == 0` it forces
host mode and returns; otherwise it calls `ena_com_config_llq_info`, then
computes `tx_max_header_size`.
[kernel/linux/common/ena_com/ena_com.c:L3488-L3515] (explicit)
```c
ena_dev->tx_max_header_size =
    llq_info->desc_list_entry_size -
    (llq_info->descs_num_before_header * sizeof(struct ena_eth_io_tx_desc));
/* must be > 0 else EINVAL; on success tx_mem_queue_type = PLACEMENT_POLICY_DEV */
```
`sizeof(struct ena_eth_io_tx_desc) == 16` (four u32:
[ena_eth_io_defs.h:L24-L98]). So **tx_max_header_size = entry_size −
16*descs_num_before_header**. With the DPDK defaults (descs_before_header=2):
- 128B entry => `128 − 32 = 96` bytes max inline header.
- 256B entry => `256 − 32 = 224` bytes max inline header ("large LLQ header").
(inferred from L3504-L3505 + tx_desc size; matches §7 accel numbers)

`ena_com_config_llq_info` negotiates each field against the device-supported
bitmaps and fills `struct ena_com_llq_info`
[kernel/linux/common/ena_com/ena_com.c:L644-L772],
[ena_com.h:L110-L119] (explicit):

| `llq_info` field            | derivation | source |
|-----------------------------|------------|--------|
| `header_location_ctrl`      | `llq_default_cfg->llq_header_location` if in `header_location_ctrl_supported`, else EINVAL | L655-L664 |
| `desc_stride_ctrl`          | default if supported, else fall back MULTIPLE→SINGLE; **0 unless INLINE_HEADER** | L666-L689 |
| `desc_list_entry_size_ctrl` | chosen entry-size enum | L691-L715 |
| `desc_list_entry_size`      | the byte value (128/192/256); must be multiple of 8 (because pushed via `__iowrite64_copy`, 64-bit granularity) | L694-L723 |
| `descs_per_entry`           | `desc_list_entry_size / sizeof(ena_eth_io_tx_desc)` if MULTIPLE stride, else 1 | L725-L729 |
| `descs_num_before_header`   | default if supported, else fall back 2→1→4→8 | L731-L754 |
| `disable_meta_caching`      | `accel.get.supported_flags & BIT(DISABLE_META_CACHING)` | L756-L760 |
| `max_entries_in_tx_burst`   | `accel.get.max_tx_burst_size / desc_list_entry_size` (bytes→entries), only if `LIMIT_TX_BURST` supported | L762-L765 |

Entry-size must be a multiple of 8 — enforced because the push uses
`__iowrite64_copy` (writes whole 64-bit words):
[ena_com.c:L716-L723] (explicit).

DPDK default LLQ config presented to `ena_com_config_llq_info`
[userspace/dpdk/ena/ena_ethdev.c:L2098-L2117] (explicit):
```c
llq_header_location      = ENA_ADMIN_INLINE_HEADER;
llq_stride_ctrl          = ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY;
llq_num_decs_before_header = ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2;
/* entry size: 256B (value 256) if use_large_llq_hdr && device supports 256B,
   else 128B (value 128) */
```
`use_large_llq_hdr` is true for policy LARGE, or for policy RECOMMENDED when
`entry_size_recommended == ENA_ADMIN_LIST_ENTRY_SIZE_256B`.
[userspace/dpdk/ena/ena_ethdev.c:L4213-L4224] (explicit)

---

## 4. SET feature (ENA_ADMIN_LLQ) — what the driver sends back

`ena_com_set_llq` issues `ENA_ADMIN_SET_FEATURE` with `feature_id =
ENA_ADMIN_LLQ`, echoing the *enabled* fields the driver chose into the same
`ena_admin_feature_llq_desc` (`u.llq` of the set-feature command), and turning
on both accel flags.
[kernel/linux/common/ena_com/ena_com.c:L609-L642] (explicit)
```c
cmd.aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
cmd.feat_common.feature_id      = ENA_ADMIN_LLQ;
cmd.u.llq.header_location_ctrl_enabled    = llq_info->header_location_ctrl;
cmd.u.llq.entry_size_ctrl_enabled         = llq_info->desc_list_entry_size_ctrl;
cmd.u.llq.desc_num_before_header_enabled  = llq_info->descs_num_before_header;
cmd.u.llq.descriptors_stride_ctrl_enabled = llq_info->desc_stride_ctrl;
cmd.u.llq.accel_mode.u.set.enabled_flags  =
    BIT(ENA_ADMIN_DISABLE_META_CACHING) | BIT(ENA_ADMIN_LIMIT_TX_BURST);
```
So the chosen entry size, stride control, descs-before-header, and header
location are carried in the **SET_FEATURE(ENA_ADMIN_LLQ)** admin command's LLQ
descriptor — **not** in `host_info`. The `host_info`
(`ENA_ADMIN_HOST_ATTR_CONFIG`) struct has **no LLQ fields**:
[ena_admin_defs.h:L1068-L1122] carries only OS/driver/feature/BDF info.
(explicit — no llq member in host_info)

This SET happens during `ena_com_config_llq_info` (it calls
`ena_com_set_llq` at the end, L767), i.e. before any CREATE_SQ. (explicit)

---

## 5. LLQ BAR and CREATE_SQ offsets

### Which BAR
LLQ device memory is the **MEM BAR**. In the DPDK PMD this is PCI BAR index
**2**:
[userspace/dpdk/ena/ena_ethdev.h:L23] (explicit) `#define ENA_MEM_BAR 2`
The PMD stores its mapped base in `adapter->dev_mem_base` and assigns it to
`ena_dev->mem_bar` only after LLQ is successfully configured and the device
actually exposes the mem bar:
[userspace/dpdk/ena/ena_ethdev.c:L2144-L2163] (explicit) — if
`dev_mem_base == NULL` the driver falls back to host placement even though the
device advertised LLQ.

### MEM BAR sizing and the per-SQ depth ceiling (emulated device)

The MEM BAR is carved into a fixed number of equal, per-SQ slices; each LLQ Tx
SQ's descriptor ring lives entirely within one slice. This bounds the ring
**depth** as a function of the negotiated entry size, because the whole ring
(`depth * desc_list_entry_size` bytes) must fit in one slice:

```
depth_max(entry_size) = slice_size / desc_list_entry_size
```

The emulated device uses a **128 KiB** slice and provides one slice per IO
queue. It exposes **128** LLQ Tx SQ slices (`max_llq_num = 128`), matching the
reference driver's `ENA_MAX_NUM_IO_QUEUES = 128`
[kernel/linux/common/ena_com/ena_com.h:L32], and advertises in
GET_FEATURE(LLQ) (§2):

| entry size | `max_*_llq_depth` advertised | derivation       |
|------------|------------------------------|------------------|
| 128 B      | `max_llq_depth = 1024`       | 128 KiB / 128 B  |
| 256 B      | `max_wide_llq_depth = 512`   | 128 KiB / 256 B  |

CREATE_SQ rejects any LLQ SQ whose `depth * entry_size` would exceed the slice
with `ENA_ADMIN_ILLEGAL_PARAMETER` (e.g. a 1024-deep ring of 256 B entries =
256 KiB > 128 KiB). [hw/ena.c `ENA_MAX_LLQ_SQ`/`ENA_LLQ_SLICE_SIZE`/
`ENA_LLQ_MAX_DEPTH`/`ENA_LLQ_MAX_WIDE_DEPTH`] (explicit, emulation).

The `max_llq_num = 128` is the device's queue ceiling, taken from the reference
driver's `ENA_MAX_NUM_IO_QUEUES`. (Real AWS instances impose a smaller
*per-interface* maximum that is instance-type dependent — 32 on 6th-generation
instances, more on newer ones, always a power of 2 —
[https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ena-queues.html]; the
emulated device does not model that per-instance cap and simply uses the
driver-supported 128.)

### CREATE_SQ for LLQ
`placement_policy = ENA_ADMIN_PLACEMENT_POLICY_DEV` is written into
`sq_caps_2`; `sq_ba` (SQ base address in OS memory) is left **zero** for LLQ
because the ring is in device memory.
[kernel/linux/common/ena_com/ena_com.c:L1320-L1343] (explicit; header comment
L314-L316 "should not be used for Low Latency queues").

### Response offsets (`ena_admin_acq_create_sq_resp_desc`)
[ena_admin_defs.h:L335-L354] (explicit):
- `sq_doorbell_offset` — offset into **REG BAR**; driver uses
  `reg_bar + sq_doorbell_offset` as the SQ tail doorbell.
- `llq_descriptors_offset` — "low latency queue ring base address as an offset
  to PCIe MMIO LLQ_MEM BAR".
- `llq_headers_offset` — "headers' memory as an offset to PCIe MMIO LLQ_MEM
  BAR".

Driver consumption [kernel/linux/common/ena_com/ena_com.c:L1355-L1364]
(explicit):
```c
io_sq->db_addr = reg_bar + sq_doorbell_offset;
if (mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV)
    io_sq->desc_addr.pbuf_dev_addr = mem_bar + llq_descriptors_offset;
```
**`llq_headers_offset` is not dereferenced by the driver.** With inline-header
mode the header is part of the descriptor-list entry written at
`pbuf_dev_addr`, so the separate header ring is unused. The push destination
for entry `tail` is:
`pbuf_dev_addr + (tail & (q_depth-1)) * desc_list_entry_size`
[kernel/linux/common/ena_com/ena_eth_com.c:L102-L103] (explicit).
(inferred for `llq_headers_offset`: field present, never read; only relevant
to HEADER_RING mode which this driver never selects)

---

## 6. LLQ entry layout (inline-header mode)

A pushed entry is `desc_list_entry_size` bytes. Within the **first** entry of a
packet:

```
 offset 0                      header_offset                 entry_size
 |--- N descriptors (16B each)---|---- inline header bytes ----|... pad ...|
   N = descs_num_before_header                header_offset =
                                              descs_num_before_header * 16
```
[kernel/linux/common/ena_com/ena_eth_com.c:L136-L165] (explicit)
```c
header_offset = llq_info->descs_num_before_header * io_sq->desc_entry_size;
/* fails if header_offset + header_len > desc_list_entry_size */
memcpy(bounce_buffer + header_offset, header_src, header_len);
```
So: the entry **always starts with `descs_num_before_header` descriptor slots**
(16 bytes each), immediately followed by up to `tx_max_header_size` bytes of
inline packet header. `tx_max_header_size = entry_size −
16*descs_num_before_header` (§3).

Descriptors that don't fit in the first entry spill to **subsequent** entries.
The stride control governs how many descriptors a non-first entry holds:
- `descs_per_entry = entry_size/16` when MULTIPLE stride (e.g. 8 for 128B, 16
  for 256B), else 1. [ena_com.c:L725-L729]
- First entry holds `descs_num_before_header` descriptors before its header;
  each following entry holds `descs_per_entry` descriptors.
  [ena_eth_com.c:L213 (reset to descs_num_before_header on close),
  L255 (reset to descs_per_entry after spilling a line)] (explicit)

Header-size edge cases:
- `header_len > tx_max_header_size` → `ena_com_prepare_tx` returns `-EINVAL`
  ("Header size is too large"). [ena_eth_com.c:L461-L466] (explicit)
- `header_offset + header_len > desc_list_entry_size` →
  `ena_com_write_header_to_bounce` returns `-EFAULT`. [ena_eth_com.c:L151-L155]
- LLQ mode with no push header provided → `-EINVAL` ("Push header wasn't
  provided in LLQ mode"). [ena_eth_com.c:L468-L472] (explicit)
- `header < tx_max_header_size` is fine; only `header_len` bytes are written,
  rest of the header area is left as zero-initialized bounce-buffer padding.
  The DPDK PMD computes `push_len = min(pkt_len, tx_max_header_size)` and sets
  `header_len = push_len`. [userspace/dpdk/ena/ena_ethdev.c:L3060-L3103]
  (explicit)

The first descriptor's `header_length` field (tx_desc bits 31:24) tells the
device how many inline bytes were written: comment "For Low Latency Queues,
this field indicates the number of bytes written to the headers' memory".
[ena_eth_io_defs.h:L82-L96] (explicit). The driver sets it from `header_len`:
[ena_eth_com.c:L505-L506] (explicit).

---

## 7. Accelerated LLQ (accel_mode) semantics

Two accel features, both enabled by the driver on SET (§4):

1. **DISABLE_META_CACHING (0)** — when the device supports it the driver sets
   `llq_info->disable_meta_caching = true` [ena_com.c:L758-L760]. Effect on the
   datapath: a meta descriptor is emitted for **every** packet that needs one
   rather than relying on the device caching the last meta descriptor. This
   forces an extra descriptor into the per-packet count when computing whether
   a doorbell is needed [ena_eth_com.h:L157-L159] (explicit).

2. **LIMIT_TX_BURST (1)** — `max_tx_burst_size` (bytes) limits how many entries
   may be written between two doorbells. Converted to entries:
   `max_entries_in_tx_burst = max_tx_burst_size / desc_list_entry_size`
   [ena_com.c:L762-L765] (explicit).

"Large LLQ entries": choosing the 256B entry size (vs 128B) yields
`tx_max_header_size = 224` (vs 96 with 128B), letting larger packet headers be
pushed inline. The driver opts into this via `use_large_llq_hdr` (§3).
(inferred from §3 arithmetic + L2107-L2116)

### Burst accounting (device-visible doorbell cadence)
`entries_in_tx_burst_left` is initialised to `max_entries_in_tx_burst` and
decremented per pushed entry; each Tx doorbell resets it.
[kernel/linux/common/ena_com/ena_com.c:L385-L387] (init),
[ena_eth_com.c:L105-L116] (decrement, error if 0),
[ena_eth_com.h:L184-L200] (`ena_com_write_tx_sq_doorbell` resets it) (explicit).
`ena_com_is_doorbell_needed` precomputes whether the next packet's entries
would exceed the remaining burst (forcing an early doorbell):
[ena_eth_com.h:L143-L172] (explicit) — it counts `num_bufs` (+1 for meta when
`disable_meta_caching` or meta changed), then
`num_entries_needed = 1 + ceil((num_descs − descs_num_before_header) /
descs_per_entry)` when descs overflow the first entry.

So the device contract: after the driver has pushed at most
`max_entries_in_tx_burst` entries it WILL ring the doorbell before pushing
more; the device must not assume more entries arrive without a doorbell.
(explicit, from the reset-on-doorbell + error-when-exhausted logic)

---

## 8. Bounce buffer mechanics (host-side staging → MMIO push)

The driver never writes descriptors directly into device memory byte-by-byte.
It builds each entry in a host-side **bounce buffer**, then copies the whole
entry to the BAR with one `__iowrite64_copy`.

Bounce buffer pool (`ena_com_io_bounce_buffer_control`)
[ena_com.h:L155-L167], allocated in `ena_com_init_io_sq` for DEV placement
[ena_com.c:L351-L388] (explicit):
- `buffer_size = desc_list_entry_size`; `buffers_num =
  ENA_COM_BOUNCE_BUFFER_CNTRL_CNT` (power of 2); ring-allocated round-robin via
  `ena_com_get_next_bounce_buffer` [ena_com.h:L1372-L1385].
- `llq_buf_ctrl.curr_bounce_buf` is the entry under construction;
  `descs_left_in_line` starts at `descs_num_before_header`.

Per-entry push — `ena_com_write_bounce_buffer_to_dev`
[kernel/linux/common/ena_com/ena_eth_com.c:L95-L134] (explicit):
```c
dst_offset = (io_sq->tail & (q_depth-1)) * desc_list_entry_size;
/* burst accounting (§7) ... */
wmb();                                   /* finish bounce-buffer writes first */
__iowrite64_copy(pbuf_dev_addr + dst_offset, bounce_buffer,
                 desc_list_entry_size / 8);   /* copy in 64-bit words */
io_sq->tail++;                            /* phase flips on wrap */
```
Key ordering facts the device side can rely on:
- The `wmb()` barrier guarantees every byte of the entry (descriptors +
  inline header) is in the bounce buffer **before** any of it is written to the
  BAR. [ena_eth_com.c:L118-L121] (explicit)
- The entry is copied **whole** (`desc_list_entry_size/8` 64-bit words). The
  device sees a complete entry, never a partial one, per push. (explicit, the
  single `__iowrite64_copy`)
- After the last entry of a packet is pushed, `ena_com_close_bounce_buffer`
  flushes the current bounce buffer (if any descriptors were placed), advances
  to the next bounce buffer, zeroes it, and resets `descs_left_in_line =
  descs_num_before_header`. [ena_eth_com.c:L187-L215] (explicit)
- Mid-packet spill: `ena_com_sq_update_llq_tail` pushes the current entry when
  `descs_left_in_line` reaches 0, grabs the next bounce buffer, zeroes it, and
  sets `descs_left_in_line = descs_per_entry`. [ena_eth_com.c:L234-L259]
  (explicit)

**Doorbell before buffer reuse**: the bounce buffer ring has a fixed count, and
`tail` (the device-memory slot) wraps every `q_depth` entries. The driver must
ring the Tx doorbell to let the device consume entries before it reuses the
same device-memory slot or runs out of burst credit (§7). The push itself does
not doorbell; the doorbell is a separate `writel(tail, db_addr)` from the Tx
burst path. [ena_eth_com.h:L184-L200] (explicit)

---

## 9. How the device should consume an LLQ entry

From the above, the device-side parse of a pushed Tx LLQ entry (inline-header
mode) is:

1. An entry arrives as a single contiguous write of `desc_list_entry_size`
   bytes at `llq_descriptors_offset + (tail_slot)*desc_list_entry_size` in the
   MEM BAR. Validity is established by the **phase bit** in each descriptor
   (`ENA_ETH_IO_TX_DESC_PHASE_MASK`), which the driver sets to `io_sq->phase`
   and flips on ring wrap. [ena_eth_com.c:L499,L547; L127-L131] (explicit)
2. The first packet entry begins with up to `descs_num_before_header`
   descriptors (16B each). The descriptor marked `first` (or the meta
   descriptor preceding it) starts the packet; `header_length` (bits 31:24 of
   `buff_addr_hi_hdr_sz`) gives the count of inline header bytes that follow at
   `offset = descs_num_before_header*16`. [ena_eth_com.c:L505-L506],
   [ena_eth_io_defs.h:L82-L96] (explicit)
3. The header bytes are taken from the **pushed entry** (device memory), not
   DMA-read from host — this is the whole point of `INLINE_HEADER`. The
   per-descriptor `buff_addr_lo/hi` still point to host memory for the
   **payload** segments (those are DMA-read normally). [ena_eth_com.c:L550-L559]
   (explicit — payload buffer addresses come from `ena_bufs->paddr`)
4. Descriptors that did not fit in the first entry are found at the start of
   the next entry/entries (`descs_per_entry` each under MULTIPLE stride). The
   last descriptor of the packet is flagged `last`. [ena_eth_com.c:L562-L563]
   (explicit)
5. The device must not expect more entries than the driver was allowed to push
   without a doorbell (`max_entries_in_tx_burst`, §7).

For HEADER_RING mode (header_location = 2) the header would live in a separate
ring at `llq_headers_offset` and the descriptor entry is forced to 16B — but
neither the DPDK PMD nor the kernel default selects this mode, so it is not
exercised by the reference driver. (inferred — only INLINE_HEADER is set as
default §3; HEADER_RING path unused)

---

## Open ambiguities

- **`llq_headers_offset`** (CREATE_SQ response) and **HEADER_RING** mode are
  defined but never used by the reference drivers (inline-header is always
  selected). A device may return a value the driver ignores. [resp struct
  L350-L353; never dereferenced in `ena_com_create_io_sq`]
- **`max_wide_llq_depth` / "wide" LLQ**: present in `ena_admin_feature_llq_desc`
  [L724-L725] and `entry_size_recommended` [L718-L722] influence sizing, but
  the exact "wide" semantics beyond depth selection are not derivable from
  ena_com alone. (ambiguous)
- **`disable_meta_caching` device behavior**: the driver enables it and emits
  meta per packet; whether the device *requires* it vs merely permits skipping
  the meta cache is not specified in ena_com — only the driver-side effect on
  descriptor count is visible. (ambiguous)
- **Exact `ENA_COM_BOUNCE_BUFFER_CNTRL_CNT`** value is host-side only and does
  not affect the device contract (device sees device-memory slots indexed by
  `tail & (q_depth-1)`). (explicit that it's host-only)
