# TX Datapath Flow Semantics

Device-side view of the ENA transmit datapath: what the device must do from
the moment the driver rings the SQ doorbell until it posts a TX completion.
Citations are to the AWS DPDK reference (`userspace/dpdk/ena/`). Paths are
relative to `amzn-drivers/`.

Related pages: [queue-setup.md](queue-setup.md) (SQ/CQ creation, doorbell
offset, entry sizes), [llq.md](llq.md) (LLQ bounce buffers — if present),
[registers.md](registers.md), [admin-queue.md](admin-queue.md).

Two SQ placement policies exist and the device behaves differently for each:
- `ENA_ADMIN_PLACEMENT_POLICY_HOST` ("host"/regular queue): descriptors live in
  host memory; the device DMA-reads them from the SQ ring base on doorbell.
- `ENA_ADMIN_PLACEMENT_POLICY_DEV` (LLQ, Low Latency Queue): descriptors +
  push header are written by the driver directly into device memory (BAR) via
  64-byte writes; the doorbell just tells the device the new tail.

---

## 1. Submission flow — how a packet is laid out in the SQ

The driver builds, in SQ tail order, the following descriptor sequence per
packet (`ena_com_prepare_tx`, `userspace/dpdk/ena/base/ena_eth_com.c:461-620`):

1. **Optional meta descriptor (first).** Emitted only when offload/TSO metadata
   changed vs the cached copy, or when meta caching is disabled
   (`ena_com_create_and_store_tx_meta_desc`, `ena_eth_com.c:403-427`;
   `ena_com_meta_desc_changed`, `ena_eth_com.h:114-123`). Built in
   `ena_com_create_meta` (`ena_eth_com.c:352-401`): it sets
   `META_DESC`, `EXT_VALID`, `ETH_META_TYPE`, `FIRST`, `META_STORE`, the current
   `io_sq->phase`, MSS (lo bits 0-9 in `word2`, hi bits 10-13 in `len_ctrl`),
   and L3/L4 header len/offset in `word2`. The meta descriptor carries the
   `FIRST` flag, so when a meta desc is present the data first-descriptor does
   **not** set `FIRST` (`ena_eth_com.c:525-527`). `classify: explicit`.

2. **First data descriptor.** Obtained via `get_sq_desc` (`ena_eth_com.c:520`).
   Fields set (`ena_eth_com.c:523-571`):
   - `HEADER_LENGTH` (bits 31:24 of `buff_addr_hi_hdr_sz`) = `header_len`. For
     LLQ this is the number of bytes the driver pushed into device memory; for
     host mode, 0 unless the L4 header is being split out
     (`ena_eth_io_defs.h:82-97`).
   - `PHASE` = `io_sq->phase`, `COMP_REQ` (request a completion — only valid on
     the first desc), `req_id` split into REQ_ID_LO (bits 31:22 of `meta_ctrl`)
     and REQ_ID_HI (bits 21:16 of `len_ctrl`), `DF`, and (when `meta_valid`)
     the inline offload bits: `TSO_EN`, `L3_PROTO_IDX`, `L4_PROTO_IDX`,
     `L3_CSUM_EN`, `L4_CSUM_EN`, `L4_CSUM_PARTIAL`.
   - `FIRST` only if no meta desc preceded it.
   The first data buffer (`ena_bufs[0]`) is then written into this same
   descriptor in the loop body below (the header desc and first buffer share one
   descriptor — `ena_eth_com.c:573-604`, comment at L574). `classify: explicit`.

3. **Remaining data descriptors.** For each subsequent buffer the loop advances
   the SQ tail, zeroes a fresh descriptor, sets only `PHASE`, then fills
   `LENGTH` (bits 15:0 of `len_ctrl`, `ena_eth_io_defs.h:282`), `buff_addr_lo`,
   and `ADDR_HI` (bits 15:0 of `buff_addr_hi_hdr_sz`)
   (`ena_eth_com.c:573-604`). Note `LENGTH == 0` means a 64 KB buffer is not
   implied here for TX (that note is RX-specific); TX length is the literal byte
   count excluding the push header and the 4-byte 802.3 FCS
   (`ena_eth_io_defs.h:24-46`). `classify: explicit`.

4. **Last descriptor.** After the loop the `LAST` flag is OR-ed into the most
   recently written descriptor's `len_ctrl` (`ena_eth_com.c:607`).
   `classify: explicit`.

**Packet boundary determination (device contract):** a TX packet spans from the
descriptor with `FIRST` set (the meta desc if present, else the first data desc)
through the descriptor with `LAST` set. The device must accumulate buffers
across all descriptors of one packet, in tail order, and treat `LAST` as the
end-of-packet marker. The total host-memory payload is the concatenation of each
buffer `(buff_addr_hi:buff_addr_lo, LENGTH)`; for LLQ the push header (the first
`header_len` bytes) precedes those buffers.
Evidence: FIRST/LAST set sites `ena_eth_com.c:385,527,607`; flag definitions
`ena_eth_io_defs.h:289-292`. `classify: explicit`.

`ena_com_prepare_tx` returns `nb_hw_desc = io_sq->tail - start_tail`
(`ena_eth_com.c:618`) — the number of SQ entries consumed (host mode) or LLQ
entries advanced; the driver stores this in `tx_info->tx_descs`
(`ena_ethdev.c:3186`) and later credits it back on completion.

### Push header construction (driver side, for context)
`ena_tx_map_mbuf` (`ena_ethdev.c:3053-3124`): for LLQ the push header is
`min(pkt_len, tx_max_header_size)` bytes taken from the head of the mbuf (or
copied into `push_buf_intermediate_buf` if it spans segments); the bytes already
in the push header are skipped when building the buffer list (the `delta` /
`push_len` arithmetic). For host mode `push_header = NULL`, `header_len = 0`.
`tx_max_header_size` is derived at LLQ config time as
`desc_list_entry_size - descs_num_before_header * sizeof(tx_desc)`
(`ena_com.c:3454-3455`). `classify: explicit`.

---

## 2. Doorbell semantics

`ena_com_write_sq_doorbell` (`ena_eth_com.h:162-181`):
- Writes the **current `io_sq->tail`** (a 16-bit ring index) to `io_sq->db_addr`
  via `ENA_REG_WRITE32`. The value is the new producer index; the device must
  process all SQ entries from its last consumed head up to (not including) this
  tail.
- `db_addr = reg_bar + cmd_completion.sq_doorbell_offset` — the doorbell
  register offset is returned by the device in the CREATE_SQ admin completion
  (`ena_com.c:1316-1317`). See [queue-setup.md](queue-setup.md).
- For LLQ with a tx-burst limit it resets `entries_in_tx_burst_left =
  max_entries_in_tx_burst` after the write (`ena_eth_com.h:173-178`).
`classify: explicit`.

**When the doorbell is rung (driver policy the device relies on):**
- After a burst: `eth_ena_xmit_pkts` enqueues packets, setting
  `pkts_without_db = true` per packet, then rings one doorbell at the end of the
  burst (`ena_ethdev.c:3292-3306`). So the device may receive many descriptors
  per doorbell — it must read the whole range up to `tail`.
- Mid-burst for LLQ: `ena_com_is_doorbell_needed` (`ena_eth_com.h:131-160`)
  returns true when the next packet's required LLQ entries
  (`1 + DIV_ROUND_UP(descs_after_first_entry, descs_per_entry)`, plus one for a
  meta desc if needed) exceed `entries_in_tx_burst_left`. In that case
  `ena_xmit_mbuf` rings the doorbell *before* writing the new packet
  (`ena_ethdev.c:3165-3173`). `classify: explicit`.

**LLQ bounce-buffer / doorbell ordering:** in LLQ mode each completed 64-byte
line is copied to device memory by `ena_com_write_bounce_buffer_to_dev`
(`ena_eth_com.c:96-137`), which executes a `wmb()` before
`ENA_MEMCPY_TO_DEVICE_64` and decrements `entries_in_tx_burst_left`, returning
`ENA_COM_NO_SPACE` if the burst budget is exhausted (`ena_eth_com.c:107-118`).
The doorbell carries the tail; the device must not assume an LLQ entry is valid
until both its descriptors are present in device memory and the doorbell tail
covers it. `classify: explicit`.

`max_entries_in_tx_burst` comes from the LLQ feature negotiation
(`ena_com.c:768`, stored in `llq_info`); `is_llq_max_tx_burst_exists` gates all
of the above on LLQ + a non-zero burst limit (`ena_eth_com.h:125-129`).

---

## 3. Host SQ vs LLQ SQ consumption (head/tail/phase)

**Host mode** (`get_sq_desc_regular_queue`, `ena_eth_com.c:84-94`): the driver
writes descriptors at `desc_addr.virt_addr + (tail & (q_depth-1)) *
desc_entry_size`. The device DMA-reads descriptors from the SQ ring base (the
address given at CREATE_SQ) at the masked head index. Tail advances in
`ena_com_sq_update_reqular_queue_tail` (`ena_eth_com.c:261-270`): `tail++`, and
on wrap (`(tail & (q_depth-1)) == 0`) the producer **phase bit flips**
(`io_sq->phase ^= 1`). The device tracks its own consumer head and uses the
phase bit in each descriptor to know whether an entry has been (re)written for
the current lap. Initial state: `tail=0`, `phase=1`, `next_to_comp=0`
(`ena_com.c:408-410`). `classify: explicit`.

**LLQ mode** (`ena_com_sq_update_llq_tail`, `ena_eth_com.c:230-259`): the SQ
tail and phase advance only when a 64-byte device-memory line is closed
(`ena_com_write_bounce_buffer_to_dev`, `ena_eth_com.c:130-134` does
`tail++` and the wrap phase flip). Multiple descriptors can share one LLQ entry
(`descs_per_entry`, controlled by `desc_stride_ctrl`), so SQ entries here count
LLQ lines, not individual descriptors. `classify: explicit`.

Free-space accounting the driver enforces before submitting
(`ena_com_sq_have_enough_space`, `ena_eth_com.h:96-112`): host mode needs
`free >= num_bufs+1`; LLQ uses an approximate
`free > num_bufs/descs_per_entry + 2`. The device should never see more
in-flight entries than `q_depth - 1` (`ena_com_free_q_entries`,
`ena_eth_com.h:90-93`). `classify: explicit`.

---

## 4. Completion path

The device posts one **TX completion descriptor (cdesc)** per completed packet
into the TX CQ. Layout `struct ena_eth_io_tx_cdesc`
(`ena_eth_io_defs.h:148-164`), 8 bytes:
- `req_id` (u16, bytes 0-1) — echoes the `req_id` from the packet's first TX
  descriptor.
- `status` (u8, byte 2).
- `flags` (u8, byte 3): bit 0 = `PHASE`, bits 7:6 = `MBZ6` (must be zero)
  (`ena_eth_io_defs.h:346-348`).
- `sub_qid` (u16, bytes 4-5).
- `sq_head_idx` (u16, bytes 6-7) — the SQ head index the device has consumed up
  to.

CQ cdesc entry size is `sizeof(ena_eth_io_tx_cdesc)` (8 bytes), set at CQ init
(`ena_com.c:424-427`). The TX CQ is a phase-based ring; CQ starts `head=0`,
`phase=1` (`ena_com.c:453-454`).

**Driver consumption** (`ena_com_tx_comp_req_id_get`, `ena_eth_com.h:213-261`):
1. Read the cdesc at `cdesc_addr.virt_addr + (head & (q_depth-1)) *
   entry_size`.
2. Read `flags`; compare cdesc `PHASE` bit to expected `io_cq->phase`. If they
   differ the completion is not yet posted -> `ENA_COM_TRY_AGAIN`
   (`ena_eth_com.h:229-239`). **This is the core device contract: the phase bit
   is the validity signal.**
3. Validate `MBZ6` zero (if `ENA_ADMIN_CDESC_MBZ` capability)
   (`ena_eth_com.h:241-247`).
4. `dma_rmb()` then read `req_id`; reject if `req_id >= q_depth`
   (`ena_eth_com.h:249-256`).
5. `ena_com_cq_inc_head` (`ena_eth_com.h:204-211`): `head++`, flip
   `io_cq->phase` on wrap.

**Driver-level req_id validation** (`validate_tx_req_id`, `ena_ethdev.c:778-799`):
`req_id < ring_size` and `tx_buffer_info[req_id].mbuf != NULL`. A bad req_id
triggers a device reset (`ENA_REGS_RESET_INV_TX_REQ_ID`). The device must only
emit `req_id` values it received in TX first-descriptors for in-flight packets.

**SQ head credit:** `ena_tx_cleanup` (`ena_ethdev.c:3198-3270`) sums
`tx_info->tx_descs` over the completed packets and calls `ena_com_comp_ack`
(`ena_eth_com.h:199-202`), which advances `io_sq->next_to_comp` by that many SQ
entries — freeing SQ space. The cdesc `sq_head_idx` field conveys the device's
own SQ consumer position. `classify: explicit`.

**Interrupt vs polling:** the TX datapath is poll-driven. `eth_ena_xmit_pkts`
opportunistically calls `ena_tx_cleanup` when free descriptors drop below
`tx_free_thresh` (`ena_ethdev.c:3288-3290`); cleanup can also be invoked via the
`tx_done_cleanup` op (`ena_ethdev.c:350`). There is **no CQ head doorbell** in
the TX path — the driver never writes a consumer index back for the CQ; it only
advances its local `head`/`phase`. The only TX-side MMIO writes are the SQ
doorbell and, where used, interrupt unmask via `ena_com_unmask_intr`
(writes `unmask_reg`, `ena_eth_com.h:79-83`; driver wrapper
`ena_ethdev.c:3880-3890`). `classify: explicit`.

---

## 5. Ordering / atomicity the driver relies on

The device MUST honor these for correctness:
- **Phase bit written last / visible after payload.** The driver reads the
  cdesc `flags` (phase) first, and only after the phase matches does it issue
  `dma_rmb()` and read `req_id` (`ena_eth_com.h:229-251`). For this to be safe
  the device must ensure all other cdesc fields are visible to the host before
  (or atomically with) the phase bit flip — i.e. publish the completion body,
  then the phase. `classify: inferred` from the read-ordering and barrier in
  `ena_com_tx_comp_req_id_get` (`ena_eth_com.h:229-258`).
- **SQ descriptor contents before doorbell.** Host mode relies on the driver's
  writes landing before the doorbell MMIO; LLQ relies on `wmb()` before the
  64-byte device-memory copy (`ena_eth_com.c:120-128`). The device must treat
  the doorbell tail as the publish point for all preceding SQ entries.
- **CQ head phase wrap** is purely host-local bookkeeping
  (`ena_com_cq_inc_head`, `ena_eth_com.h:204-211`); the device tracks its own
  CQ producer phase independently and they stay in lockstep only because both
  start at phase 1 and flip every `q_depth` entries.
`classify: explicit` for the barrier placement; `classify: inferred` for the
device-side publish-order obligation.

---

## 6. What the device must NOT do

- **Must not post a completion before its body is coherent.** Flipping the
  phase bit while `req_id`/`status` are stale would let the driver read garbage
  (see §5). `classify: inferred`.
- **Must not set the `MBZ6` bits** (`flags[7:6]`) in TX cdescs, nor the various
  MBZ descriptor bits, when the `ENA_ADMIN_CDESC_MBZ` capability is advertised —
  the driver treats a non-zero MBZ as corruption and faults
  (`ena_eth_com.h:241-247`). `classify: explicit`.
- **Must not emit a `req_id >= q_depth`**, nor a `req_id` not currently
  in-flight; both fault the driver and trigger reset
  (`ena_eth_com.h:252-256`, `ena_ethdev.c:778-799`). `classify: explicit`.
- **Out-of-order completions are permitted by design.** Completions are matched
  to packets by `req_id` (carried in the first descriptor, echoed in the cdesc),
  not by position — the driver does not assume the CQ order equals the SQ
  submission order. The `req_id` indirection (`empty_tx_reqs` free-list at
  `ena_ethdev.c:3148`, `tx_buffer_info[req_id]` lookup at `:3229`) exists
  precisely so the device may complete packets in an order different from
  submission. The one ordering the driver DOES require is that the **CQ phase
  bit advances sequentially** per CQ slot (one cdesc per slot, in CQ index
  order) so the phase scan terminates correctly. `classify: inferred` (the
  req_id mechanism strongly implies OOO tolerance; no code path assumes FIFO
  completion order).

---

## Quick reference — TX descriptor field map

`struct ena_eth_io_tx_desc` (16 bytes, `ena_eth_io_defs.h:24-98`):
| word | field | bits | meaning |
|------|-------|------|---------|
| len_ctrl | length | 15:0 | buffer length (excl. push hdr and FCS) |
| len_ctrl | req_id_hi | 21:16 | req_id[15:10] |
| len_ctrl | meta_desc | 23 | MBZ for data desc |
| len_ctrl | phase | 24 | producer phase |
| len_ctrl | first | 26 | first desc of packet |
| len_ctrl | last | 27 | last desc of packet |
| len_ctrl | comp_req | 28 | request completion (first desc only) |
| meta_ctrl | l3_proto_idx | 3:0 | L3 protocol |
| meta_ctrl | DF | 4 | IPv4 don't-fragment |
| meta_ctrl | tso_en | 7 | TSO enable |
| meta_ctrl | l4_proto_idx | 12:8 | L4 protocol |
| meta_ctrl | l3_csum_en | 13 | IPv4 hdr csum |
| meta_ctrl | l4_csum_en | 14 | L4 csum |
| meta_ctrl | ethernet_fcs_dis | 15 | suppress 802.3 FCS |
| meta_ctrl | l4_csum_partial | 17 | partial L4 csum |
| meta_ctrl | req_id_lo | 31:22 | req_id[9:0] |
| buff_addr_lo | - | 31:0 | buffer addr[31:0] |
| buff_addr_hi_hdr_sz | addr_hi | 15:0 | buffer addr[47:32] |
| buff_addr_hi_hdr_sz | header_length | 31:24 | LLQ push len / host L4 hdr split |

Mask/shift constants: `ena_eth_io_defs.h:282-314`.

`struct ena_eth_io_tx_cdesc` (8 bytes, `ena_eth_io_defs.h:148-164`):
`req_id`(u16) | `status`(u8) | `flags`(u8: phase bit0, MBZ6 bits7:6) |
`sub_qid`(u16) | `sq_head_idx`(u16).
