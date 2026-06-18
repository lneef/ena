# AENQ — Asynchronous Event Notification Queue

The AENQ is a device-to-host ring through which the ENA device pushes
asynchronous events (link change, keep-alive, fatal error, hints, reset
requests, etc.). It is a pure DMA-write ring owned by the device; the host
only reads entries and acks them with a single head doorbell. The AENQ shares
MSI-X vector 0 with the admin completion queue.

Unless noted, evidence is from the Linux kernel driver
(`kernel/linux/...`); the DPDK driver (`userspace/dpdk/...`) behaves
identically at the device boundary and is cited where it confirms semantics.

## 1. Entry layout

`struct ena_admin_aenq_entry` is **64 bytes**: a 16-byte common descriptor
followed by 48 bytes (`u32[12]`) of group-specific inline payload.
[kernel/linux/common/ena_com/ena_admin_defs.h:1370-1375]

```c
struct ena_admin_aenq_common_desc {
    u16 group;          /* enum ena_admin_aenq_group */
    u16 syndrome;       /* group-specific sub-code */
    u8  flags;          /* bit0 = phase; bits 7:1 reserved MBZ */
    u8  reserved1[3];
    u32 timestamp_low;
    u32 timestamp_high; /* 64-bit timestamp = high<<32 | low */
};                      /* 16 bytes */

struct ena_admin_aenq_entry {
    struct ena_admin_aenq_common_desc aenq_common_desc;
    u32 inline_data_w4[12];   /* 48 bytes group-specific payload */
};
```
[kernel/linux/common/ena_com/ena_admin_defs.h:1336-1375]

Phase bit mask: `ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK = BIT(0)` of `flags`.
[kernel/linux/common/ena_com/ena_admin_defs.h:1545-1546]

The driver reads `timestamp` as `(u64)timestamp_low | ((u64)timestamp_high << 32)`.
[kernel/linux/common/ena_com/ena_com.c:2469-2472]

### Group-specific payload structs

All payload structs start with the same 16-byte `aenq_common_desc`, so the
payload begins at byte offset 16.

Link change — `struct ena_admin_aenq_link_change_desc`:
```c
struct ena_admin_aenq_link_change_desc {
    struct ena_admin_aenq_common_desc aenq_common_desc;
    u32 flags;          /* bit0 = link_status (1 = up) */
};
```
`ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK = BIT(0)`.
[kernel/linux/common/ena_com/ena_admin_defs.h:1377-1382, 1548-1549]

Keep-alive — `struct ena_admin_aenq_keep_alive_desc` (carries cumulative
device drop counters, accumulated since last reset):
```c
struct ena_admin_aenq_keep_alive_desc {
    struct ena_admin_aenq_common_desc aenq_common_desc;
    u32 rx_drops_low;
    u32 rx_drops_high;
    u32 tx_drops_low;
    u32 tx_drops_high;
    u32 rx_overruns_low;
    u32 rx_overruns_high;
};
```
[kernel/linux/common/ena_com/ena_admin_defs.h:1384-1398]
Driver reassembles each counter as `high<<32 | low` and stores them as
cumulative stats. [kernel/linux/ena/ena_netdev.c:6058-6072]

Notification (group `NOTIFICATION`, syndrome `UPDATE_HINTS`) carries
`struct ena_admin_ena_hw_hints` overlaid on `inline_data_w4`:
```c
struct ena_admin_ena_hw_hints {
    u16 mmio_read_timeout;          /* ms */
    u16 driver_watchdog_timeout;    /* ms; 0xFFFF = no timeout */
    u16 missing_tx_completion_timeout; /* ms */
    u16 missed_tx_completion_count_threshold_to_reset;
    u16 admin_completion_tx_timeout;/* ms */
    u16 netdev_wd_timeout;          /* ms */
    u16 max_tx_sgl_size;
    u16 max_rx_sgl_size;
    u16 reserved[8];
};
```
[kernel/linux/common/ena_com/ena_admin_defs.h:1157-1179],
consumed at [kernel/linux/ena/ena_netdev.c:6086-6091, 5081-5118]

Configuration notifications — `struct ena_admin_aenq_conf_notifications_desc`:
```c
struct ena_admin_aenq_conf_notifications_desc {
    struct ena_admin_aenq_common_desc aenq_common_desc;
    u64 notifications_bitmap;   /* each set bit = a sub-optimal config code */
    u64 reserved;
};
```
[kernel/linux/common/ena_com/ena_admin_defs.h:1400-1406],
consumed (bit N reported as code N+1) at [kernel/linux/ena/ena_netdev.c:6109-6131]

## 2. AENQ groups and syndromes

`enum ena_admin_aenq_group` [kernel/linux/common/ena_com/ena_admin_defs.h:1353-1364]:

| Value | Group | Driver handler / meaning |
|-------|-------|--------------------------|
| 0 | `ENA_ADMIN_LINK_CHANGE` | link up/down; `flags` bit0 = link status |
| 1 | `ENA_ADMIN_FATAL_ERROR` | (no specific handler; unimplemented) |
| 2 | `ENA_ADMIN_WARNING` | (no specific handler; unimplemented) |
| 3 | `ENA_ADMIN_NOTIFICATION` | HW hints update (syndrome `UPDATE_HINTS`) |
| 4 | `ENA_ADMIN_KEEP_ALIVE` | device heartbeat + drop counters |
| 5 | `ENA_ADMIN_REFRESH_CAPABILITIES` | driver triggers reset to re-read caps |
| 6 | `ENA_ADMIN_CONF_NOTIFICATIONS` | sub-optimal config bitmap |
| 7 | `ENA_ADMIN_DEVICE_REQUEST_RESET` | device asks driver to reset |
| 8 | `ENA_ADMIN_AENQ_GROUPS_NUM` | count (not a group) |

`enum ena_admin_aenq_notification_syndrome`: `ENA_ADMIN_UPDATE_HINTS = 2`.
[kernel/linux/common/ena_com/ena_admin_defs.h:1366-1368]

Handler registration table (which groups the Linux driver actually acts on):
LINK_CHANGE, NOTIFICATION, KEEP_ALIVE, CONF_NOTIFICATIONS,
DEVICE_REQUEST_RESET, REFRESH_CAPABILITIES. FATAL_ERROR and WARNING fall to
the `unimplemented_handler`. [kernel/linux/ena/ena_netdev.c:6154-6164]

Handler dispatch: `group` is used as an index into a 256-entry handler array
(`ENA_MAX_HANDLERS = 256`); unknown/unhandled groups use the unimplemented
handler. [kernel/linux/common/ena_com/ena_com.c:2429-2438],
[kernel/linux/common/ena_com/ena_com.h:36, 478-480]

## 3. Registers and setup

Register offsets (BAR0) [kernel/linux/common/ena_com/ena_regs_defs.h:49-53]:

| Offset | Register | Direction | Purpose |
|--------|----------|-----------|---------|
| 0x34 | `AENQ_CAPS` | host write | depth (bits 15:0), entry size in bytes (bits 31:16) |
| 0x38 | `AENQ_BASE_LO` | host write | ring DMA base, low 32 bits |
| 0x3c | `AENQ_BASE_HI` | host write | ring DMA base, high 32 bits |
| 0x40 | `AENQ_HEAD_DB` | host write | head doorbell (ack: number of consumed entries) |
| 0x44 | `AENQ_TAIL` | (device) | producer tail index |
| 0x4c | `INTR_MASK` | host write | admin/AENQ interrupt mask (see §6) |

`AENQ_CAPS` field masks [kernel/linux/common/ena_com/ena_regs_defs.h:100-103]:
- `AENQ_DEPTH_MASK = 0xffff`
- `AENQ_ENTRY_SIZE_SHIFT = 16`, `AENQ_ENTRY_SIZE_MASK = 0xffff0000`

### Setup sequence (`ena_com_admin_init_aenq`)
[kernel/linux/common/ena_com/ena_com.c:137-177]

1. Allocate a DMA-coherent, zeroed ring of `ENA_ASYNC_QUEUE_DEPTH = 16`
   entries; size = `depth * sizeof(ena_admin_aenq_entry)` = 16 * 64 = 1024
   bytes. [ena_com.c:144-146, 14], [ena_com.h:45]
2. Initialise host state: `head = q_depth (=16)`, `phase = 1`.
   [ena_com.c:153-154]
3. Write `AENQ_BASE_LO` then `AENQ_BASE_HI` with the ring physical address.
   [ena_com.c:156-160]
4. Write `AENQ_CAPS` = `FIELD_PREP(DEPTH, 16) | FIELD_PREP(ENTRY_SIZE, sizeof(entry)=64)`.
   [ena_com.c:162-167]

Note: depth `16` is a fixed driver constant, not negotiated from the device.

### Enable / initial doorbell (`ena_com_admin_aenq_enable`)
[kernel/linux/common/ena_com/ena_com.c:1597-1607]

After admin IRQ is requested and polling disabled, the driver writes
`AENQ_HEAD_DB = depth (=16)` to publish that all 16 entries are initially
available to the device. It asserts `head == depth` before doing so.
Call site ordering: request mgmt IRQ -> disable admin polling ->
`ena_com_admin_aenq_enable`. [kernel/linux/ena/ena_netdev.c:4481-4489]

## 4. Phase bit and consumption protocol

The phase bit is the classic toggling-ownership scheme.

- Host keeps `head` (free-running, never masked in software state) and
  `phase` (starts at 1).
- An entry is "ready" when `entry.flags & PHASE_MASK == driver phase`.
  [kernel/linux/common/ena_com/ena_com.c:2459]
- The device writes the whole entry (payload first), then publishes it by
  writing the `flags`/phase last; the driver issues `dma_rmb()` after seeing
  a matching phase before reading payload. [ena_com.c:2459-2467]

Consumption loop (`ena_com_aenq_intr_handler`)
[kernel/linux/common/ena_com/ena_com.c:2444-2504]:

1. `masked_head = head & (q_depth - 1)`; `phase = aenq->phase`.
2. While `entry.flags & PHASE_MASK == phase`:
   - `dma_rmb()`, dispatch handler by `group`,
   - `masked_head++`, `processed++`,
   - when `masked_head == q_depth`: wrap to 0 and **flip phase**.
3. `head += processed`; store new `phase`.
4. If nothing processed, return without touching the doorbell.
5. `mb()`, then write `AENQ_HEAD_DB = (u32)head` to ack all consumed entries.
   [ena_com.c:2491-2503]

So the head doorbell value is the cumulative free-running `head` (after init
it is `16`, and grows by the number of entries consumed). The phase flips
every full wrap of the 16-entry ring. The device must compare its written
phase bit against the host's expected phase; the host's expected phase toggles
each ring wrap.

`ena_com_aenq_has_keep_alive` scans the same ready entries (same phase logic)
without consuming/acking, only to test whether a KEEP_ALIVE event is pending —
used to refine the watchdog reset reason. [ena_com.c:2506-2545]

## 5. AENQ_CONFIG feature negotiation

`ENA_ADMIN_AENQ_CONFIG = 26` feature.
[kernel/linux/common/ena_com/ena_admin_defs.h:69]

`struct ena_admin_feature_aenq_desc` carries two bitmasks (bit positions =
`enum ena_admin_aenq_group` values):
```c
struct ena_admin_feature_aenq_desc {
    u32 supported_groups;  /* groups the device can report (GET) */
    u32 enabled_groups;    /* groups to report (SET) */
};
```
[kernel/linux/common/ena_com/ena_admin_defs.h:936-942]
Present in both the GET response union (`get_feat_resp.u.aenq`) and SET command
union (`set_feat_cmd.u.aenq`).
[kernel/linux/common/ena_com/ena_admin_defs.h:1255, 1297-1298]

Negotiation flow:
- During init the driver GETs `AENQ_CONFIG` and copies `supported_groups`
  into the per-device feature context. [kernel/linux/ena/ena_netdev.c:2380-2385]
  (via `ena_com_get_feature(..., ENA_ADMIN_AENQ_CONFIG, 0)`).
- Driver builds a desired mask (LINK_CHANGE, FATAL_ERROR, WARNING,
  NOTIFICATION, KEEP_ALIVE, CONF_NOTIFICATIONS, DEVICE_REQUEST_RESET),
  ANDs it with `supported_groups`, and calls
  `ena_com_set_aenq_config(ena_dev, aenq_groups)`.
  [kernel/linux/ena/ena_netdev.c:4399-4410]
- `ena_com_set_aenq_config` re-GETs the feature, rejects with `-EOPNOTSUPP`
  if any requested group is not in `supported_groups`, then issues
  `SET_FEATURE(AENQ_CONFIG)` with `enabled_groups = groups_flag`.
  [kernel/linux/common/ena_com/ena_com.c:1609-1648]
- Keep-alive watchdog is enabled iff KEEP_ALIVE survived the AND:
  `wd_state = !!(aenq_groups & BIT(ENA_ADMIN_KEEP_ALIVE))`.
  [kernel/linux/ena/ena_netdev.c:4416]

Device expectation: the device must only emit AENQ events for groups present
in the most recent `enabled_groups`, and must advertise the full set it can
ever produce in `supported_groups`.

## 6. Interrupt delivery and INTR_MASK interaction

- The AENQ and the admin completion queue share **MSI-X vector 0**
  (`ENA_MGMNT_IRQ_IDX = 0`); IO queues use vectors 1..N
  (`ENA_IO_IRQ_FIRST_IDX = 1`).
  [kernel/linux/ena/ena_netdev.h:121-123]
- The single management ISR `ena_intr_msix_mgmnt` first runs the admin
  completion handler, then (once probe done) the AENQ handler with the
  adapter as `data`. [kernel/linux/ena/ena_netdev.c:1983-1994]
- `INTR_MASK` (0x4c) gates the management interrupt:
  `ena_com_set_admin_polling_mode(polling)` writes
  `ENA_REGS_ADMIN_INTR_MASK = 1` to mask when polling, `0` to unmask
  (interrupt-driven). [kernel/linux/common/ena_com/ena_com.c:35, 1756-1765]
  So writing `INTR_MASK = 0` enables device interrupts on vector 0;
  `INTR_MASK = 1` masks them and the driver polls the admin CQ / AENQ instead.
- During init the driver runs admin in polling mode (mask set), then after
  registering the mgmt IRQ switches to interrupt mode
  (`ena_com_set_admin_polling_mode(ena_dev, false)`) and only then enables the
  AENQ doorbell. [kernel/linux/ena/ena_netdev.c:4487-4489]
- The AENQ ack doorbell (`AENQ_HEAD_DB`, §4) is independent of `INTR_MASK`:
  acking consumed entries does not re-arm the interrupt; the device is
  expected to raise vector 0 again whenever it advances `AENQ_TAIL` with a new
  ready entry while the interrupt is unmasked.

DPDK confirms the identical device contract: it allocates the AENQ via the
same `ena_com_admin_init`, sets the AENQ config mask, and either registers a
control-path interrupt callback (`ena_com_set_admin_polling_mode(false)`) or
polls the AENQ on a timer. [userspace/dpdk/ena/ena_ethdev.c:1895-1963,
2436-2442]

## 7. Keep-alive semantics and watchdog

- The device periodically emits a KEEP_ALIVE AENQ event; each one carries
  cumulative rx/tx drop and rx-overrun counters (§1).
- Each KEEP_ALIVE updates `last_keep_alive_jiffies = jiffies`.
  [kernel/linux/ena/ena_netdev.c:6058-6059]
- Driver watchdog timeout default: `ENA_DEVICE_KALIVE_TIMEOUT = 6 * HZ`
  (6 seconds). [kernel/linux/ena/ena_netdev.h:130], set at
  [kernel/linux/ena/ena_netdev.c:5742, 4620]
- The timer-service `check_for_missing_keep_alive` runs only if `wd_state`
  is set and `keep_alive_timeout != ENA_HW_HINTS_NO_TIMEOUT (0xFFFF)`. If
  `last_keep_alive_jiffies + keep_alive_timeout` is in the past, it resets the
  device. Reset reason is `ENA_REGS_RESET_KEEP_ALIVE_TO (1)` normally, or
  `ENA_REGS_RESET_MISSING_ADMIN_INTERRUPT (18)` if a KEEP_ALIVE entry is
  actually sitting unconsumed in the AENQ (interrupt was missed).
  [kernel/linux/ena/ena_netdev.c:5041-5065],
  [kernel/linux/common/ena_com/ena_com.h:70]
- The watchdog timeout can be overridden at runtime via a NOTIFICATION /
  UPDATE_HINTS event setting `driver_watchdog_timeout` (ms; `0xFFFF` disables
  the watchdog). [kernel/linux/ena/ena_netdev.c:5110-5115]

Device expectation: while the KEEP_ALIVE group is enabled, send a KEEP_ALIVE
event at an interval comfortably below the host watchdog (default 6 s) or the
host will reset the device. The exact device-side emission interval is not
encoded in the driver (the driver only enforces the timeout), so it is a
device design parameter; the only hard constraint visible from the driver is
"< keep_alive_timeout (default 6 s, or hint-overridden value)".

## Claim classification

- Explicit (register/struct/constant layouts, masks, init writes, consumption
  loop, handler table, INTR_MASK values, 6 s default, depth 16, entry size 64):
  directly from cited code.
- Inferred (device-side obligations: when to raise vector 0, phase publishing
  order, KEEP_ALIVE emission cadence): derived from how the driver reads/acks
  and from the phase/doorbell protocol; the driver never states the device
  algorithm, only its own consumer side. The KEEP_ALIVE cadence is bounded but
  not exactly specified by the driver.
- Ambiguous: FATAL_ERROR and WARNING groups are negotiated/enabled by the
  driver but have no dedicated Linux handler (fall through to unimplemented),
  so their precise device payloads are not exercised in this reference.
