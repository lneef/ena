# Interrupts and Interrupt Moderation

Device-side view of how ENA delivers MSI-X interrupts and how the driver
programs interrupt moderation. This page covers the *vector allocation model*,
*moderation negotiation*, the *per-interrupt INTR_REG semantics*, and the
*mask/unmask* behavior.

Cross-references (do not duplicate):
- The `ENA_ETH_IO_INTR_REG` bit layout and the RX re-arm write are documented in
  [rx-path.md](./rx-path.md) §5. This page reuses that layout and focuses on the
  device-side firing/moderation semantics.
- Vector 0 (admin completion + AENQ) sharing and the `INTR_MASK` (0x4c) register
  are documented in [aenq.md](./aenq.md) §6 and
  [registers.md](./registers.md) (`INTR_MASK 0x4c`). This page references them.
- CREATE_CQ `msix_vector` / `cq_interrupt_unmask_register_offset` handling is in
  [queue-setup.md](./queue-setup.md) §3.

Sources: DPDK PMD is the primary reference. Adaptive moderation (DIM) only
exists in the Linux kernel driver, so those claims are cited from
`kernel/linux/`. Differences are flagged in §6.

---

## 1. MSI-X vector allocation model

### Vector layout (kernel reference)
The Linux driver fixes the vector map; the device just receives a `msix_vector`
index per CQ in CREATE_CQ.

```
vector 0          = admin completion queue + AENQ   (ENA_ADMIN_MSIX_VEC = 1 reserves it)
vector 1 .. N     = one per IO queue (TX+RX pair share a vector)
```

[kernel/linux/ena/ena_netdev.h:L56-L57] (explicit)
```c
#define ENA_ADMIN_MSIX_VEC          1
#define ENA_MAX_MSIX_VEC(io_queues) (ENA_ADMIN_MSIX_VEC + (io_queues))
```
[kernel/linux/ena/ena_netdev.h:L121-L123] (explicit)
```c
#define ENA_MGMNT_IRQ_IDX     0
#define ENA_IO_IRQ_FIRST_IDX  1
#define ENA_IO_IRQ_IDX(q)     (ENA_IO_IRQ_FIRST_IDX + (q))
```
So total MSI-X vectors requested = `1 + max_num_io_queues`; the admin/AENQ
vector is index 0, IO vectors start at 1.
`ENA_MAX_NUM_IO_QUEUES = 128`, so the table is sized for up to `1 + 128`
vectors. [kernel/linux/common/ena_com/ena_com.h:L32] (explicit)
Vector 0 is requested with `ena_intr_msix_mgmnt`, which fans out to both the
admin-CQ handler and the AENQ handler — a single vector serving control plane.
[kernel/linux/ena/ena_netdev.c:L1983-L1991, L2089-L2099] (explicit)

The kernel assigns the IO vector to **both** the TX and RX CQ of a pair:
`ctx.msix_vector = ENA_IO_IRQ_IDX(qid)` for each direction.
[kernel/linux/ena/ena_netdev.c:L2468-L2476, L2534-L2542] (explicit) The shared
vector is then unmasked through *one* of the two CQs (see §5).

### DPDK vector model (differs)
DPDK requests the admin/AENQ interrupt as the PCI default interrupt and a
separate efd vector list for RX queues only:
- Admin + AENQ ride the PCI device's default `intr_handle`, served by
  `ena_control_path_handler`. [userspace/dpdk/ena/ena_ethdev.c:L2436-L2437]
  (explicit)
- RX datapath vectors are allocated lazily only if `intr_conf.rxq` is set:
  one vector per RX queue, offset by `RTE_INTR_VEC_RXTX_OFFSET`.
  [userspace/dpdk/ena/ena_ethdev.c:L3813-L3869] (explicit)
- **TX queues get no MSI-X vector** in DPDK: `ctx.msix_vector = -1` for TX, and
  the RX queue gets a real vector only when datapath interrupts are enabled
  (otherwise it also stays `-1`).
  [userspace/dpdk/ena/ena_ethdev.c:L1423-L1436] (explicit)

### `msix_vector` in CREATE_CQ and the interrupt-mode bit
`ena_com_create_io_cq` copies `io_cq->msix_vector` into the admin command and
**always** sets the interrupt-mode-enabled bit:
[userspace/dpdk/ena/base/ena_com.c:L1425-L1430] (explicit)
```c
create_cmd.cq_caps_2 |= (io_cq->cdesc_entry_size_in_bytes / 4) & ...ENTRY_SIZE_WORDS_MASK;
create_cmd.cq_caps_1 |= ENA_ADMIN_AQ_CREATE_CQ_CMD_INTERRUPT_MODE_ENABLED_MASK;
create_cmd.msix_vector = io_cq->msix_vector;
```

CREATE_CQ command struct: [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L342-L368]
(explicit)
```c
struct ena_admin_aq_create_cq_cmd {
    struct ena_admin_aq_common_desc aq_common_descriptor;
    uint8_t  cq_caps_1;   /* bit 5: interrupt_mode_enabled */
    uint8_t  cq_caps_2;   /* bits 4:0: cq_entry_size_words (4 or 8) */
    uint16_t cq_depth;
    uint32_t msix_vector; /* MSI-X vector assigned to this cq */
    struct ena_common_mem_addr cq_ba;
};
```
- `cq_caps_1` bit 5 = `interrupt_mode_enabled`: *if set, the CQ operates in
  interrupt mode; otherwise polling.* The header comment is explicit.
  [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L345-L350] (explicit)
  Constant: `INTERRUPT_MODE_ENABLED_SHIFT = 5`, `..._MASK = BIT(5)`.
  [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L1317-L1318] (explicit)
- **Device-side meaning (inferred):** when bit 5 is set, the device must
  associate this CQ with the supplied `msix_vector` and be prepared to raise
  that vector on completions (subject to mask/moderation, §5). When clear, the
  device never raises an interrupt for the CQ and the driver polls. Both DPDK
  and kernel always set the bit for IO CQs, so the polling-CQ path is never
  exercised by these drivers. (inferred from the bit comment + the fact that it
  is unconditionally set)

### Response: per-CQ unmask register offset
CREATE_CQ response returns the per-CQ MMIO offsets:
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L370-L383] (explicit)
```c
struct ena_admin_acq_create_cq_resp_desc {
    ...
    uint16_t cq_idx;
    uint16_t cq_actual_depth;
    uint32_t numa_node_register_offset;
    uint32_t cq_head_db_register_offset;
    uint32_t cq_interrupt_unmask_register_offset;
};
```
The driver stores
`io_cq->unmask_reg = reg_bar + cq_interrupt_unmask_register_offset`.
[userspace/dpdk/ena/base/ena_com.c:L1451-L1459] (explicit) This is the register
written to mask/unmask and to push moderation deltas (§3, §5). The device is
free to choose this offset; the driver treats it opaquely.

---

## 2. Interrupt moderation negotiation

### Feature ID
`ENA_ADMIN_INTERRUPT_MODERATION = 20`.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L65] (explicit) Support is
probed via `ena_com_check_supported_feature_id(...)`.
[userspace/dpdk/ena/base/ena_com.c:L3355-L3359] (explicit)

### GET_FEATURE response
`ena_com_init_interrupt_moderation` issues GET_FEATURE(INTERRUPT_MODERATION,0).
If the device returns `ENA_COM_UNSUPPORTED`, moderation is disabled silently.
On success the only field consumed is `intr_delay_resolution`.
[userspace/dpdk/ena/base/ena_com.c:L3394-L3426] (explicit)

Response struct: [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L789-L794]
(explicit)
```c
struct ena_admin_feature_intr_moder_desc {
    uint16_t intr_delay_resolution; /* interrupt delay granularity in usec */
    uint16_t reserved;
};
```
(part of the GET_FEATURE response union as `u.intr_moderation`.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L1135] (explicit))

### `intr_delay_resolution` semantics (units, division, zero handling)
- **Unit:** the device-reported granularity in microseconds. All delay values
  written to the INTR register are *usecs divided by this resolution*.
  [userspace/dpdk/ena/base/ena_com.h:L392-L396] (explicit) — the field comment
  states moderation intervals are "in usec divided by intr_delay_resolution,
  which is supplied by the device."
- **Division:** the driver converts a user-configured coalescing time into a
  device interval by integer division:
  [userspace/dpdk/ena/base/ena_com.c:L3361-L3392] (explicit)
  ```c
  *intr_moder_interval = coalesce_usecs / intr_delay_resolution;
  ```
  applied for TX (`intr_moder_tx_interval`) and RX (`intr_moder_rx_interval`).
- **Zero-resolution handling:** a device-reported resolution of 0 is illegal.
  `ena_com_update_intr_delay_resolution` substitutes
  `ENA_DEFAULT_INTR_DELAY_RESOLUTION = 1` (1 usec/tick).
  [userspace/dpdk/ena/base/ena_com.c:L1353-L1376] (explicit),
  [userspace/dpdk/ena/base/ena_com.h:L42] (explicit) The same guard in the
  per-interval update path returns `ENA_COM_FAULT` rather than dividing by zero.
  [userspace/dpdk/ena/base/ena_com.c:L3366-L3369] (explicit)
- **Re-scaling on change:** if the resolution changes, the already-stored
  tx/rx intervals are rescaled `interval * prev_resolution / new_resolution`.
  [userspace/dpdk/ena/base/ena_com.c:L1363-L1375] (explicit)

### `ena_com_interrupt_moderation_init` flow (summary)
1. GET_FEATURE(INTERRUPT_MODERATION). Unsupported -> disable adaptive, return.
2. Read `intr_delay_resolution`, store it (with the zero-guard above).
3. Disable adaptive moderation by default (can be enabled later by the OS
   driver). [userspace/dpdk/ena/base/ena_com.c:L3418-L3423] (explicit)

This admin exchange is the *only* moderation-related admin command. After it,
all moderation values reach the device purely through INTR_REG writes (§3).

### Default coalescing intervals
- DPDK: TX = 0 us, RX = 0 us (no moderation by default).
  [userspace/dpdk/ena/base/ena_plat_dpdk.h:L315-L316] (explicit), aliased via
  [userspace/dpdk/ena/base/ena_com.h:L40-L41] (explicit).
- Kernel: TX = 64 us, RX = 20 us.
  [kernel/linux/common/ena_com/ena_com.h:L64-L65] (explicit)

These are pre-resolution-division usec values; the device sees them divided by
`intr_delay_resolution`.

---

## 3. How moderation values reach the device

There is **no admin command that pushes moderation intervals**. After the
one-time GET_FEATURE in §2, the delays are delivered to the device only as
fields of the per-interrupt 32-bit MMIO write to the CQ's unmask register
(`io_cq->unmask_reg`). [userspace/dpdk/ena/base/ena_eth_com.h:L79-L83]
(explicit)

The register is built by `ena_com_update_intr_reg` and written by
`ena_com_unmask_intr` (single `writel`). Full bit layout is in
[rx-path.md](./rx-path.md) §5; reproduced here for the moderation fields:
```
bits 14:0  rx_intr_delay         (usecs / intr_delay_resolution)
bits 29:15 tx_intr_delay         (usecs / intr_delay_resolution)
bit  30    intr_unmask           (1 = unmask/arm the vector)
bit  31    no_moderation_update  (1 = device must NOT update its moderation state)
```
[userspace/dpdk/ena/base/ena_defs/ena_eth_io_defs.h:L389-L395] (explicit)

### `no_moderation_update` (bit 31) semantics
Driver-side header comment: "0 - Indicates that any of the TX/RX intervals was
updated, 1 - otherwise." [userspace/dpdk/ena/base/ena_com.h:L1188-L1197]
(explicit)

- **Device-side meaning (inferred):** when bit 31 = 1, the device must ignore
  the rx/tx delay fields of this write and keep its current moderation state —
  the write is purely a mask/unmask operation. When bit 31 = 0, the device
  latches the rx_intr_delay/tx_intr_delay fields as the new moderation
  intervals for this CQ. (inferred from the field comment plus the two callers
  below, which only clear the bit when an interval actually changed.)

Callers:
- DPDK RX intr enable/disable always passes `rx=tx=0, no_moderation_update=1`,
  i.e. it touches the register *only to arm/disarm*, never to reprogram delays.
  [userspace/dpdk/ena/ena_ethdev.c:L3886-L3889] (explicit)
- Kernel `ena_unmask_interrupt` sets `no_moderation_update=0` *only when* the
  current tx or rx interval differs from the previously written value; otherwise
  it leaves it 1. [kernel/linux/ena/ena_netdev.c:L1827-L1867] (explicit) This is
  the path that actually pushes new delays to the device (so the device updates
  moderation state only when explicitly told via bit 31 = 0).

---

## 4. Adaptive moderation / DIM (kernel only) — device-level view

Adaptive RX moderation (DIM, Dynamic Interrupt Moderation) exists only in the
Linux kernel driver, not DPDK. From the **device's point of view it adds
nothing new**: the device still only ever sees varying `rx_intr_delay` values in
the same unmask write (§3).

Flow (kernel):
- After each RX NAPI poll, `ena_adjust_adaptive_rx_intr_moderation` feeds packet
  and byte counts to `net_dim`. [kernel/linux/ena/ena_netdev.c:L1803-L1825]
  (explicit)
- `ena_dim_work` picks a profile and writes the chosen usec into
  `rx_ring->interrupt_interval`. [kernel/linux/ena/ena_netdev.c:L1791-L1801]
  (explicit)
- On the next unmask, `ena_unmask_interrupt` notices the interval changed and
  emits the INTR_REG write with `no_moderation_update=0` and the new
  `rx_intr_delay`. [kernel/linux/ena/ena_netdev.c:L1843-L1858] (explicit)

So the device must simply honor whatever `rx_intr_delay` it is handed; the
adaptation policy is entirely host-side. (explicit for the mechanism; the
"device sees only varying delays" conclusion is inferred from there being no
adaptive-specific admin command or register.)

### Kernel `lost_interrupt` parameter (no new device bit)
The kernel `ena_com_update_intr_reg` has an extra `lost_interrupt` argument
absent in DPDK. It does **not** introduce a new register bit: when
`lost_interrupt` is true the driver simply omits bit 30 (does not re-arm) and
forces `no_moderation_update` effectively, producing the same 32-bit layout.
[kernel/linux/common/ena_com/ena_com.h:L1351-L1370] (explicit) Emulation can
ignore it — it changes only which bits the driver chooses to set.

---

## 5. Mask / unmask device semantics

Device-side rules, all inferred from the driver's use of the register unless a
header comment is cited:

### IO CQ vectors
- An IO CQ's MSI-X vector is armed only after a write to that CQ's unmask
  register with bit 30 (`intr_unmask`) = 1.
  [userspace/dpdk/ena/base/ena_defs/ena_eth_io_defs.h:L392-L393] (explicit for
  the bit; arming semantics inferred)
- **Auto-mask after firing (inferred):** the driver re-writes the unmask
  register with bit 30 every time it finishes processing completions and wants
  the next interrupt (kernel `ena_unmask_interrupt` is called at the end of NAPI
  [kernel/linux/ena/ena_netdev.c:L1964, L5190]; DPDK re-enables on demand
  [ena_ethdev.c:L3892-L3906]). The natural device contract consistent with this
  is: firing the MSI-X auto-masks the vector, and the driver must explicitly
  unmask (bit 30) to re-arm. (inferred from the re-arm-after-poll pattern; not
  stated in a header)
- A write with bit 30 = 0 (`unmask=false`) masks the vector. DPDK uses this to
  disable RX datapath interrupts. [ena_ethdev.c:L3900-L3906] (explicit)
- **Startup state:** RX queues created with datapath interrupts start *masked*;
  the first unmask is deferred until the application enables the RX interrupt.
  [userspace/dpdk/ena/ena_ethdev.c:L1467-L1469] (explicit, see
  [queue-setup.md](./queue-setup.md))

### Shared TX/RX vector (kernel)
Because the kernel maps one MSI-X vector to a TX+RX CQ pair, it unmasks through
just one of them: `ena_com_unmask_intr(tx_ring->ena_com_io_cq, &intr_reg)`. The
device must treat a write to either CQ's unmask register as arming the shared
vector. [kernel/linux/ena/ena_netdev.c:L1863-L1867] (explicit driver behavior;
shared-vector device contract inferred)

### Admin / AENQ vector 0
The control-plane vector is masked/unmasked by the separate `INTR_MASK`
register at offset `0x4c` (bit 0 = masked), not by the per-CQ unmask register.
Full semantics (polling vs interrupt mode, AENQ ack independence) are in
[aenq.md](./aenq.md) §6 and [registers.md](./registers.md) `INTR_MASK (0x4c)`.
[userspace/dpdk/ena/base/ena_defs/ena_regs_defs.h:L53] (explicit) Interplay: the
IO-CQ unmask registers and the admin `INTR_MASK` are independent mask domains —
masking the admin interrupt does not affect IO vectors and vice versa.
(inferred from the registers being distinct offsets with distinct call sites)

---

## 6. Device-written-but-ignored fields and DPDK-vs-kernel differences

### Device-written, driver-ignored
- `ena_admin_feature_intr_moder_desc.reserved` (upper 16 bits of the response
  word) is ignored. [...ena_admin_defs.h:L789-L794] (explicit)
- CREATE_CQ response `numa_node_register_offset` is optional — only stored if
  non-zero, and in DPDK only TX uses the NUMA cfg write.
  [userspace/dpdk/ena/base/ena_com.c:L1456-L1459] (explicit)
- `cq_actual_depth` in the CREATE_CQ response is consumed for queue setup, not
  for interrupts; noted here only to avoid confusion. (explicit)

### DPDK vs kernel behavioral differences worth flagging for emulation
1. **TX interrupts:** kernel arms IO vectors shared by TX+RX; DPDK never assigns
   an MSI-X to TX (`msix_vector = -1`) and polls TX completions.
   [ena_ethdev.c:L1423-L1436] vs [ena_netdev.c:L2468-L2476] (explicit) An
   emulation must accept `msix_vector = -1` / unset for a CQ without arming a
   vector.
2. **Default moderation:** DPDK defaults to 0/0 us (no moderation); kernel
   defaults to 64/20 us and may further vary RX via DIM. The device should not
   assume non-zero delays. (explicit, §2)
3. **`no_moderation_update`:** DPDK *always* sets it (never reprograms delays
   through the fast path); kernel clears it whenever an interval changed. A
   device that latches delays only when bit 31 = 0 is correct for both.
   [ena_ethdev.c:L3888] vs [ena_netdev.c:L1838-L1858] (explicit)
4. **`lost_interrupt`:** kernel-only driver heuristic; produces no new
   device-visible bit (§4). Safe to ignore in emulation.
5. **Admin/AENQ vector:** kernel uses a dedicated MSI-X vector 0 served by
   `ena_intr_msix_mgmnt`; DPDK rides the PCI default interrupt with
   `ena_control_path_handler`. Both ultimately gate on the `INTR_MASK` 0x4c
   register, so the device behavior is identical from the register side.
   [ena_netdev.c:L1983-L1991] vs [ena_ethdev.c:L2436-L2437] (explicit)
6. **HOST_INFO interrupt-moderation capability bit:** the kernel advertises
   `ENA_ADMIN_HOST_INFO_INTERRUPT_MODERATION` (bit 2 of
   `driver_supported_features`); DPDK does **not** set it (it only sets
   RX_OFFSET and RSS_CONFIGURABLE_FUNCTION_KEY).
   [...ena_admin_defs.h:L1382-L1383] (explicit),
   [userspace/dpdk/ena/ena_ethdev.c:L829-L831] (explicit) The device should not
   require this bit to enable the moderation feature.
