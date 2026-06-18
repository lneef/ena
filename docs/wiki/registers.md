# ENA Register Map (PCI BAR0 MMIO)

Device-facing specification of the ENA control register window (the "reg
bar", BAR0). All offsets, widths, field masks/shifts are taken verbatim
from `ena_regs_defs.h`; behavioral semantics from `ena_com.c`. Three copies
of the regs header exist (DPDK, Linux kernel, FreeBSD) and are byte-identical
for everything documented here, so a single citation per fact is given from
the DPDK tree unless a cross-check matters.

All registers are 32-bit. The driver always accesses them with 32-bit
`ENA_REG_WRITE32` / `ENA_REG_READ32` (or the indirect read path, see below).
64-bit addresses are split into LO/HI register pairs. Offsets are byte
offsets from the start of BAR0.

## Register offset table

Source: [userspace/dpdk/ena/base/ena_defs/ena_regs_defs.h:36-65]

| Offset | Name | Acc | Purpose |
|--------|------|-----|---------|
| 0x00 | VERSION | RO | Device API version (major.minor) |
| 0x04 | CONTROLLER_VERSION | RO | Controller firmware version + impl id |
| 0x08 | CAPS | RO | Device capabilities (reset/admin timeouts, DMA width, contiguous-queue req) |
| 0x0c | CAPS_EXT | RO | Extended caps. Defined in header; **not read by the com layer** (no reference in ena_com.c) |
| 0x10 | AQ_BASE_LO | WO | Admin SQ base address, low 32 bits |
| 0x14 | AQ_BASE_HI | WO | Admin SQ base address, high 32 bits |
| 0x18 | AQ_CAPS | WO | Admin SQ depth + entry size (driver-programmed) |
| 0x20 | ACQ_BASE_LO | WO | Admin CQ base address, low 32 bits |
| 0x24 | ACQ_BASE_HI | WO | Admin CQ base address, high 32 bits |
| 0x28 | ACQ_CAPS | WO | Admin CQ depth + entry size (driver-programmed) |
| 0x2c | AQ_DB | WO (doorbell) | Admin SQ tail doorbell |
| 0x30 | ACQ_TAIL | - | Defined in header; **not accessed by the com layer** |
| 0x34 | AENQ_CAPS | WO | AENQ depth + entry size (driver-programmed) |
| 0x38 | AENQ_BASE_LO | WO | AENQ base address, low 32 bits |
| 0x3c | AENQ_BASE_HI | WO | AENQ base address, high 32 bits |
| 0x40 | AENQ_HEAD_DB | WO (doorbell) | AENQ head doorbell (consumer index) |
| 0x44 | AENQ_TAIL | - | Defined in header; **not accessed by the com layer** |
| 0x4c | INTR_MASK | WO | Admin/AENQ interrupt mask (1 = masked) |
| 0x54 | DEV_CTL | WO | Device control: reset trigger + reset reason |
| 0x58 | DEV_STS | RO | Device status: ready / reset handshake / fatal |
| 0x5c | MMIO_REG_READ | WO (doorbell) | Indirect register-read request (req_id + reg_off) |
| 0x60 | MMIO_RESP_LO | WO | DMA address of mmio read response buffer, low 32 |
| 0x64 | MMIO_RESP_HI | WO | DMA address of mmio read response buffer, high 32 |
| 0x68 | RSS_IND_ENTRY_UPDATE | WO | Single RSS indirection entry update. Defined; **not accessed by com layer** |
| 0x100 | PHC_DB | WO (doorbell) | PHC (precision clock) doorbell. Separate 0x100 base |

Note: 0x1c, 0x48, 0x50, 0x6c..0xfc are not assigned register names in the
header (reserved/hole). The split between the 0x18 AQ_CAPS / 0x20 ACQ_BASE_LO
leaves 0x1c unused.

Access column legend: RO = device-produced, driver only reads; WO = driver
writes, device consumes (driver never reads it back); doorbell = a write is
itself an event/notification to the device. "-" = present in the address map
but not exercised by the reference com layer, so device-side semantics are
unspecified by the driver.

## Register field definitions

### VERSION (0x00, RO)
[ena_regs_defs.h:67-69]
- bits [7:0]  MINOR_VERSION (mask 0xff)
- bits [15:8] MAJOR_VERSION (shift 8, mask 0xff00)

Read in `ena_com_validate_version` [ena_com.c:1645,1655-1661]; used only for
logging on the version field itself.

### CONTROLLER_VERSION (0x04, RO)
[ena_regs_defs.h:72-78]
- bits [7:0]   SUBMINOR_VERSION (mask 0xff)
- bits [15:8]  MINOR_VERSION (shift 8, mask 0xff00)
- bits [23:16] MAJOR_VERSION (shift 16, mask 0xff0000)
- bits [31:24] IMPL_ID (shift 24, mask 0xff000000)

`ena_com_validate_version` masks out IMPL_ID and requires the remaining
major/minor/subminor value to be >= MIN_ENA_CTRL_VER, else returns -1
[ena_com.c:1677-1686]. MIN_ENA_CTRL_VER is built from ENA_CTRL_MAJOR=0,
ENA_CTRL_MINOR=0, ENA_CTRL_SUB_MINOR=1, i.e. the encoded value 0x000001
(controller version must be at least 0.0.1) [ena_com.c:17-26].

### CAPS (0x08, RO)
[ena_regs_defs.h:81-87]
- bit  [0]     CONTIGUOUS_QUEUE_REQUIRED (mask 0x1)
- bits [5:1]   RESET_TIMEOUT (shift 1, mask 0x3e)
- bits [15:8]  DMA_ADDR_WIDTH (shift 8, mask 0xff00)
- bits [19:16] ADMIN_CMD_TO (shift 16, mask 0xf0000)

Driver interpretation:
- RESET_TIMEOUT: unit is 100 ms. The raw field is the timeout used directly
  in `wait_for_reset_state`, which converts `100 * 1000 * timeout` us
  [ena_com.c:998-999, 2535-2541]. A value of 0 is rejected as invalid
  ("Invalid timeout value", ENA_COM_INVAL) [ena_com.c:2538-2541]. So the
  device must advertise a non-zero reset timeout.
- DMA_ADDR_WIDTH: number of physical address bits. `ena_com_get_dma_width`
  reads it and requires `32 <= width <= ENA_MAX_PHYS_ADDR_SIZE_BITS` (48),
  else ENA_COM_INVAL; stored in `ena_dev->dma_addr_bits`
  [ena_com.c:1620-1633; ena_com.h:17].
- ADMIN_CMD_TO: admin command timeout, unit 100 ms. Used after reset:
  if non-zero, `admin_queue.completion_timeout = timeout * 100000` us;
  if zero, falls back to ADMIN_CMD_TIMEOUT_US = 3000000 us (3 s)
  [ena_com.c:2590-2597; ena_com.c:12].
- CONTIGUOUS_QUEUE_REQUIRED: read by the higher driver layer (host memory
  policy), not by the offset/field logic in ena_com.c documented here.

Note: admin/AENQ queue *depths* are NOT taken from CAPS. They are driver
constants programmed into AQ_CAPS/ACQ_CAPS/AENQ_CAPS (see below).

### CAPS_EXT (0x0c, RO)
[ena_regs_defs.h:39]
Offset is defined but the com layer never reads it (no reference in
ena_com.c in either the DPDK or Linux tree). No field definitions exist in
the header. Device-side meaning is unspecified by the reference driver.

### AQ_BASE_LO / AQ_BASE_HI (0x10 / 0x14, WO)
Admin Submission Queue base physical address. Written during
`ena_com_admin_init` after the SQ memory is allocated:
LO = low 32 bits, HI = high 32 bits of `admin_queue->sq.dma_addr`
[ena_com.c:2117-2121].

### AQ_CAPS (0x18, WO)
[ena_regs_defs.h:90-92]
- bits [15:0]  AQ_DEPTH (mask 0xffff)
- bits [31:16] AQ_ENTRY_SIZE (shift 16, mask 0xffff0000)

Driver programs DEPTH = `admin_queue->q_depth` = ENA_ADMIN_QUEUE_DEPTH = 32,
and ENTRY_SIZE = `sizeof(struct ena_admin_aq_entry)`
[ena_com.c:2091, 2129-2135; ena_com.c:15]. This is a driver->device write
that tells the device the admin SQ geometry.

### ACQ_BASE_LO / ACQ_BASE_HI (0x20 / 0x24, WO)
Admin Completion Queue base physical address, from
`admin_queue->cq.dma_addr` [ena_com.c:2123-2127].

### ACQ_CAPS (0x28, WO)
[ena_regs_defs.h:95-97]
- bits [15:0]  ACQ_DEPTH (mask 0xffff)
- bits [31:16] ACQ_ENTRY_SIZE (shift 16, mask 0xffff0000)

DEPTH = q_depth (32), ENTRY_SIZE = `sizeof(struct ena_admin_acq_entry)`
[ena_com.c:2137-2143].

### AQ_DB (0x2c, WO doorbell)
Admin SQ tail doorbell. On every admin command submission the driver copies
the command into the SQ ring, advances the masked `sq.tail`, flips the SQ
phase bit on wrap, and writes the new absolute `sq.tail` value to AQ_DB
[ena_com.c:264-277]. Device action on write: the new entries up to the
written tail index are available for the device to process. `db_addr` is set
to `reg_bar + ENA_REGS_AQ_DB_OFF` [ena_com.c:2114-2115].

### ACQ_TAIL (0x30)
Defined in the header but never written or read by the com layer. Admin
completions are detected via the per-entry phase bit, not via a tail
register read. Device-side semantics unspecified by the driver.

### AENQ_CAPS (0x34, WO)
[ena_regs_defs.h:100-102]
- bits [15:0]  AENQ_DEPTH (mask 0xffff)
- bits [31:16] AENQ_ENTRY_SIZE (shift 16, mask 0xffff0000)

Written in `ena_com_admin_init_aenq` together with the base address
[ena_com.c:161-172].

### AENQ_BASE_LO / AENQ_BASE_HI (0x38 / 0x3c, WO)
AENQ ring base physical address, written in `ena_com_admin_init_aenq`
[ena_com.c:161-162].

### AENQ_HEAD_DB (0x40, WO doorbell)
AENQ head (consumer index) doorbell. After the driver consumes AENQ entries
it writes the new head. At enable time `ena_com_admin_aenq_enable` writes the
full queue depth to mark all entries initially available to the device
[ena_com.c:1557-1567]. During steady-state processing the driver writes the
updated `aenq->head` (relaxed write) [ena_com.c:2466]. Device uses this to
know how many AENQ slots are free.

### AENQ_TAIL (0x44)
Defined in the header but not accessed by the com layer (the device owns the
tail and the driver tracks producer position via the per-entry phase bit).

### INTR_MASK (0x4c, WO)
`ena_com_set_admin_polling_mode` writes ENA_REGS_ADMIN_INTR_MASK (= 1) to
mask the admin/AENQ interrupt when polling, or 0 to unmask
[ena_com.c:35, 1737-1746]. Semantics: bit 0 set = admin interrupt masked.

### DEV_CTL (0x54, WO)
[ena_regs_defs.h:105-115]
- bit  [0]     DEV_RESET (mask 0x1)
- bit  [1]     AQ_RESTART (shift 1, mask 0x2)
- bit  [2]     QUIESCENT (shift 2, mask 0x4)
- bit  [3]     IO_RESUME (shift 3, mask 0x8)
- bits [27:24] RESET_REASON_EXT (shift 24, mask 0xf000000)
- bits [31:28] RESET_REASON (shift 28, mask 0xf0000000)

Used by the reset sequence (below). Only DEV_RESET + the reset-reason fields
are written by ena_com.c. AQ_RESTART / QUIESCENT / IO_RESUME bits are defined
but not driven by the reference com layer.

### DEV_STS (0x58, RO)
[ena_regs_defs.h:118-132]
- bit [0] READY (mask 0x1)
- bit [1] AQ_RESTART_IN_PROGRESS (mask 0x2)
- bit [2] AQ_RESTART_FINISHED (mask 0x4)
- bit [3] RESET_IN_PROGRESS (mask 0x8)
- bit [4] RESET_FINISHED (mask 0x10)
- bit [5] FATAL_ERROR (mask 0x20)
- bit [6] QUIESCENT_STATE_IN_PROGRESS (mask 0x40)
- bit [7] QUIESCENT_STATE_ACHIEVED (mask 0x80)

Driver usage:
- READY: checked before admin init and before reset; must be set or the
  driver aborts (ENA_COM_NO_DEVICE / ENA_COM_INVAL)
  [ena_com.c:2086-2089, 2530-2533].
- RESET_IN_PROGRESS: the reset handshake bit polled by
  `wait_for_reset_state` [ena_com.c:1009, 2575-2588].
- The READY/RESET_FINISHED/FATAL_ERROR bits are also consulted by the
  higher-level driver health checks (outside the offset logic here).

### MMIO_REG_READ (0x5c, WO doorbell)
[ena_regs_defs.h:135-137]
- bits [15:0]  REQ_ID (mask 0xffff)
- bits [31:16] REG_OFF (shift 16, mask 0xffff0000)

The indirect-read request register. See "Indirect MMIO read" below.

### MMIO_RESP_LO / MMIO_RESP_HI (0x60 / 0x64, WO)
DMA address of the host response buffer the device writes mmio-read results
into. Written in `ena_com_mmio_reg_read_request_write_dev_addr`
[ena_com.c:2060-2069]. Re-written after every device reset because reset
clears device state [ena_com.c:2572-2573]. On
`ena_com_mmio_reg_read_request_destroy` the driver writes 0 to both
to tell the device to stop using the buffer [ena_com.c:2047-2048].

### RSS_IND_ENTRY_UPDATE (0x68, WO)
[ena_regs_defs.h:140-142]
- bits [15:0]  INDEX (mask 0xffff)
- bits [31:16] CQ_IDX (shift 16, mask 0xffff0000)

Defined in the header but not written by the com layer (RSS indirection is
configured via admin commands in this driver). Device-side meaning
unspecified by the reference.

### PHC_DB (0x100, WO doorbell)
[ena_regs_defs.h:64, 145]
- bits [15:0] REQ_ID (mask 0xffff)

PHC (precision host clock) request doorbell. The driver writes `phc->req_id`
to `reg_bar + phc->doorbell_offset` [ena_com.c:1938]. Only relevant when the
PHC feature is enabled. Lives at a separate 0x100 base.

## Indirect MMIO register read (readless mechanism)

Function: `ena_com_reg_bar_read32` [ena_com.c:840-898].

When `mmio_read->readless_supported` is true (the default after
`ena_com_mmio_reg_read_request_init` [ena_com.c:2010-2034]), the driver does
NOT read RO registers directly. Instead:

1. Driver has previously published a host DMA buffer address via
   MMIO_RESP_LO/HI. The buffer is a
   `struct ena_admin_ena_mmio_req_read_less_resp`
   [ena_admin_defs.h:1263-1270]:
   ```
   uint16_t req_id;   /* offset 0 */
   uint16_t reg_off;  /* offset 2 */
   uint32_t reg_val;  /* offset 4; valid when poll/req_id matches */
   ```
   Total 8 bytes.
2. Driver increments `seq_num` and pre-seeds `read_resp->req_id` with
   `seq_num + 0xDEAD` so it can detect when the device overwrites it
   [ena_com.c:859-861].
3. Driver builds the request word: REG_OFF field = the target register
   offset, REQ_ID field = `seq_num & 0xffff`, and writes it to MMIO_REG_READ
   [ena_com.c:862-869].
4. Device must read the target register, then DMA-write the response buffer
   with `req_id = seq_num`, `reg_off = requested offset`, `reg_val = current
   register value`.
5. Driver polls `read_resp->req_id` until it equals `seq_num`, busy-waiting
   1 us per iteration up to `timeout` iterations [ena_com.c:871-876].
   `timeout` = `mmio_read->reg_read_to` if set, else ENA_REG_READ_TIMEOUT =
   200000 (i.e. up to 200000 us = 200 ms) [ena_com.c:847-852; ena_com.h:20].
   `reg_read_to` is never assigned by the reference com layer, so the
   effective default is 200 ms.
6. On timeout the read returns ENA_MMIO_READ_TIMEOUT = 0xFFFFFFFF
   [ena_com.c:878-886; ena_com.c:31]. Callers treat 0xFFFFFFFF as a fatal
   read failure.
7. After a req_id match, the driver also checks `read_resp->reg_off ==
   offset`; a mismatch is treated as a read failure (returns 0xFFFFFFFF)
   [ena_com.c:888-892]. So the device must echo the correct register offset.

The whole sequence is serialized under `mmio_read->lock` so only one
outstanding indirect read exists at a time [ena_com.c:858, 895].

If readless is disabled (`ena_com_set_mmio_read_mode(..., false)`), the
driver instead does a plain 32-bit BAR read of the register
[ena_com.c:854-856, 2036-2040].

## Device reset sequence

Function: `ena_com_dev_reset` [ena_com.c:2514-2599].

Preconditions / reads:
1. Read DEV_STS and CAPS via the indirect read; if either times out
   (0xFFFFFFFF) return ENA_COM_TIMER_EXPIRED [ena_com.c:2521-2528].
2. DEV_STS READY bit must be set, else ENA_COM_INVAL ("Device isn't ready,
   can't reset device") [ena_com.c:2530-2533].
3. Extract RESET_TIMEOUT from CAPS (unit 100 ms); 0 is rejected as invalid
   [ena_com.c:2535-2541].

Trigger:
4. Build `reset_val = DEV_CTL.DEV_RESET (bit 0)`.
5. Encode the reset reason. The reset reason enum (0..20, ENA_REGS_RESET_*)
   is split: LSB nibble (reason & 0xf) goes into RESET_REASON bits [31:28];
   MSB nibble ((reason>>4) & 0xf) goes into RESET_REASON_EXT bits [27:24]
   [ena_com.c:2549-2560; ena_com.h:29-32]. The MSB/EXT half is only written
   if the device advertises the ENA_ADMIN_EXTENDED_RESET_REASONS capability
   (from the device-attributes `capabilities` bitmap, not a register). If the
   reason needs the MSB nibble but the device lacks EXTENDED_RESET_REASONS,
   the driver falls back to reason = ENA_REGS_RESET_GENERIC (13) encoded in
   the legacy RESET_REASON field only [ena_com.c:2559-2569].
6. Write `reset_val` to DEV_CTL (0x54) [ena_com.c:2570]. This is the write
   that starts the reset.
7. Re-publish the MMIO read response buffer address (MMIO_RESP_LO/HI),
   because reset clears it [ena_com.c:2572-2573].

Handshake (device must implement both transitions):
8. Wait for DEV_STS.RESET_IN_PROGRESS (bit 3) to turn ON:
   `wait_for_reset_state(timeout, RESET_IN_PROGRESS_MASK)` — poll DEV_STS
   until `(val & RESET_IN_PROGRESS) == RESET_IN_PROGRESS`. Failure ->
   "Reset indication didn't turn on" [ena_com.c:2575-2580].
9. Write 0 to DEV_CTL to clear the reset request [ena_com.c:2583].
10. Wait for DEV_STS.RESET_IN_PROGRESS to turn OFF:
    `wait_for_reset_state(timeout, 0)` [ena_com.c:2584-2588]. Failure ->
    "Reset indication didn't turn off".

Polling details of `wait_for_reset_state` [ena_com.c:992-1018]:
- Total deadline = `100 * 1000 * timeout` us (timeout from CAPS RESET_TIMEOUT,
  in 100 ms units).
- Each iteration reads DEV_STS via indirect read; an indirect-read timeout
  aborts with ENA_COM_TIMER_EXPIRED.
- Compares `(DEV_STS & RESET_IN_PROGRESS) == exp_state` (exp_state is the
  full mask 0x8 in step 8, 0 in step 10).
- Between polls it sleeps with exponential backoff starting from
  `ena_dev->ena_min_poll_delay_us`, capped by the overall deadline.

Post-reset:
11. Recompute `admin_queue.completion_timeout` from CAPS ADMIN_CMD_TO (unit
    100 ms; 0 -> 3 s default) [ena_com.c:2590-2597].

So from the device side, the minimal reset contract is: when bit 0 of DEV_CTL
is written 1, set DEV_STS.RESET_IN_PROGRESS; when DEV_CTL is written 0, clear
DEV_STS.RESET_IN_PROGRESS (and re-arm READY). The device must also keep
honoring the indirect MMIO read of DEV_STS throughout, and stop using the old
MMIO response buffer (driver re-publishes it).

## Reset reason encoding (enum values)

`enum ena_regs_reset_reason_types` [ena_regs_defs.h:8-31]:

| Val | Name | Val | Name |
|-----|------|-----|------|
| 0 | NORMAL | 11 | SHUTDOWN |
| 1 | KEEP_ALIVE_TO | 12 | USER_TRIGGER |
| 2 | ADMIN_TO | 13 | GENERIC |
| 3 | MISS_TX_CMPL | 14 | MISS_INTERRUPT |
| 4 | INV_RX_REQ_ID | 15 | SUSPECTED_POLL_STARVATION |
| 5 | INV_TX_REQ_ID | 16 | RX_DESCRIPTOR_MALFORMED |
| 6 | TOO_MANY_RX_DESCS | 17 | TX_DESCRIPTOR_MALFORMED |
| 7 | INIT_ERR | 18 | MISSING_ADMIN_INTERRUPT |
| 8 | DRIVER_INVALID_STATE | 19 | DEVICE_REQUEST |
| 9 | OS_TRIGGER | 20 | MISS_FIRST_INTERRUPT |
| 10 | OS_NETDEV_WD | | |

Reasons 0..15 fit in the legacy 4-bit RESET_REASON field. Reasons 16..20
require the MSB nibble and thus the EXTENDED_RESET_REASONS device capability.

## Constants summary (driver-side)

| Constant | Value | Source |
|----------|-------|--------|
| ENA_ADMIN_QUEUE_DEPTH | 32 | [ena_com.c:15] |
| ENA_ASYNC_QUEUE_DEPTH (AENQ) | 16 | [ena_com.c:14] |
| ENA_REG_READ_TIMEOUT | 200000 (us) | [ena_com.h:20] |
| ENA_MMIO_READ_TIMEOUT (sentinel) | 0xFFFFFFFF | [ena_com.c:31] |
| ADMIN_CMD_TIMEOUT_US (fallback) | 3000000 (us) | [ena_com.c:12] |
| ENA_MAX_PHYS_ADDR_SIZE_BITS | 48 | [ena_com.h:17] |
| ENA_REGS_ADMIN_INTR_MASK | 1 | [ena_com.c:35] |
| MIN_ENA_CTRL_VER | 0.0.1 (0x000001) | [ena_com.c:17-26] |
| RESET/ADMIN_CMD timeout register unit | 100 ms | [ena_com.c:998-999, 2594] |

## Notes / open ambiguities
- CAPS_EXT (0x0c), ACQ_TAIL (0x30), AENQ_TAIL (0x44), RSS_IND_ENTRY_UPDATE
  (0x68): present in the address map but the reference com layer never reads
  or writes them, so their device-side behavior is not derivable from this
  driver. Classify as ambiguous.
- `capabilities` checked by `ena_com_get_cap` (e.g. EXTENDED_RESET_REASONS)
  come from the GET_FEATURE DEVICE_ATTRIBUTES admin response
  [ena_com.c:2313], NOT from the CAPS / CAPS_EXT registers. Do not conflate
  the register CAPS field with the admin capabilities bitmap.
- The 0x18 AQ_CAPS / 0x20 ACQ_BASE_LO gap leaves 0x1c unassigned; the header
  has no name for it.
