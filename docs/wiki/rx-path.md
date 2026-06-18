# RX Datapath Runtime Semantics

Runtime behavior of the ENA receive datapath from a device point of view:
what DMA/MMIO the driver performs to post Rx buffers, what the device must
write into RX completion descriptors, and how the driver consumes them.

Reference driver: AWS DPDK PMD (`userspace/dpdk/ena`), cross-checked against
`ena_com`/`ena_eth_com`. Paths below are relative to `~/ena/amzn-drivers/`.

Descriptor and CDESC byte layouts (bitfields, struct offsets) live in
[rx-descriptors.md](./rx-descriptors.md) — not duplicated here.
Queue creation, doorbell/unmask register offsets, and head/tail/phase init
live in [queue-setup.md](./queue-setup.md).
Reset trigger plumbing lives in [device-init.md](./device-init.md) /
[registers.md](./registers.md).

---

## 1. RX SQ model (buffer posting queue)

An RX queue pair is (RX CQ, RX SQ) as in [queue-setup.md](./queue-setup.md).
The **RX SQ holds buffer descriptors** (`ena_eth_io_rx_desc`) that the driver
posts to hand empty DMA buffers to the device. The **RX CQ holds completion
descriptors** (`ena_eth_io_rx_cdesc_base`) the device writes when a packet has
landed.

The RX SQ is **always a HOST-placement (regular) queue** — RX buffer
descriptors are never LLQ. `ena_com_add_single_rx_desc` calls
`get_sq_desc_regular_queue` directly and updates the regular-queue tail, never
the LLQ/bounce-buffer path. [userspace/dpdk/ena/base/ena_eth_com.c:L698-L735]
(explicit)

Ring tracking is initialized before CREATE: SQ `tail = 0`, `next_to_comp = 0`,
`phase = 1`; CQ `head = 0`, `phase = 1`.
[userspace/dpdk/ena/base/ena_com.c:L408-L410, L453-L454] (explicit, see
[queue-setup.md](./queue-setup.md) §8)

---

## 2. RX buffer posting / refill flow

### 2.1 ena_com layer — building one RX SQ descriptor

`ena_com_add_single_rx_desc(io_sq, ena_buf, req_id)`:
[userspace/dpdk/ena/base/ena_eth_com.c:L698-L735] (explicit)

1. Space check: `ena_com_sq_have_enough_space(io_sq, 1)`; returns
   `ENA_COM_NO_SPACE` if full. Free entries =
   `q_depth - 1 - (tail - next_to_comp)`, so **one slot is always kept
   unused** (the ring can never be 100% full).
   [userspace/dpdk/ena/base/ena_eth_com.h:L85-L102] (explicit)
2. `desc = get_sq_desc_regular_queue(io_sq)` → points at
   `desc_addr.virt_addr + (tail & (q_depth-1)) * desc_entry_size`. The slot is
   chosen by the **masked tail**. [ena_eth_com.c:L84-L94] (explicit)
3. `memset(desc, 0, sizeof(ena_eth_io_rx_desc))` then fills:
   - `length = ena_buf->len`
   - `ctrl = FIRST | LAST | COMP_REQ | phase`  — every RX buffer descriptor is
     posted as a **self-contained single-buffer transaction** (FIRST and LAST
     both set) and **always requests a completion** (COMP_REQ set). The phase
     bit written is the current `io_sq->phase`.
   - `req_id = req_id` (the driver's buffer handle, see §2.3)
   - `buff_addr_lo` / `buff_addr_hi` = buffer physical address; high bits
     masked to `dma_addr_bits`.
   [ena_eth_com.c:L713-L732] (explicit)
4. `ena_com_sq_update_reqular_queue_tail`: `tail++`; if
   `(tail & (q_depth-1)) == 0` then `phase ^= 1`.
   [ena_eth_com.c:L261-L270] (explicit)

Note the layout comment for `rx_desc.length`: **0 means 64 KB**.
[userspace/dpdk/ena/base/ena_defs/ena_eth_io_defs.h:L167-L168] (explicit)

The driver never writes a metadata/multi-buffer RX SQ descriptor; on the
submission side FIRST/LAST/COMP_REQ are constant per descriptor. (explicit,
from L717-L719 — no other code path posts RX descs)

### 2.2 DPDK layer — refill and doorbell

`ena_populate_rx_queue(rxq, count)`:
[userspace/dpdk/ena/ena_ethdev.c:L1772-L1837] (explicit)

1. Bulk-allocate `count` mbufs. If allocation fails, **post nothing** and
   return 0 (no partial). [L1794-L1800] (explicit)
2. For each buffer: take `req_id = empty_rx_reqs[next_to_use]`, look up
   `rx_buffer_info[req_id]`, and call `ena_add_single_rx_desc`. On the first
   `ena_com_add_single_rx_desc` failure the loop **breaks** (partial refill).
   [L1802-L1818] (explicit)
3. `ena_add_single_rx_desc` sets the device-visible buffer address to
   `mbuf->buf_iova + RTE_PKTMBUF_HEADROOM` and length to
   `mbuf->buf_len - RTE_PKTMBUF_HEADROOM`. [L1754-L1770] (explicit)
4. If `i > 0` buffers were posted, ring the **SQ doorbell** once for the whole
   batch and advance `rxq->next_to_use`. [L1828-L1834] (explicit)

**Doorbell = absolute tail.** `ena_com_write_sq_doorbell` does a single
32-bit MMIO write of `io_sq->tail` to `io_sq->db_addr`
(`reg_bar + sq_doorbell_offset` from CREATE_SQ).
[userspace/dpdk/ena/base/ena_eth_com.h:L162-L181] (explicit) It writes the
**current absolute tail index**, not an increment; the device infers newly
posted descriptors from the delta against its own last-seen tail. The LLQ
tx-burst reset branch in that function never fires for RX (RX is HOST
placement). (explicit / inferred: RX SQ is HOST per §1)

A write barrier orders the descriptor stores before the doorbell: regular-queue
posting relies on `ENA_REG_WRITE32` issuing after the descriptor memset/fills;
the explicit `wmb()` in `ena_com_write_bounce_buffer_to_dev` is LLQ-only.
For RX the DPDK platform `ENA_REG_WRITE32` provides the ordering MMIO store.
(inferred — no explicit `wmb()` in the RX HOST post path at L698-L735)

### 2.3 req_id allocation / tracking (empty-req-id list)

`req_id` is the driver's **buffer handle**, echoed by the device in the
completion so the driver can map a CDESC back to the mbuf that filled it.

- `empty_rx_reqs[]` is a free-list ring of available req_ids, initialized
  identity `empty_rx_reqs[i] = i` for `i in [0, ring_size)` at queue setup.
  [userspace/dpdk/ena/ena_ethdev.c:L1733-L1735] (explicit) (a second init site
  at [L1437-L1439] for the restart path).
- `rx_buffer_info[req_id].mbuf` stores the mbuf currently owned by that
  req_id. [L1809-L1816] (explicit)
- On refill, req_ids are drawn from `empty_rx_reqs[next_to_use++]`.
  [L1809-L1817] (explicit)
- On completion, the consumed req_id is returned to the free list at the
  `next_to_clean` slot. [L2714-L2716, L2764-L2766, L2841-L2846] (explicit)

So **the SQ tail index and the req_id are decoupled**: req_id is not the SQ
slot. The device must round-trip whatever `req_id` was in the RX SQ
descriptor into the matching RX CDESC. (explicit — driver only ever looks up
by `cdesc->req_id`, never by CQ slot, see §4)

### 2.4 Buffer size constraints / min/max post sizes

- Per-buffer usable size = `mbuf data_room_size - RTE_PKTMBUF_HEADROOM`,
  validated at rx_queue_setup. [userspace/dpdk/ena/ena_ethdev.c:L1682]
  (explicit)
- `ENA_RX_BUF_MIN_SIZE = 1400` bytes — a configuration floor used by large-LLQ
  / mbuf sizing. [userspace/dpdk/ena/ena_ethdev.h:L29] (explicit)
- Max scatter buffers per packet `ENA_PKT_MAX_BUFS = 17`; the per-queue
  `sgl_size` (= `rx_ctx.max_bufs`) is `min(17, device max_per_packet_rx_descs)`
  from limits negotiation. [ena_ethdev.h:L28], [ena_ethdev.c:L1145, L1165]
  (explicit, see [queue-setup.md](./queue-setup.md) §7)
- Initial fill at queue start posts `ring_size - 1` buffers (one slot kept
  free, matching §2.1 space rule). [ena_ethdev.c:L1521-L1530] (explicit)
- Refill threshold: refill is deferred until free SQ entries
  `>= rx_free_thresh`, where `rx_free_thresh = min(ring_size/8, 256)`.
  [ena_ethdev.h:L56-L57], [ena_ethdev.c:L1742-L1744, L2864-L2869] (explicit)

---

## 3. Device-side expectations when consuming the RX SQ

Inferred from the contract the driver writes (no device source available):

- The device reads RX SQ buffer descriptors **in tail order** starting from
  the descriptor after the one it last consumed; the absolute tail written to
  the doorbell bounds how far it may read. (inferred from §2.2 absolute-tail
  doorbell + §2.1 masked-tail slot selection)
- **Phase**: each RX SQ descriptor carries the driver's `io_sq->phase` at post
  time; the phase flips every time the SQ `tail` wraps `q_depth`. A descriptor
  is "valid/new" to the device when its phase matches the device's expected SQ
  phase. [ena_eth_com.c:L717-L722, L261-L270] (explicit that the driver writes
  it; device honoring is inferred — symmetric to the CQ phase protocol in §4).
- **FIRST/LAST/COMP_REQ on the submission side**: each RX buffer descriptor is
  FIRST+LAST (a one-descriptor "transaction" = one buffer) and COMP_REQ
  (device must emit a completion). The device may consume **multiple** RX SQ
  buffers to land one large packet, but each buffer is an independent
  single-descriptor SQ entry. [ena_eth_com.c:L717-L719] (explicit for what is
  written; multi-buffer landing inferred from the multi-cdesc consumption in
  §4)
- The device must preserve `length` (clamp data into the buffer) and echo
  `req_id` into the resulting CDESC(s). (inferred from §2.3 / §4)

---

## 4. RX completion handling

### 4.1 CQ polling and phase protocol

`ena_com_get_next_rx_cdesc(io_cq)`:
[userspace/dpdk/ena/base/ena_eth_com.c:L8-L33] (explicit)

1. `head_masked = head & (q_depth-1)`; `expected_phase = io_cq->phase`.
2. CDESC pointer = `cdesc_addr.virt_addr + head_masked * cdesc_entry_size`.
3. Read `cdesc->status`, extract PHASE bit (bit 24). If
   `desc_phase != expected_phase` → return NULL (no new completion).
4. `dma_rmb()` after the phase read, before reading the rest of the CDESC.

The expected phase flips on wrap in `ena_com_cq_inc_head`: `head++`; if
`(head & (q_depth-1)) == 0` then `phase ^= 1`.
[userspace/dpdk/ena/base/ena_eth_com.h:L204-L211] (explicit) CQ phase starts at
1. So the device must write PHASE=1 into CDESCs on the first pass through the
ring, PHASE=0 on the second, etc. (explicit contract)

`ena_com_cq_empty` is just `ena_com_get_next_rx_cdesc() == NULL`.
[ena_eth_com.c:L737-L746] (explicit)

### 4.2 Gathering the cdescs of one packet (first/last)

`ena_com_cdesc_rx_pkt_get(io_cq, &first_cdesc_idx, &num_descs)`:
[userspace/dpdk/ena/base/ena_eth_com.c:L289-L350] (explicit)

Loop, accumulating cdescs until one with LAST set:

- For each cdesc, get it via `ena_com_get_next_rx_cdesc`; stop the loop if NULL
  (partial packet not yet fully written — saved state in
  `cur_rx_pkt_cdesc_count` / `cur_rx_pkt_cdesc_start_idx`, resumed next call).
  [L298-L303, L344-L347] (explicit)
- **FIRST validation**: if FIRST (bit 26) is set on any cdesc with
  `count != 0` → `ENA_COM_FAULT` (a new packet started mid-packet).
  [L306-L314] (explicit)
- **MBZ validation**: if status has MBZ7 (bit 7) or MBZ17 (bit 17) set and the
  device advertised `ENA_ADMIN_CDESC_MBZ` capability → `ENA_COM_FAULT`
  (corrupted descriptor). [L316-L323] (explicit)
- `ena_com_cq_inc_head` (advance head + phase), `count++`, read LAST (bit 27).
  Loop while `!last`. [L325-L330] (explicit)
- On LAST: `*first_cdesc_idx = cur_rx_pkt_cdesc_start_idx`, `*num_descs = count`,
  reset `cur_rx_pkt_cdesc_count = 0`, set `cur_rx_pkt_cdesc_start_idx =
  head_masked`. [L332-L343] (explicit)

So **one packet = a run of cdescs from FIRST..LAST**, each consuming one head
slot and contributing one buffer. The device sets FIRST on the first CDESC,
LAST on the last, and the phase bit on all of them.

### 4.3 Mapping cdesc.req_id back to a buffer + req_id validation

`ena_com_rx_pkt(io_cq, io_sq, rx_ctx)`:
[userspace/dpdk/ena/base/ena_eth_com.c:L622-L696] (explicit)

1. Call `ena_com_cdesc_rx_pkt_get`. On its `ENA_COM_FAULT` →
   `ena_com_rx_pkt` returns `ENA_COM_FAULT`. [L637-L639] (explicit)
2. If `num_descs == 0` (no complete packet) → `rx_ctx->descs = 0`, return 0.
   [L641-L644] (explicit)
3. **Too-many-cdescs check**: if `nb_hw_desc > rx_ctx->max_bufs` →
   `ENA_COM_NO_SPACE`. [L650-L655] (explicit)
4. `pkt_offset = first_cdesc->offset` (byte offset into the first buffer where
   data starts). [L657-L658] (explicit)
5. For each of `nb_hw_desc` cdescs (indexed `cdesc_idx + i`), copy
   `ena_bufs[i].len = cdesc->length` and `ena_bufs[i].req_id = cdesc->req_id`.
   **req_id validation**: if `req_id >= q_depth` → `ENA_COM_EIO`.
   [L660-L671] (explicit)
6. `io_sq->next_to_comp += nb_hw_desc` — advances the SQ "head" tracking so
   those SQ slots are now free for refill. [L673-L674] (explicit)
7. `ena_com_rx_set_flags` from the **last** cdesc (offload/hash, §6).
   [L680-L681] (explicit)
8. `rx_ctx->descs = nb_hw_desc`. [L693] (explicit)

DPDK consumes the result in `ena_rx_mbuf`:
[userspace/dpdk/ena/ena_ethdev.c:L2680-L2772] (explicit)
- `req_id = ena_bufs[buf].req_id` → `rx_buffer_info[req_id].mbuf` is the mbuf
  to hand up; req_id is returned to `empty_rx_reqs`. [L2698-L2716] (explicit)
- Multi-buffer packets are chained mbuf→next, `pkt_len` accumulated; each
  segment's length comes from `ena_bufs[buf].len`. [L2718-L2767] (explicit)
- **Zero-length trailing descriptor handling**: a cdesc with `len == 0` is
  treated as "unused"; the driver re-posts that same buffer
  (`ena_add_single_rx_desc`) and treats it as the last descriptor.
  [L2726-L2744] (explicit)
- `pkt_offset` is applied via `mbuf_head->data_off += offset`. [L2712]
  (explicit)

### 4.4 CQ head doorbell — driver does NOT write it

Neither the DPDK PMD nor `ena_com` writes any IO RX CQ head doorbell. RX
completions are consumed **purely by the phase bit**; `io_cq->head` is a
driver-private counter and is never written back to the device.

- No `cq_head_db_register_offset` consumer exists in the DPDK base layer; the
  only occurrence of that symbol is the struct field definition.
  [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L380] (explicit — grep of
  `base/` finds no other use)
- There is no `ena_com_update_dev_comp_head`-style function in the DPDK base.
  (explicit — absent from the tree)
- The driver's only RX MMIO writes are the **SQ doorbell** (refill, §2.2) and
  the **interrupt unmask register** (§5). (explicit)

**Flag for device implementers:** `cq_head_db_register_offset` is a value the
device returns in CREATE_CQ but the driver never reads/uses for IO CQs — a
device write the driver ignores. See [queue-setup.md](./queue-setup.md) §3.
The device frees an RX SQ buffer slot implicitly when the driver re-posts it
(refill), not via any CQ head acknowledgement.

### 4.5 Length accounting summary

- Per-buffer received length = `cdesc->length` (16-bit). Summed across the
  FIRST..LAST run = packet `pkt_len`. [ena_eth_com.c:L661], [ena_ethdev.c:L2711,
  L2751] (explicit)
- Number of buffers/cdescs consumed for the packet = `nb_hw_desc` (= mbuf
  `nb_segs`). [ena_ethdev.c:L2709] (explicit)

---

## 5. RX interrupt path (unmask / moderation)

The RX IO CQ has an MSI-X vector assigned at CREATE_CQ (`msix_vector`), and the
device returns `cq_interrupt_unmask_register_offset`; the driver stores
`io_cq->unmask_reg = reg_bar + that_offset`.
[userspace/dpdk/ena/base/ena_com.c:L1451-L1459] (explicit, see
[queue-setup.md](./queue-setup.md) §3)

### Unmask / arm

`ena_rx_queue_intr_set(dev, queue_id, unmask)`:
[userspace/dpdk/ena/ena_ethdev.c:L3880-L3890] (explicit)
```c
ena_com_update_intr_reg(&intr_reg, 0 /*rx_delay*/, 0 /*tx_delay*/,
                        unmask, 1 /*no_moderation_update*/);
ena_com_unmask_intr(rxq->ena_com_io_cq, &intr_reg);
```
`ena_com_unmask_intr` is a single 32-bit MMIO write of `intr_reg.intr_control`
to `io_cq->unmask_reg`. [userspace/dpdk/ena/base/ena_eth_com.h:L79-L83]
(explicit)

### ENA_ETH_IO_INTR_REG layout (`intr_control`, 32-bit)
[userspace/dpdk/ena/base/ena_defs/ena_eth_io_defs.h:L263-L271, L389-L395]
(explicit)
```
bits 14:0  rx_intr_delay         (usecs)
bits 29:15 tx_intr_delay         (usecs)
bit  30    intr_unmask           (1 = unmask/arm the vector)
bit  31    no_moderation_update  (1 = do NOT update moderation intervals)
```
Built by `ena_com_update_intr_reg`:
[userspace/dpdk/ena/base/ena_com.h:L1193-L1215] (explicit) — ORs
`rx_delay_interval` into 14:0, `tx_delay_interval` into 29:15, sets bit 30 iff
`unmask`, sets bit 31 from `no_moderation_update`.

### Moderation basics

In the DPDK RX intr enable/disable path the driver passes
`rx_delay = tx_delay = 0` and `no_moderation_update = 1`, i.e. it writes the
register **only to unmask** and explicitly tells the device not to reprogram
moderation intervals. [ena_ethdev.c:L3888] (explicit) Dynamic/adaptive RX
moderation tables exist in `ena_com` but the DPDK RX fast path here does not
program per-completion delays — it just re-arms with bit 30. (explicit for the
RX intr_set path; broader moderation is out of scope for this page) See
[interrupts.md](./interrupts.md) for moderation negotiation, the MSI-X vector
model, and `no_moderation_update`/mask-unmask device semantics.

Startup state: RX queues with datapath interrupts begin **masked** (the
unmask write is deferred to when the application enables the Rx interrupt).
[ena_ethdev.c:L1467-L1469] (explicit, see [queue-setup.md](./queue-setup.md)
§8)

**Device expectation (inferred):** writing `intr_control` with bit 30 set
re-arms the CQ's MSI-X vector so the next RX completion can raise an
interrupt; clearing bit 30 (driver writes with `unmask=false`) masks it. The
device latches the delay fields only when bit 31 (no_moderation_update) is 0.
(inferred from the register field semantics + how the driver toggles them)

---

## 6. Offload / hash completion semantics (overview)

Read from the **last** cdesc of the packet by `ena_com_rx_set_flags`:
[userspace/dpdk/ena/base/ena_eth_com.c:L429-L455] (explicit). Bit positions
and full semantics are in [rx-descriptors.md](./rx-descriptors.md); summary of
what the driver consumes:

- `l3_proto` (status bits 4:0), `l4_proto` (bits 12:8) — protocol indices.
- `l3_csum_err` (bit 13), `l4_csum_err` (bit 14) — checksum-error flags.
- `l4_csum_checked` (bit 16) — when clear, the L4 checksum status is **unknown**
  and the driver must not trust `l4_csum_err`. The DPDK mbuf-prep path keys
  off this. [ena_eth_com.c:L446-L449] (explicit)
- `hash` (full 32-bit word) — RSS hash, surfaced as RSS hash.
- `frag` (bit 15, ipv4_frag).

`l3_csum2` (bit 25, "second checksum engine result") and `buffer` (bit 30,
metadata vs buffer descriptor) are present in the layout but **not consumed**
by `ena_com_rx_set_flags`. (explicit — not read at L429-L455; flag for device
implementers as driver-ignored status bits, though MBZ bits 7/17 ARE checked
per §4.2)

---

## 7. Error / edge behavior and reset reasons

`eth_ena_recv_pkts` maps `ena_com_rx_pkt` return codes to reset reasons and
calls `ena_trigger_reset`, then returns 0:
[userspace/dpdk/ena/ena_ethdev.c:L2809-L2832] (explicit). Reset reason enum
values from [userspace/dpdk/ena/base/ena_defs/ena_regs_defs.h:L9-L30]
(explicit):

| ena_com return | cause | reset reason | value |
|---|---|---|---|
| `ENA_COM_NO_SPACE` | `nb_hw_desc > max_bufs` (packet spans more buffers than the SGL limit) | `ENA_REGS_RESET_TOO_MANY_RX_DESCS` | 6 |
| `ENA_COM_FAULT` | FIRST-bit set mid-packet, or MBZ bit set with CDESC_MBZ cap | `ENA_REGS_RESET_RX_DESCRIPTOR_MALFORMED` | 16 |
| `ENA_COM_EIO` | `cdesc->req_id >= q_depth` (invalid req_id) | `ENA_REGS_RESET_INV_RX_REQ_ID` | 4 |
| other | unexpected | `ENA_REGS_RESET_DRIVER_INVALID_STATE` | 8 |

Notes:
- **Out-of-buffers (Rx):** purely a driver-side condition. If mbuf allocation
  fails, `ena_populate_rx_queue` posts nothing and bumps `rx_nombuf` /
  `mbuf_alloc_fail` — it does **not** reset the device.
  [ena_ethdev.c:L1794-L1800] (explicit) The device simply has fewer posted
  buffers; if it has no free RX SQ buffer for an incoming packet, that drop is
  device-side (not modeled by driver code here). (inferred)
- **Partial refill:** if some `ena_com_add_single_rx_desc` in the batch fails,
  the loop breaks, unused mbufs are freed, `refill_partial` is bumped, and the
  doorbell is still rung for the buffers that were posted (`i > 0`).
  [ena_ethdev.c:L1812-L1834] (explicit)
- **Packet larger than buffers:** surfaces as `nb_hw_desc > max_bufs` →
  `TOO_MANY_RX_DESCS` reset (above). A correctly-behaving device must split a
  packet across at most `sgl_size` (= negotiated `max_per_packet_rx_descs`,
  ≤ 17) RX buffers. (explicit reset path; bound inferred from §2.4)
- **MISS / invalid req_id:** any `req_id` the device writes that is
  `>= q_depth` triggers `INV_RX_REQ_ID` (value 4). The DPDK layer additionally
  guards: a returned packet whose `ena_rx_mbuf` yields NULL restores the
  req_ids to `empty_rx_reqs` and breaks without reset.
  [ena_ethdev.c:L2840-L2848] (explicit)
- There is **no RX "MISS completion" watchdog** in this RX path equivalent to
  the Tx `MISS_TX_CMPL`; RX miss/stall detection (if any) is handled elsewhere
  (keep-alive / interrupt-miss timers in the health path), not in
  `eth_ena_recv_pkts`. (inferred — only the four codes above appear here)

---

## 8. Device write/read summary (what the device writes vs. what the driver reads)

| Device writes (RX) | Driver reads it? | Where |
|---|---|---|
| CDESC `status` (phase, first, last, proto, csum, frag, l4_csum_checked) | yes | §4.1, §4.2, §6 |
| CDESC `length` | yes | §4.3 length accounting |
| CDESC `req_id` | yes (buffer lookup + validation) | §4.3 |
| CDESC `hash` | yes (RSS) | §6 |
| CDESC `offset` | yes (`pkt_offset` → mbuf data_off) | §4.3 |
| CDESC `sub_qid` | **no** — never read in RX path | §4.3 (not referenced) |
| CDESC `status.l3_csum2` (bit 25) | **no** | §6 |
| CDESC `status.buffer` (bit 30) | **no** | §6 |
| MSI-X RX interrupt (when armed) | yes (wakes poll) | §5 |
| (CREATE_CQ) `cq_head_db_register_offset` | **no** for IO CQ | §4.4 |

Driver→device RX MMIO: **SQ doorbell** (absolute tail, §2.2) and **interrupt
unmask register** (§5). No CQ head doorbell.

---

## Open ambiguities

- Device-side RX SQ consumption order and SQ-phase honoring are **inferred**
  from the symmetric doorbell/phase contract the driver writes; there is no
  device source in the tree to confirm exact read ordering.
- `cdesc->offset` (`pkt_offset`): the driver applies it as a per-packet data
  offset into the first buffer. When/why the device sets a non-zero offset
  (e.g. header padding) is not specified by driver code. (inferred use only)
- Whether a device may legitimately emit a zero-length **trailing** RX cdesc
  is implied by the DPDK `len == 0` handling (§4.3) but not documented as a
  required behavior. (inferred)
- RX completion-miss detection: this page covers only the synchronous poll
  path; any RX stall/interrupt-miss reset lives in the health/keepalive path
  and is out of scope here.
