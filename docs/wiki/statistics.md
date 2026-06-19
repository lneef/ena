# Device statistics

What the ENA device produces as statistics, and what the reference driver does
with them. The page is split into two halves on purpose:

- **§1–§3 Device side** — counters and structures the emulated device must
  maintain and return. This is the device contract: get these layouts and
  semantics right and the driver is satisfied.
- **§4–§5 Driver side** — aggregation, negotiation, fetch cadence, and string
  tables the driver builds *on top of* the device output. None of this is the
  device's job; it is documented here only so the boundary is unambiguous.

Cross-references (do not duplicate):
- GET_STATS as an admin opcode in the command catalog: [admin-queue.md](admin-queue.md) §7
- Keep-alive cadence / watchdog reset (the liveness use of the keep-alive AENQ): [health.md](health.md) §1, §5
- Keep-alive AENQ descriptor framing (phase, head doorbell): [aenq.md](aenq.md) §7
- Capability negotiation at init (where CUSTOMER_METRICS support is probed): [device-init.md](device-init.md)

Sources: `userspace/dpdk/ena/` (DPDK PMD) and `kernel/linux/ena/` +
`kernel/linux/common/ena_com/`. Paths below are relative to
`amzn-drivers/`.

---

# Device side — what the device produces

## 1. GET_STATS admin command (opcode 11)

`ENA_ADMIN_GET_STATS = 11`. [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:36]
This is the only way the device hands out counters on demand; it is a normal
admin-queue command (request on the AQ, response on the ACQ — see
[admin-queue.md](admin-queue.md)), **not** part of the periodic liveness path.

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

### Which request fields the device must honor
`ena_get_dev_stats` zeroes the whole ctx and sets only `opcode`, `flags=0`, and
`type`. [userspace/dpdk/ena/base/ena_com.c:2249-2274] Callers `memset` the rest
[userspace/dpdk/ena/base/ena_com.c:2648, 2288, 2679; ena_ethdev.c:4174-4179].
Consequences for the device:
- `type` — **must** dispatch on this; it selects the response struct (§2).
- `requested_metrics` — **must** read this, but only for CUSTOMER_METRICS
  [userspace/dpdk/ena/base/ena_com.c:2289, 2689].
- `u.control_buffer` — **must** read this for the buffer-backed types
  (customer metrics, extended); the device writes results to that DMA address.
- `scope`, `queue_idx`, `device_id` — left zero by every reference caller. The
  device returns whole-device counters and can ignore them. `device_id = 0` (not
  the `0xFFFF` the struct comment documents) must be treated as "this function /
  mine"; accept both 0 and 0xFFFF. There is no `ENA_ADMIN_DEVICE_ID_ANY` symbol
  anywhere in `ena_com` (it is never set). (inferred from omission + memset)

## 2. Response payloads (per `type`)

Response `struct ena_admin_acq_get_stats_resp`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:522-536]: an `acq_common_desc`
followed by a union (`u64 raw[7]`) overlaying the stats structs. For the inline
types (BASIC, ENI, ENA_SRD) the device writes the counters straight into this
union in the ACQ completion. For buffer-backed types (CUSTOMER_METRICS) the
device writes the values into the driver-supplied control buffer (DMA) and the
union carries only the reported-metrics bitmap.

### BASIC (type 0) — `struct ena_admin_basic_stats`
14 × u32 as lo/hi 64-bit pairs.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:431-459]
```
tx_bytes_low/high,  tx_pkts_low/high,
rx_bytes_low/high,  rx_pkts_low/high,
rx_drops_low/high,  tx_drops_low/high,
rx_overruns_low/high
```
Copied verbatim into the driver by `ena_com_get_dev_basic_stats`.
[userspace/dpdk/ena/base/ena_com.c:2642-2655] These are whole-device,
cumulative-since-reset counters. This is the only `type` an emulated device
strictly needs for `rte_eth_stats_get` to work.

### ENI (type 2) — `struct ena_admin_eni_stats`
5 × u64 allowance-exceeded counters: `bw_in_allowance_exceeded`,
`bw_out_allowance_exceeded`, `pps_allowance_exceeded`,
`conntrack_allowance_exceeded`, `linklocal_allowance_exceeded`.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:462-486] Fetched by
`ena_com_get_eni_stats`. [userspace/dpdk/ena/base/ena_com.c:2602-2620] These are
rate-limiter accounting; an emulator with no shaping can report all zeros.

### ENA_SRD (type 3) — `struct ena_admin_ena_srd_info`
`u64 flags` (`ena_admin_ena_srd_flags`) + `struct ena_admin_ena_srd_stats`
(`ena_srd_tx_pkts`, `ena_srd_eligible_tx_pkts`, `ena_srd_rx_pkts`,
`ena_srd_resource_utilization`).
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:488-512] Fetched by
`ena_com_get_ena_srd_info`. [userspace/dpdk/ena/base/ena_com.c:2622-2640]
SRD (Scalable Reliable Datagram) is a Nitro-fabric feature; zeros are a valid
"not engaged" report.

### CUSTOMER_METRICS (type 4) — `struct ena_admin_customer_metrics`
Union arm is just `u64 reported_metrics` (a bitmap of which metric IDs the
device populated). [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:515-520]
The actual metric values are written by the device into the driver's control
buffer (DMA), not into the inline union. Metric IDs
(`enum ena_admin_customer_metrics_id`): BW_IN, BW_OUT, PPS,
CONNTRACK_ALLOWANCE_EXCEEDED, LINKLOCAL_ALLOWANCE_EXCEEDED,
CONNTRACK_ALLOWANCE_AVAILABLE (0..5).
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:20-27]

### EXTENDED (type 1)
Enum value exists but **no reference caller ever issues it**. A device need not
implement it; if it does, output goes to the control buffer like customer
metrics. (explicit — grep finds no `..._TYPE_EXTENDED` caller)

## 3. Keep-alive counters (device-pushed, not polled)

The keep-alive AENQ descriptor doubles as a lightweight stats push: alongside
its liveness role (cadence + watchdog reset live in [health.md](health.md) §1,
§5), `struct ena_admin_aenq_keep_alive_desc` carries three lo/hi 64-bit
counters the device fills: `rx_drops`, `tx_drops`, `rx_overruns`, cumulative
since the last device reset.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1239-1253] (group
`ENA_ADMIN_KEEP_ALIVE = 4`.) The device must keep these consistent with the
BASIC GET_STATS counters of the same name. Descriptor framing (phase, head
doorbell) is in [aenq.md](aenq.md) §7.

---

# Driver side — what the driver handles

None of the following is required of the device; it is what the reference
driver layers on top of the raw counters above.

## 4. Fetch, aggregation, and capability gating

- **On-demand only.** Stats are pulled from DPDK ethdev callbacks
  (`ena_stats_get` for basic, the `xstats_get*` family for extended), never on
  the periodic timer. [userspace/dpdk/ena/ena_ethdev.c:264, 334-338] There is no
  fixed GET_STATS cadence; frequency is whatever the application calls. The only
  *periodic* device→driver stats signal is the keep-alive push (§3).
- **Locking / multi-process.** Every fetch is wrapped in `adapter->admin_lock`;
  in multi-process mode a secondary proxies the request to the primary via the
  `ENA_MP_*` dispatch in `ena_mp_primary_handle`.
  [userspace/dpdk/ena/ena_ethdev.c:4172-4204]
- **Capability-gated source selection.** `ena_copy_customer_metrics` prefers
  CUSTOMER_METRICS and falls back to ENI stats when only the
  `ENA_ADMIN_ENI_STATS` cap is present.
  [userspace/dpdk/ena/ena_ethdev.c:3315-3356] `ena_copy_ena_srd_info` is issued
  only if `ENA_ADMIN_ENA_SRD_INFO` is advertised.
  [userspace/dpdk/ena/ena_ethdev.c:3358-3377] So a device that advertises
  neither cap is never asked for ENI/SRD/customer metrics — BASIC alone is
  enough for a minimal emulation.
- **Keep-alive counter aggregation.** DPDK folds `rx_overruns` into `rx_drops`
  (`drv_stats->rx_drops = rx_drops + rx_overruns`,
  `dev_stats.tx_drops = tx_drops`) — it does **not** surface `rx_overruns`
  separately. [userspace/dpdk/ena/ena_ethdev.c:4095-4104] The kernel keeps all
  three separately in `dev_stats.ka_stats`.
  [kernel/linux/ena/ena_netdev.c:6061-6072]
- **BASIC is copied verbatim**; the driver does not transform basic_stats beyond
  the lo/hi → u64 reassembly. [userspace/dpdk/ena/base/ena_com.c:2642-2655]

## 5. Customer-metrics negotiation and string tables

- **Supported-metrics negotiation (init).** Gated on
  `ena_com_get_cap(ENA_ADMIN_CUSTOMER_METRICS)`.
  [userspace/dpdk/ena/base/ena_com.c:2283, 2668] At init the driver issues
  GET_STATS type=CUSTOMER_METRICS with
  `requested_metrics = ENA_ADMIN_CUSTOMER_METRICS_SUPPORT_MASK`; the device
  replies `u.customer_metrics.reported_metrics`, stored as `supported_metrics`.
  [userspace/dpdk/ena/base/ena_com.c:2276-2294, 2380] Later fetches set
  `requested_metrics = supported_metrics`. So the device sees two distinct
  customer-metrics requests: a probe (all-supported mask) and steady-state
  pulls (the negotiated subset).
- **DMA buffer.** A dedicated buffer is allocated once,
  `buffer_len = ENA_CUSTOMER_METRICS_BUFFER_SIZE`.
  [userspace/dpdk/ena/base/ena_com.c:3242-3256] `ena_com_get_customer_metrics`
  points `u.control_buffer.address/length` at it, issues GET_STATS, then
  `memcpy`s `buffer_len` bytes out. [userspace/dpdk/ena/base/ena_com.c:2657-2696]
- **xstats string table / debug area.** The driver-built xstats name table and
  the HOST_ATTR debug area sized to it
  (`ss_count * ETH_GSTRING_LEN + sizeof(u64) * ss_count`) are a host-side
  presentation concern; the device only provides the registered region and never
  needs to populate it. [userspace/dpdk/ena/ena_ethdev.c:861-890] (See
  [health.md](health.md) §7 for the HOST_ATTR_CONFIG / debug-area lifecycle.)

---

## Device-boundary contract summary
A minimal but compliant device must:
1. Maintain whole-device, cumulative-since-reset counters for the BASIC set
   (tx/rx bytes, tx/rx pkts, rx_drops, tx_drops, rx_overruns) and return them in
   the `raw[7]` union on GET_STATS type=0, dispatching on `type` and ignoring
   `scope`/`queue_idx`/`device_id`. (§1, §2)
2. Keep the keep-alive `rx_drops`/`tx_drops`/`rx_overruns` push in step with the
   BASIC counters. (§3)
3. Only implement ENI/ENA_SRD/CUSTOMER_METRICS if the matching capability is
   advertised; otherwise the driver never requests them. Zeros are valid for
   ENI and SRD. (§2, §4)
4. For CUSTOMER_METRICS, honor the two-phase use of `requested_metrics` and
   write values into the driver's control buffer, reporting populated IDs in
   `reported_metrics`. (§2, §5)

## Claim classification summary
- Explicit (cited to code): GET_STATS opcode; request/response struct layouts;
  type and scope enums; BASIC/ENI/SRD/customer field lists; the verbatim BASIC
  copy; customer-metrics capability gate, negotiation, DMA buffer, and fetch;
  keep-alive counter payload; DPDK rx_overruns fold vs kernel separate storage;
  on-demand (non-periodic) fetch path; admin_lock / MP proxy; ENI/SRD
  capability gating; EXTENDED having no caller.
- Inferred: device must treat zeroed `scope`/`queue_idx`/`device_id` as
  whole-device/mine and accept both `device_id` 0 and 0xFFFF; BASIC alone
  suffices for a minimal emulation; keep-alive counters should agree with BASIC.
- Ambiguous: device-side meaning of EXTENDED output and of the documented
  `device_id = 0xFFFF` value (never sent by the reference drivers).
