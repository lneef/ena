# Health checks, liveness, and statistics

How the reference driver decides the ENA device is unhealthy, and what it
expects the device to emit for liveness/stats. From the device's point of
view this page tells you: what counters/events you must produce on time, and
what the driver does (resets) if you don't.

Cross-references (do not duplicate):
- Keep-alive AENQ payload, hint-driven watchdog override, `ena_com_aenq_has_keep_alive`: [aenq.md](aenq.md)
- DEV_CTL/DEV_STS reset handshake, reset-reason encoding + full enum value table: [registers.md](registers.md)
- RX-path reset reasons (INV_RX_REQ_ID, TOO_MANY_RX_DESCS, RX_DESCRIPTOR_MALFORMED): [rx-path.md](rx-path.md), [rx-descriptors.md](rx-descriptors.md)
- Admin command submission/completion mechanics: [admin-queue.md](admin-queue.md)

Sources: `userspace/dpdk/ena/` (DPDK PMD) and `kernel/linux/ena/` +
`kernel/linux/common/ena_com/` (richer watchdog). Paths below are relative to
`~/ena/amzn-drivers/`.

---

## 1. Periodic health/timer service

Both drivers run a periodic timer that calls a fixed sequence of checks; any
check that fails records a reset reason and triggers the reset flow (see
[registers.md](registers.md) for the DEV_CTL reset handshake).

### DPDK timer service `ena_timer_wd_callback`
Period: 1 second (`rte_timer_reset(..., ticks, PERIODICAL, ...)` with
`ticks = rte_get_timer_hz()`). [userspace/dpdk/ena/ena_ethdev.c:1342-1344]

Sequence, in order, skipped entirely if `adapter->trigger_reset` is already
set: [userspace/dpdk/ena/ena_ethdev.c:2077-2095]
1. `check_for_missing_keep_alive` — keep-alive watchdog.
2. `check_for_admin_com_state` — admin queue running-state.
3. `check_for_tx_completions` — missing TX completion scan.

After the checks, if `trigger_reset` is set it fires
`RTE_ETH_EVENT_INTR_RESET`. [userspace/dpdk/ena/ena_ethdev.c:2090-2094]

### Kernel timer service `ena_timer_service`
Period: 1 second, re-armed with `mod_timer(&adapter->timer_service,
round_jiffies(jiffies + HZ))`. [kernel/linux/ena/ena_netdev.c:4619, 5247, 5256, 5754]

Sequence: [kernel/linux/ena/ena_netdev.c:5226-5232]
1. `check_for_missing_keep_alive`
2. `check_for_admin_com_state`
3. `check_for_missing_completions` (also runs `check_for_rx_interrupt_queue`
   per monitored queue)
4. `check_for_empty_rx_ring` (re-schedules NAPI to refill RX; not a reset path)

### Keep-alive timeout (explicit)
- DPDK default: `ENA_DEVICE_KALIVE_TIMEOUT = ENA_WD_TIMEOUT_SEC (3) * rte_get_timer_hz()`,
  i.e. **3 s**. [userspace/dpdk/ena/ena_ethdev.h:39-40], assigned at
  [userspace/dpdk/ena/ena_ethdev.c:1340]
- Kernel default: `ENA_DEVICE_KALIVE_TIMEOUT = 6 * HZ`, i.e. **6 s**.
  [kernel/linux/ena/ena_netdev.h:130], assigned at [kernel/linux/ena/ena_netdev.c:5742]
- The two drivers disagree on the default; the device should satisfy the
  stricter 3 s (DPDK) to be safe. Both can be overridden by a hint (see aenq.md).
- DPDK check: enabled only if KEEP_ALIVE is in `active_aenq_groups` and
  `keep_alive_timeout != ENA_HW_HINTS_NO_TIMEOUT`. On
  `now - timestamp_wd >= keep_alive_timeout`: reset reason
  **ENA_REGS_RESET_KEEP_ALIVE_TO (1)**, increments `wd_expired`.
  [userspace/dpdk/ena/ena_ethdev.c:1971-1985]
- Kernel check: gated on `wd_state` and timeout != NO_TIMEOUT. On expiry it
  normally uses **KEEP_ALIVE_TO (1)**, but if `ena_com_aenq_has_keep_alive`
  finds an unprocessed keep-alive still sitting in the AENQ ring it instead
  resets with **ENA_REGS_RESET_MISSING_ADMIN_INTERRUPT (18)** (the event was
  delivered but no MSI-X arrived). [kernel/linux/ena/ena_netdev.c:5041-5065]

### Admin-queue running-state check (explicit)
- DPDK: if `!ena_com_get_admin_running_state` -> reset
  **ENA_REGS_RESET_ADMIN_TO (2)**. [userspace/dpdk/ena/ena_ethdev.c:1987-1994]
- Kernel: same condition; resets with **MISSING_ADMIN_INTERRUPT (18)** if
  `ena_com_get_missing_admin_interrupt` is true, else **ADMIN_TO (2)**; also
  bumps `admin_q_pause`. [kernel/linux/ena/ena_netdev.c:5067-5078]
- See section 3 for how `running_state` becomes false.

### Missing-TX-completion scan
A TX buffer records a send timestamp; the scan flags entries whose completion
is overdue and resets once too many are outstanding.

DPDK `check_for_tx_completions` / `check_for_tx_completion_in_queue`:
- Disabled if `missing_tx_completion_to == ENA_HW_HINTS_NO_TIMEOUT`.
  [userspace/dpdk/ena/ena_ethdev.c:2051-2052]
- Budget = `missing_tx_completion_budget = RTE_MIN(ENA_MONITORED_TX_QUEUES (3),
  nb_tx_queues)`; round-robins from `last_tx_comp_qid`.
  [userspace/dpdk/ena/ena_ethdev.c:2055-2074, 2499-2500]
- Per buffer: `completion_delay = now - tx_buf->timestamp`; counted as missed
  when `> missing_tx_completion_to`. Default
  `missing_tx_completion_to = ENA_TX_TIMEOUT = 5 * rte_get_timer_hz()` (**5 s**).
  [userspace/dpdk/ena/ena_ethdev.c:2008-2024, 2332]; const
  [userspace/dpdk/ena/ena_ethdev.h:42]
- Per-queue threshold: `missing_tx_completion_threshold =
  RTE_MIN(ring_size/2, ENA_DEFAULT_MISSING_COMP (256))`.
  [userspace/dpdk/ena/ena_ethdev.c:1636-1637]; const
  [userspace/dpdk/ena/ena_ethdev.h:45]
- When `missed_tx > threshold`: sets `reset_reason =
  ENA_REGS_RESET_MISS_TX_CMPL (3)` and `trigger_reset = true`.
  [userspace/dpdk/ena/ena_ethdev.c:2027-2036]
- A queue is only scanned if its last cleanup was recent
  (`tx_cleanup_delay < tx_cleanup_stall_delay`, where
  `tx_cleanup_stall_delay = missing_tx_completion_to / 2`) to avoid false
  positives when the app simply stopped polling.
  [userspace/dpdk/ena/ena_ethdev.c:2067-2070, 2508]
- User cap: TX timeout configurable up to `ENA_MAX_TX_TIMEOUT_SECONDS = 60`;
  value 0 disables the check (`ENA_HW_HINTS_NO_TIMEOUT`).
  [userspace/dpdk/ena/ena_ethdev.c:3714-3729]

Kernel `check_for_missing_completions` / `check_missing_comp_in_tx_queue`
(richer): [kernel/linux/ena/ena_netdev.c:4779-4938, 4940-4980]
- Budget = `min(num_io_queues, ENA_MONITORED_QUEUES (4))`, round-robin from
  `last_monitored_qid`. [kernel/linux/ena/ena_netdev.c:4958-4979]; const
  [kernel/linux/ena/ena_netdev.h:106]
- Defaults: `missing_tx_completion_to_jiffies = TX_TIMEOUT`,
  `missing_tx_completion_threshold = MAX_NUM_OF_TIMEOUTED_PACKETS = 128`.
  [kernel/linux/ena/ena_netdev.c:5743-5744]; const
  [kernel/linux/ena/ena_netdev.h:108]
- Per buffer uses `tx_buf->tx_sent_jiffies`; `timeout = sent + miss_to`,
  `graceful_timeout = timeout + miss_to` (a second window).
  [kernel/linux/ena/ena_netdev.c:4803-4804]
- Distinguishes several reset reasons depending on interrupt/NAPI state:
  - **MISS_TX_CMPL (3)** — default when threshold exceeded.
  - **ENA_REGS_RESET_SUSPECTED_POLL_STARVATION (15)** — overdue but NAPI is
    scheduled and not running (bottom half starved). [kernel/linux/ena/ena_netdev.c:4829-4837]
  - **MISS_FIRST_INTERRUPT (20)** — `last_intr_jiffies == 0` (queue never got
    its first MSI-X) and graceful window expired; selected via
    `check_cdesc_in_tx_cq`. [kernel/linux/ena/ena_netdev.c:4807-4818, 4910-4928]
- On failure it sets `ENA_FLAG_TRIGGER_RESET` then `ena_reset_device(reason)`.
  [kernel/linux/ena/ena_netdev.c:4916-4932]

Inferred (device implication): the device must produce a TX completion for
every accepted TX descriptor within these windows, and must deliver an MSI-X
for the first completion of a freshly created queue; otherwise the driver
resets. The exact reasons above are diagnostic only — all of them lead to the
same DEV_CTL reset write.

---

## 2. Reset-reason enum (canonical table)

The canonical `enum ena_regs_reset_reason_types` value table and the DEV_CTL
encoding (legacy 4-bit RESET_REASON field bits[31:28] plus
RESET_REASON_EXT bits[27:24] for values 16..20, gated on the
EXTENDED_RESET_REASONS capability) live in
[registers.md](registers.md#reset-reason-encoding-enum-values).
Definition: [userspace/dpdk/ena/base/ena_defs/ena_regs_defs.h:8-31].

Which subsystem raises which reason (this page's scope):

| Reason | Value | Raised by | Citation |
|--------|-------|-----------|----------|
| KEEP_ALIVE_TO | 1 | keep-alive watchdog expiry | dpdk ena_ethdev.c:1982 / kernel ena_netdev.c:5063 |
| ADMIN_TO | 2 | admin queue not running | dpdk ena_ethdev.c:1992 / kernel ena_netdev.c:5077 |
| MISS_TX_CMPL | 3 | missing TX completion scan | dpdk ena_ethdev.c:2033 / kernel ena_netdev.c:4783 |
| SUSPECTED_POLL_STARVATION | 15 | TX overdue + NAPI starved (kernel) | kernel ena_netdev.c:4836 |
| MISSING_ADMIN_INTERRUPT | 18 | admin/keep-alive present but no MSI-X (kernel) | kernel ena_netdev.c:5061, 5075 |
| DEVICE_REQUEST | 19 | DEVICE_REQUEST_RESET AENQ (kernel) | kernel ena_netdev.c:6141 |
| MISS_FIRST_INTERRUPT | 20 | first MSI-X never seen (kernel) | kernel ena_netdev.c:4695, 4928 |
| GENERIC | 13 | misc control-path failures (e.g. failed alarm re-arm) | dpdk ena_ethdev.c:1966 |

RX-path reasons (INV_RX_REQ_ID 4, TOO_MANY_RX_DESCS 6,
RX_DESCRIPTOR_MALFORMED 16) and INV_TX_REQ_ID (5) / TX_DESCRIPTOR_MALFORMED
(17) are raised on the data path — see [rx-path.md](rx-path.md) and
[tx-path.md](tx-path.md). OS/shutdown/user reasons (8,9,10,11,12) are host-side.

---

## 3. Admin-queue health (dead-queue detection)

The driver does not poll a DEV_STS "admin dead" bit; instead `running_state`
is a software flag in `struct ena_com_admin_queue` that becomes false on any
admin failure, and the timer service (section 1) converts that into a reset.

`running_state` semantics: [userspace/dpdk/ena/base/ena_com.c:1542-1553]
- `ena_com_get_admin_running_state` returns the flag; the timer-service
  admin check reads it.
- Set true at admin init. [userspace/dpdk/ena/base/ena_com.c:2152]

Where it is cleared (every path means "stop trusting the admin queue"):
- Submit while already not running: returns NO_DEVICE without clearing again.
  [userspace/dpdk/ena/base/ena_com.c:313-317]
- Submit fails (queue full / error from `__ena_com_submit_admin_cmd`):
  cleared. [userspace/dpdk/ena/base/ena_com.c:322-323]
- Completion handling sees a bad/unexpected completion: cleared.
  [userspace/dpdk/ena/base/ena_com.c:472]
- **Command completion timeout** — polling path: after
  `completion_timeout` with `comp_ctx->status` still `ENA_CMD_SUBMITTED`, logs
  "Wait for completion (polling) timeout", bumps `stats.no_completion`, clears
  `running_state`, returns `ENA_COM_TIMER_EXPIRED`.
  [userspace/dpdk/ena/base/ena_com.c:560-608] (interrupt path mirrors this at
  [userspace/dpdk/ena/base/ena_com.c:779-820])
- `completion_timeout` default = `ADMIN_CMD_TIMEOUT_US = 3000000` (3 s).
  [userspace/dpdk/ena/base/ena_com.c:12, 2597]

So from the device side: if you accept an admin command (advance the SQ) but
never post its ACQ completion within 3 s, the driver flags the admin queue
dead and resets with **ADMIN_TO (2)** at the next timer tick.

`ena_com_admin_q_comp_intr_handler` (kernel) is the MSI-X-driven counterpart
that processes ACQ completions; missing that interrupt is what
`ena_com_get_missing_admin_interrupt` / `is_missing_admin_interrupt` detect to
upgrade the reason to **MISSING_ADMIN_INTERRUPT (18)**.
[kernel/linux/ena/ena_netdev.c:1987; kernel/linux/common/ena_com/ena_com.c:805, 2191, 2421]

DEV_STS FATAL_ERROR bit: `ENA_REGS_DEV_STS_FATAL_ERROR_MASK = 0x20`
(bit 5) exists in the register map
[userspace/dpdk/ena/base/ena_defs/ena_regs_defs.h:127-128] but **neither
driver reads it during the health/timer service** — only the
RESET_IN_PROGRESS/RESET_FINISHED handshake bits are polled during reset (see
[registers.md](registers.md)). Classify the device's use of the FATAL bit as
ambiguous/ignored by the reference driver.

---

## 4. GET_STATS admin command (opcode 11)

`ENA_ADMIN_GET_STATS = 11`. [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:36]

### Request `struct ena_admin_aq_get_stats_cmd`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:400-428]
```
struct ena_admin_aq_get_stats_cmd {
    struct ena_admin_aq_common_desc aq_common_descriptor;
    union {
        u32 inline_data_w1[3];
        struct ena_admin_ctrl_buff_info control_buffer; /* addr+length */
    } u;
    u8  type;        /* enum ena_admin_get_stats_type */
    u8  scope;       /* enum ena_admin_get_stats_scope */
    u16 reserved3;
    u16 queue_idx;   /* used when scope == SPECIFIC_QUEUE */
    u16 device_id;   /* 0xFFFF == "mine"; privileged only for others */
    u64 requested_metrics; /* bitmap, customer-metrics only */
};
```
`enum ena_admin_get_stats_type`: BASIC=0, EXTENDED=1, ENI=2, ENA_SRD=3,
CUSTOMER_METRICS=4. [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:132-141]
`enum ena_admin_get_stats_scope`: SPECIFIC_QUEUE=0, ETH_TRAFFIC=1.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:143-146]

How the reference fills it: `ena_get_dev_stats` zeroes the whole ctx and sets
only `opcode`, `flags=0`, and `type`. [userspace/dpdk/ena/base/ena_com.c:2249-2274]
Therefore (explicit, by omission + memset at callers
[userspace/dpdk/ena/base/ena_com.c:2648, 2288, 2679; ena_ethdev.c:4174-4179]):
- `scope = 0` (SPECIFIC_QUEUE encoding, but left as the zero default).
- `queue_idx = 0`.
- `device_id = 0` — **not** 0xFFFF. There is no `ENA_ADMIN_DEVICE_ID_ANY`
  symbol anywhere in the DPDK or kernel `ena_com` (grep: absent). The device
  must treat `device_id = 0` as "this function / mine".
  Ambiguity: the struct comment ("0xFFFF == mine") suggests 0xFFFF is the
  documented self-reference value, yet the drivers only ever send 0 — an
  emulated device should accept both 0 and 0xFFFF as "mine".
- `requested_metrics` only set for CUSTOMER_METRICS.
  [userspace/dpdk/ena/base/ena_com.c:2289, 2689]

Inferred: for BASIC/ENI/ENA_SRD the device returns whole-device counters and
must ignore scope/queue_idx (they are left zero, never populated). Only the
customer-metrics path sets a control buffer + `requested_metrics`.

### Response `struct ena_admin_acq_get_stats_resp`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:522-536] — `acq_common_desc`
followed by a union (`u64 raw[7]`) overlaying the stats structs.

`struct ena_admin_basic_stats` (14 x u32, lo/hi 64-bit pairs):
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:431-459]
```
tx_bytes_low/high, tx_pkts_low/high,
rx_bytes_low/high, rx_pkts_low/high,
rx_drops_low/high, tx_drops_low/high,
rx_overruns_low/high
```
Copied verbatim by `ena_com_get_dev_basic_stats`.
[userspace/dpdk/ena/base/ena_com.c:2642-2655]

`struct ena_admin_eni_stats` (5 x u64): bw_in_allowance_exceeded,
bw_out_allowance_exceeded, pps_allowance_exceeded,
conntrack_allowance_exceeded, linklocal_allowance_exceeded.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:462-486]; fetched by
`ena_com_get_eni_stats` with type=ENI. [userspace/dpdk/ena/base/ena_com.c:2602-2620]

`struct ena_admin_ena_srd_info` = `u64 flags`
(`ena_admin_ena_srd_flags`) + `struct ena_admin_ena_srd_stats`
(ena_srd_tx_pkts, ena_srd_eligible_tx_pkts, ena_srd_rx_pkts,
ena_srd_resource_utilization). [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:488-512];
`ena_com_get_ena_srd_info` type=ENA_SRD. [userspace/dpdk/ena/base/ena_com.c:2622-2640]

`struct ena_admin_customer_metrics` = `u64 reported_metrics` bitmap.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:515-520]

### Customer metrics path (DPDK uses it)
- Capability gate: `ena_com_get_cap(ENA_ADMIN_CUSTOMER_METRICS)`.
  [userspace/dpdk/ena/base/ena_com.c:2283, 2668]
- A dedicated DMA buffer is allocated once: `buffer_len =
  ENA_CUSTOMER_METRICS_BUFFER_SIZE`. [userspace/dpdk/ena/base/ena_com.c:3242-3256]
- Supported-metrics negotiation: at init, sends GET_STATS type=CUSTOMER_METRICS
  with `requested_metrics = ENA_ADMIN_CUSTOMER_METRICS_SUPPORT_MASK`; the
  device replies `u.customer_metrics.reported_metrics`, stored as
  `supported_metrics`. [userspace/dpdk/ena/base/ena_com.c:2276-2294, 2380]
- Fetch: `ena_com_get_customer_metrics` sets `u.control_buffer.address/length`
  to the DMA buffer, `requested_metrics = supported_metrics`, issues GET_STATS,
  then `memcpy`s `buffer_len` bytes out of the DMA buffer.
  [userspace/dpdk/ena/base/ena_com.c:2657-2696]
- Metric IDs (`enum ena_admin_customer_metrics_id`): BW_IN/BW_OUT/PPS/
  CONNTRACK_EXCEEDED/LINKLOCAL_EXCEEDED/CONNTRACK_AVAILABLE (0..5).
  [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:20-27]

So for customer metrics the device writes the metric values into the
driver-supplied control buffer (DMA), not into the inline response.

### When the driver fetches stats (explicit)
On demand, from DPDK ethdev callbacks — **not** on the periodic timer:
- `ena_stats_get` (rte basic stats) and the `xstats_get*` family.
  [userspace/dpdk/ena/ena_ethdev.c:264, 334-338]
- `ena_copy_customer_metrics` prefers CUSTOMER_METRICS, falls back to ENI
  stats if only `ENA_ADMIN_ENI_STATS` cap is present.
  [userspace/dpdk/ena/ena_ethdev.c:3315-3356]
- `ena_copy_ena_srd_info` fetches SRD info if `ENA_ADMIN_ENA_SRD_INFO` cap.
  [userspace/dpdk/ena/ena_ethdev.c:3358-3377]
- All wrapped in `adapter->admin_lock`. In multi-process mode the secondary
  proxies the request to the primary (`ENA_MP_*` requests dispatched in
  `ena_mp_primary_handle`). [userspace/dpdk/ena/ena_ethdev.c:4172-4204]

Inferred: there is no fixed polling cadence for GET_STATS; frequency is
whatever the application calls `rte_eth_stats_get` / `rte_eth_xstats_get`.
The only periodic device->driver liveness signal is the keep-alive AENQ
(section 5), not GET_STATS.

---

## 5. Keep-alive (device obligation)

Full payload struct, phase handling, and hint override are in
[aenq.md section 7](aenq.md). Only the health-relevant deltas here:

- Group `ENA_ADMIN_KEEP_ALIVE = 4`. Device must emit a KEEP_ALIVE AENQ entry
  periodically; the driver records receipt as `timestamp_wd` (DPDK) /
  `last_keep_alive_jiffies` (kernel) and the watchdog (section 1) resets if the
  gap exceeds `keep_alive_timeout`.
  [userspace/dpdk/ena/ena_ethdev.c:4082-4105; kernel/linux/ena/ena_netdev.c:6051-6073]
- Required cadence (inferred from defaults): comfortably under **3 s** to
  satisfy DPDK (kernel allows 6 s). Both overridable via UPDATE_HINTS
  `driver_watchdog_timeout` (ms; 0xFFFF disables). [aenq.md]
- Payload counters the device fills (`struct ena_admin_aenq_keep_alive_desc`,
  lo/hi 64-bit pairs): `rx_drops`, `tx_drops`, `rx_overruns`. These are
  cumulative since last device reset. [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1239-1253]
- DPDK consumes them as `drv_stats->rx_drops = rx_drops + rx_overruns` and
  `dev_stats.tx_drops = tx_drops` (note: rx_overruns folded into rx_drops).
  [userspace/dpdk/ena/ena_ethdev.c:4095-4104]
- Kernel stores all three separately in `dev_stats.ka_stats`.
  [kernel/linux/ena/ena_netdev.c:6061-6072]

---

## 6. FATAL_ERROR, WARNING, DEVICE_REQUEST_RESET, REFRESH_CAPABILITIES groups

AENQ group enum: LINK_CHANGE=0, FATAL_ERROR=1, WARNING=2, NOTIFICATION=3,
KEEP_ALIVE=4, REFRESH_CAPABILITIES=5, CONF_NOTIFICATIONS=6,
DEVICE_REQUEST_RESET=7. [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1209-1219]

### DPDK
Registers (`aenq_groups` AND-ed with device `supported_groups`):
LINK_CHANGE, NOTIFICATION, KEEP_ALIVE, FATAL_ERROR, WARNING,
CONF_NOTIFICATIONS. [userspace/dpdk/ena/ena_ethdev.c:1918-1925]
- It does **not** register REFRESH_CAPABILITIES or DEVICE_REQUEST_RESET.
- Handler table only fills LINK_CHANGE, NOTIFICATION, KEEP_ALIVE,
  CONF_NOTIFICATIONS. [userspace/dpdk/ena/ena_ethdev.c:4133-4141]
- **FATAL_ERROR and WARNING are requested but have no handler** — they fall
  through to `unimplemented_aenq_handler`, which only logs "Unknown event ...".
  So DPDK takes no action (no reset) on FATAL_ERROR/WARNING.
  [userspace/dpdk/ena/ena_ethdev.c:4126-4131]
- NOTIFICATION only acts on syndrome `UPDATE_HINTS` (2); other syndromes log
  an error. [userspace/dpdk/ena/ena_ethdev.c:4058-4080]
- CONF_NOTIFICATIONS: logs each set bit as a "sub-optimal configuration"
  warning; no reset. [userspace/dpdk/ena/ena_ethdev.c:4107-4121]

### Kernel (richer)
Registers: ... | BIT(FATAL_ERROR) | ... | BIT(DEVICE_REQUEST_RESET).
[kernel/linux/ena/ena_netdev.c:4401-4406]
- `ena_admin_device_request_reset` (DEVICE_REQUEST_RESET): logs "device has
  detected an unhealthy state" and `ena_reset_device(ENA_REGS_RESET_DEVICE_REQUEST (19))`.
  [kernel/linux/ena/ena_netdev.c:6133-6142, 6160]
- `ena_refresh_fw_capabilites` (REFRESH_CAPABILITIES): sets
  `ENA_FLAG_TRIGGER_RESET` (reset to re-read capabilities; no explicit reason
  arg here). [kernel/linux/ena/ena_netdev.c:6099-6107]
- `ena_keep_alive_wd`, `ena_notification`, `ena_conf_notification` as above.

Inferred device guidance: DEVICE_REQUEST_RESET is the device's clean way to
ask the host to reset (kernel honors it; DPDK does not subscribe). FATAL_ERROR
/ WARNING groups exist but neither reference driver takes corrective action on
them, so their payloads are effectively ignored — do not rely on them to
drive host behavior.

---

## 7. Debug area + host info (HOST_ATTR_CONFIG)

SET_FEATURE `ENA_ADMIN_HOST_ATTR_CONFIG = 28`.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:68]

Command payload `struct ena_admin_set_feature_host_attr_desc`:
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:774-787]
```
struct ena_common_mem_addr os_info_ba;  /* host_info, 4KB phys-contiguous */
struct ena_common_mem_addr debug_ba;    /* debug area, phys-contiguous */
u32 debug_area_size;
```
`ena_com_set_host_attributes` writes `os_info_ba` from `host_info_dma_addr`,
`debug_ba` from `debug_area_dma_addr`, and `debug_area_size`.
[userspace/dpdk/ena/base/ena_com.c:3305-3340]

Driver side:
- `host_info` is filled with OS type/driver version/num_cpus/supported-features
  and pushed via SET_FEATURE. [userspace/dpdk/ena/ena_ethdev.c:801-847]
- Debug area is allocated sized to the xstats string+value table
  (`ss_count * ETH_GSTRING_LEN + sizeof(u64) * ss_count`) and registered via a
  second SET_FEATURE. [userspace/dpdk/ena/ena_ethdev.c:861-890]

What the device writes there: **nothing the reference driver ever reads back.**
The debug area is a device->host scratch/crash region the firmware may populate
for out-of-band debugging, but neither DPDK nor the kernel driver reads or
parses `debug_area_virt_addr` after registering it (only allocate/set/free:
[userspace/dpdk/ena/base/ena_com.c:3223-3301]). Classify the device's debug-area
content as ambiguous / not observable from the driver. Providing the region and
accepting the SET_FEATURE is sufficient; populating it is optional.

---

## 8. Things the device can emit that the reference driver ignores

Explicit (cited):
- **DEV_STS FATAL_ERROR bit (0x20)** — defined but never read by the health
  service or any driver path. §3.
- **AENQ FATAL_ERROR (group 1) / WARNING (group 2)** under DPDK — subscribed
  but routed to `unimplemented_aenq_handler`; logged only, no action. §6.
- **AENQ REFRESH_CAPABILITIES (5) / DEVICE_REQUEST_RESET (7)** under DPDK —
  not subscribed at all; if delivered they hit `unimplemented_aenq_handler`. §6.
- **NOTIFICATION syndromes other than UPDATE_HINTS (2)** — logged as error,
  ignored. §6. [userspace/dpdk/ena/ena_ethdev.c:4076-4079]
- **Debug-area contents** — written region never read back. §7.
- **GET_STATS scope / queue_idx / device_id fields** — drivers leave them
  zero; per-queue and cross-device stats requests are never issued, so any
  device support for them is unexercised. §4.
- **rx_overruns keep-alive counter** under DPDK — not surfaced separately; it
  is folded into `rx_drops`. §5. [userspace/dpdk/ena/ena_ethdev.c:4103]
- **EXTENDED stats type (1)** — enum exists but no reference caller issues
  `ENA_ADMIN_GET_STATS_TYPE_EXTENDED`. §4.

Inferred:
- Reset reasons are diagnostic granularity for the host's own logging; the
  device only ever observes the encoded reason in the DEV_CTL write
  (see registers.md), so emitting fine-grained AENQ error detail beyond
  DEVICE_REQUEST_RESET buys nothing with these drivers.

---

## Claim classification summary
- Explicit (cited to code): timer cadence and ordering; all timeout/threshold
  constants (3 s / 6 s keep-alive, 5 s TX-to, thresholds 256/128, budgets
  3/4); reset reasons each check sets; running_state clear paths and 3 s admin
  timeout; GET_STATS opcode/struct layouts and field-by-field response;
  customer-metrics negotiation/fetch; keep-alive payload; AENQ group
  subscription and handler presence/absence; HOST_ATTR_CONFIG layout and the
  allocate/set-only debug-area lifecycle; ignored-features list.
- Inferred (reasoning over cited code, no single line proves device behavior):
  required device keep-alive cadence (< 3 s); device must complete every
  accepted TX/admin command within the windows or be reset; device treats
  zeroed scope/device_id as whole-device/mine; debug-area population is
  optional; FATAL/WARNING/REFRESH groups are effectively no-ops for these
  drivers.
- Ambiguous: device-side meaning of DEV_STS FATAL bit and of debug-area
  contents (driver never reads either).
