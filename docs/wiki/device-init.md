# ENA Device Init / Attachment Flow

How the reference driver brings an ENA device from PCI probe to a fully
attached, datapath-ready state, and how it tears it down. This page is the
top-level "what the device must answer" reference. For the byte-level register
semantics see [registers.md](registers.md); for AENQ setup/keep-alive see
[aenq.md](aenq.md); for IO queue creation see [queue-setup.md](queue-setup.md).

Primary evidence is the DPDK PMD (the project target). Kernel paths are cited
where they confirm or extend the contract.

Claim classification at the bottom.

---

## 1. PCI attachment

### Vendor / device IDs

| ID | Value | Meaning | Evidence |
|----|-------|---------|----------|
| Vendor | `0x1d0f` | Amazon | [kernel/linux/ena/ena_pci_id_tbl.h:10] |
| `PCI_DEV_ID_ENA_PF` | `0x0ec2` | Physical function | [kernel/linux/ena/ena_pci_id_tbl.h:14] |
| `PCI_DEV_ID_ENA_LLQ_PF` | `0x1ec2` | PF, LLQ variant | [kernel/linux/ena/ena_pci_id_tbl.h:18] |
| `PCI_DEV_ID_ENA_VF` | `0xec20` | Virtual function | [kernel/linux/ena/ena_pci_id_tbl.h:22] |
| `PCI_DEV_ID_ENA_LLQ_VF` | `0xec21` | VF, LLQ variant | [kernel/linux/ena/ena_pci_id_tbl.h:26] |
| `PCI_DEV_ID_ENA_RESRV0` | `0x0051` | Reserved | [kernel/linux/ena/ena_pci_id_tbl.h:30] |

The kernel `pci_device_id` table binds all six [kernel/linux/ena/ena_pci_id_tbl.h:36-43].
The DPDK PMD only binds the two VF IDs: `0xEC20` and `0xEC21`
[userspace/dpdk/ena/ena_ethdev.c:187-188, 216-219]. So the LLQ-vs-non-LLQ
"variant" is *not* selected by device ID in DPDK; LLQ placement is negotiated at
runtime via the `ENA_ADMIN_LLQ` feature (see [queue-setup.md](queue-setup.md)
and §6 here), and the device-ID variant is only a hint.

The DPDK driver registers with `RTE_PCI_DRV_NEED_MAPPING |
RTE_PCI_DRV_INTR_LSC | RTE_PCI_DRV_WC_ACTIVATE`
[userspace/dpdk/ena/ena_ethdev.c:4013-4019]. `WC_ACTIVATE` requests
write-combining on the BARs (relevant for the LLQ mem bar).

### BAR layout

| BAR | Name | Contents | Evidence |
|-----|------|----------|----------|
| BAR0 | `ENA_REGS_BAR` (0) | MMIO register space (see [registers.md](registers.md)) | [userspace/dpdk/ena/ena_ethdev.h:22] |
| BAR2 | `ENA_MEM_BAR` (2) | LLQ / device-memory bar (Tx descriptor + header push area) | [userspace/dpdk/ena/ena_ethdev.h:23] |

DPDK maps both: `adapter->regs = mem_resource[ENA_REGS_BAR].addr` and
`adapter->dev_mem_base = mem_resource[ENA_MEM_BAR].addr`
[userspace/dpdk/ena/ena_ethdev.c:2311-2312]. BAR0 is mandatory — a NULL
`regs` aborts probe with `-ENXIO` [userspace/dpdk/ena/ena_ethdev.c:2314-2318].
`ena_dev->reg_bar` is set to the BAR0 pointer
[userspace/dpdk/ena/ena_ethdev.c:2320]; every register access in `ena_com`
is `reg_bar + offset`.

The mem bar (BAR2) is only consumed if LLQ placement is chosen:
`ena_dev->mem_bar = adapter->dev_mem_base` is assigned in the LLQ-setup path,
guarded by a NULL check on `dev_mem_base`
[userspace/dpdk/ena/ena_ethdev.c:2144-2163]. Kernel mirrors this: it maps
`ENA_REG_BAR` (0) unconditionally [kernel/linux/ena/ena_netdev.c:5534-5536]
and maps `ENA_MEM_BAR` (2) only `if (bars & BIT(ENA_MEM_BAR))`
[kernel/linux/ena/ena_netdev.c:4294-4301]. Kernel `ENA_BAR_MASK =
BIT(0) | BIT(2)` [kernel/linux/ena/ena_netdev.h:72-74] — BAR1 is not used.

### Readless capability via PCI config (revision/class)

Whether the indirect "readless" register-read path is available is decided from
PCI config space, not from a register:
- DPDK reads `pdev->id.class_id & ENA_MMIO_DISABLE_REG_READ`
  [userspace/dpdk/ena/ena_ethdev.c:1875].
- Kernel reads `pdev->revision & ENA_MMIO_DISABLE_REG_READ`
  [kernel/linux/ena/ena_netdev.c:4331].
- `ENA_MMIO_DISABLE_REG_READ = BIT(0)` [userspace/dpdk/ena/ena_ethdev.h:37].

If that bit is set, readless is disabled and the driver does direct BAR0 reads
instead of the MMIO_REG_READ handshake (see [registers.md](registers.md) §
"Indirect MMIO register read").

### MSI-X expectations

DPDK uses the EAL-provided `pci_dev->intr_handle` for the control path; it
registers `ena_control_path_handler` and `rte_intr_enable`s the handle for
admin + AENQ interrupts [userspace/dpdk/ena/ena_ethdev.c:2436-2438], or runs a
polling alarm instead if `control_path_poll_interval` is set
[userspace/dpdk/ena/ena_ethdev.c:2439-2447].

Kernel vector budget: `ENA_MAX_MSIX_VEC(io_queues) = ENA_ADMIN_MSIX_VEC +
io_queues` [kernel/linux/ena/ena_netdev.h:57] — i.e. one dedicated admin/AENQ
vector (`ENA_ADMIN_MSIX_VEC`, index 0) plus one vector per IO queue. Vectors are
allocated with `pci_alloc_irq_vectors(pdev, ENA_MIN_MSIX_VEC, msix_vecs, ...)`
[kernel/linux/ena/ena_netdev.c:2034-2050]. The emulated device should therefore
expose at least 1 + N MSI-X vectors; admin completion and AENQ share vector 0.

---

## 2. Ordered initialization sequence

`eth_ena_dev_init` [userspace/dpdk/ena/ena_ethdev.c:2267] does PCI/BAR plumbing
then calls `ena_device_init` [userspace/dpdk/ena/ena_ethdev.c:1856] for the
device handshake. The device must satisfy these steps **in this order**:

### Step 0 — MMIO readless init
`ena_com_mmio_reg_read_request_init` [userspace/dpdk/ena/ena_com.c:2010-2034]:
allocates the coherent `read_resp` response buffer and writes its DMA address to
`MMIO_RESP_LO/HI` (0x60/0x64) via
`ena_com_mmio_reg_read_request_write_dev_addr`
[userspace/dpdk/ena/ena_com.c:2060-2070]. `seq_num` starts at 0,
`readless_supported` defaults true. Then
`ena_com_set_mmio_read_mode` overrides it from the PCI config bit (§1)
[userspace/dpdk/ena/ena_ethdev.c:1876; userspace/dpdk/ena/ena_com.c:2036-2041].
**The device must accept the RESP address write before any register read.**
Mechanism detail: [registers.md](registers.md) § "Indirect MMIO register read".

### Step 1 — Device reset handshake
`ena_com_dev_reset(ena_dev, ENA_REGS_RESET_NORMAL)`
[userspace/dpdk/ena/ena_ethdev.c:1879; userspace/dpdk/ena/ena_com.c:2514].
Reads `DEV_STS` and `CAPS`; requires `DEV_STS.READY`
[userspace/dpdk/ena/ena_com.c:2530-2533]; extracts the reset timeout from
`CAPS.RESET_TIMEOUT` (100 ms units), and `0` is rejected as invalid
[userspace/dpdk/ena/ena_com.c:2535-2541]. Writes `DEV_CTL.DEV_RESET` with the
reset reason, **re-writes the MMIO RESP address**
[userspace/dpdk/ena/ena_com.c:2570-2573], waits for
`DEV_STS.RESET_IN_PROGRESS` to go high, writes `DEV_CTL=0`, waits for it to go
low [userspace/dpdk/ena/ena_com.c:2575-2588]. Finally derives the admin command
timeout from `CAPS.ADMIN_CMD_TO` (100 ms units), else
`ADMIN_CMD_TIMEOUT_US = 3000000` [userspace/dpdk/ena/ena_com.c:2590-2597].
Full byte/bit handshake and reset-reason encoding: [registers.md](registers.md)
§ "Device reset sequence" and § "Reset reason encoding".

### Step 2 — Version / caps validation
`ena_com_validate_version` [userspace/dpdk/ena/ena_com.c:1636]:
reads `VERSION` (0x00) and `CONTROLLER_VERSION` (0x04). The controller version,
masked to major|minor|sub_minor (implementation-ID byte excluded), must be
`>= MIN_ENA_CTRL_VER` or init fails with `-1`
[userspace/dpdk/ena/ena_com.c:1677-1686]. `MIN_ENA_CTRL_VER` is built from
`ENA_CTRL_MAJOR=0, ENA_CTRL_MINOR=0, ENA_CTRL_SUB_MINOR=1`
[userspace/dpdk/ena/ena_com.c:17-26], i.e. controller version **0.0.1**.
A timeout read (`0xFFFFFFFF`) is treated as failure
[userspace/dpdk/ena/ena_com.c:1649-1653].

`ena_com_get_dma_width` [userspace/dpdk/ena/ena_com.c:1610]: reads
`CAPS.DMA_ADDR_WIDTH`; valid range is `32 <= width <= 48`
(`ENA_MAX_PHYS_ADDR_SIZE_BITS = 48` [userspace/dpdk/ena/ena_com.h:17]) else
`ENA_COM_INVAL` [userspace/dpdk/ena/ena_com.c:1626-1629]. Stored in
`ena_dev->dma_addr_bits` [userspace/dpdk/ena/ena_ethdev.c:1892]. Kernel
additionally calls `dma_set_mask_and_coherent(DMA_BIT_MASK(dma_width))`
[kernel/linux/ena/ena_netdev.c:4346-4358].

### Step 3 — Admin queue init
`ena_com_admin_init(ena_dev, &aenq_handlers)`
[userspace/dpdk/ena/ena_ethdev.c:1895; userspace/dpdk/ena/ena_com.c:2072].
Requires `DEV_STS.READY` (re-checked) else `ENA_COM_NO_DEVICE`
[userspace/dpdk/ena/ena_com.c:2079-2089]. Sets `q_depth =
ENA_ADMIN_QUEUE_DEPTH = 32` [userspace/dpdk/ena/ena_com.c:2091, :15], allocates
SQ/CQ/comp-ctx, programs:
- `AQ_BASE_LO/HI` (0x10/0x14) = admin SQ DMA addr
  [userspace/dpdk/ena/ena_com.c:2117-2121].
- `ACQ_BASE_LO/HI` (0x20/0x24) = admin CQ DMA addr
  [userspace/dpdk/ena/ena_com.c:2123-2127].
- `AQ_CAPS` (0x18) = depth | (`sizeof(ena_admin_aq_entry)` << 16)
  [userspace/dpdk/ena/ena_com.c:2129-2135, 2145].
- `ACQ_CAPS` (0x28) = depth | (`sizeof(ena_admin_acq_entry)` << 16)
  [userspace/dpdk/ena/ena_com.c:2137-2146].
- `sq.db_addr` = `reg_bar + AQ_DB_OFF` (0x2c)
  [userspace/dpdk/ena/ena_com.c:2114-2115].

Then `ena_com_admin_init_aenq` (Step 4 below). On success sets
`running_state = true` [userspace/dpdk/ena/ena_com.c:2151-2155].

Immediately after, the driver forces **admin polling mode**:
`ena_com_set_admin_polling_mode(ena_dev, true)` — writes `INTR_MASK=1` (mask
admin IRQ) so it can poll completions while MSI-X vector count is still unknown
[userspace/dpdk/ena/ena_ethdev.c:1906; userspace/dpdk/ena/ena_com.c:1737-1747].

### Step 4 — AENQ init (inside admin init)
`ena_com_admin_init_aenq` [userspace/dpdk/ena/ena_com.c:136] runs as part of
Step 3 [userspace/dpdk/ena/ena_com.c:2147]: `q_depth =
ENA_ASYNC_QUEUE_DEPTH = 16` [userspace/dpdk/ena/ena_com.c:143, :14], allocates
the AENQ ring, writes `AENQ_BASE_LO/HI` (0x38/0x3c) and `AENQ_CAPS` (0x34),
sets `head = q_depth`, `phase = 1`. Full setup, the `AENQ_CONFIG` set-feature
negotiation, phase-bit protocol, and the enabling `AENQ_HEAD_DB` write are
documented in [aenq.md](aenq.md) §3-§5. Note the **`AENQ_CONFIG` set-feature is
issued at different times** by the two drivers:
- Kernel issues it inside `ena_device_init`
  [kernel/linux/ena/ena_netdev.c:4400-4416].
- DPDK only computes/stashes the supported group mask in `ena_device_init`
  [userspace/dpdk/ena/ena_ethdev.c:1918-1927] and issues the actual
  `ena_com_set_aenq_config` later in `ena_configure_aenq` (called from
  `ena_dev_configure`) [userspace/dpdk/ena/ena_ethdev.c:3908-3935, 2510].

The candidate group set (intersected with `get_feat.aenq.supported_groups`):
`LINK_CHANGE | NOTIFICATION | KEEP_ALIVE | FATAL_ERROR | WARNING |
CONF_NOTIFICATIONS` [userspace/dpdk/ena/ena_ethdev.c:1918-1925] (kernel also
adds `DEVICE_REQUEST_RESET` [kernel/linux/ena/ena_netdev.c:4400-4408]).

### Step 5 — Host info / HOST_ATTR_CONFIG set-feature
`ena_config_host_info` [userspace/dpdk/ena/ena_ethdev.c:801] runs *before*
GET_FEATURE device attributes [userspace/dpdk/ena/ena_ethdev.c:1908]. It
allocates the host-info page, fills `ena_admin_host_info`, then issues
`ena_com_set_host_attributes` [userspace/dpdk/ena/ena_com.c:3305] which sends a
`SET_FEATURE` with `feature_id = ENA_ADMIN_HOST_ATTR_CONFIG`
[userspace/dpdk/ena/ena_com.c:3321-3322] carrying:
- `os_info_ba` = host_info page DMA addr
  [userspace/dpdk/ena/ena_com.c:3332-3334].
- `debug_ba` = debug-area DMA addr [userspace/dpdk/ena/ena_com.c:3324-3326].
- `debug_area_size` [userspace/dpdk/ena/ena_com.c:3340].

`ena_admin_host_info` layout [userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:937-990]:

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0x00 | `os_type` | u32 | `ENA_ADMIN_OS_*`; DPDK sets 3, kernel sets `ENA_ADMIN_OS_LINUX` [userspace/dpdk/ena/ena_ethdev.c:815 expands to 3 via macro; kernel/linux/ena/ena_netdev.c:3507] |
| 0x04 | `os_dist_str[128]` | u8[] | distro string |
| 0x84 | `os_dist` | u32 | numeric |
| 0x88 | `kernel_ver_str[32]` | u8[] | version string |
| 0xa8 | `kernel_ver` | u32 | numeric |
| 0xac | `driver_version` | u32 | 7:0 major, 15:8 minor, 23:16 sub_minor, 31:24 module_type |
| 0xb0 | `supported_network_features[2]` | u32[2] | |
| 0xb8 | `ena_spec_version` | u16 | |
| 0xba | `bdf` | u16 | 2:0 function, 7:3 device, 15:8 bus; kernel sets `pci_dev_id(pdev)` [kernel/linux/ena/ena_netdev.c:3506] |
| 0xbc | `num_cpus` | u16 | |
| 0xbe | `reserved` | u16 | |
| 0xc0 | `driver_supported_features` | u32 | bit flags below |

`driver_version` field shifts: `MINOR_SHIFT=8`, `SUB_MINOR_SHIFT=16`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1369-1371].
`driver_supported_features` bits: `RX_OFFSET = BIT(1)`,
`RSS_CONFIGURABLE_FUNCTION_KEY = BIT(4)`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1381, 1387]; DPDK sets those
two [userspace/dpdk/ena/ena_ethdev.c:829-831], kernel sets a larger set
including interrupt_moderation, rx_buf_mirroring, rx_page_reuse,
tx_ipv6_csum_offload, phc, debug_area_ext
[kernel/linux/ena/ena_netdev.c:3529-3537].

A second `HOST_ATTR_CONFIG` set-feature is issued from `ena_config_debug_area`
after allocating the debug area [userspace/dpdk/ena/ena_ethdev.c:861-889],
called from `eth_ena_dev_init` at
[userspace/dpdk/ena/ena_ethdev.c:2403].

The device may answer `ENA_COM_UNSUPPORTED` — the driver only warns and frees
the host-info page; it is **non-fatal**
[userspace/dpdk/ena/ena_ethdev.c:834-846].

### Step 6 — GET_FEATURE DEVICE_ATTRIBUTES (+ companion feature reads)
`ena_com_get_dev_attr_feat` [userspace/dpdk/ena/ena_com.c:2298], called at
[userspace/dpdk/ena/ena_ethdev.c:1911]. Issues `GET_FEATURE` for
`ENA_ADMIN_DEVICE_ATTRIBUTES` and copies
`ena_admin_device_attr_feature_desc`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:557-584]:
- `mac_addr[6]` (network byte order) — copied to `eth_dev->data->mac_addrs`
  [userspace/dpdk/ena/ena_ethdev.c:2410-2414].
- `max_mtu` — stored as `adapter->max_mtu`
  [userspace/dpdk/ena/ena_ethdev.c:2406]. See "max_mtu semantics" below.
- `supported_features` bitmap (of `ena_admin_aq_feature_id`) →
  `ena_dev->supported_features` [userspace/dpdk/ena/ena_com.c:2312].
- `capabilities` bitmap → `ena_dev->capabilities`
  [userspace/dpdk/ena/ena_com.c:2313].
- `phys_addr_width` / `virt_addr_width`.

#### max_mtu semantics
- **No reference-mandated value.** `max_mtu` is a runtime `uint32_t`
  [ena_admin_defs.h:583]; the reference has no `ENA_MAX_MTU` / jumbo upper
  constant. The only MTU constants are lower bounds: `ENA_MIN_MTU = 128`
  [ena_ethdev.h:35], `ENA_MIN_FRAME_LEN = 64` [ena_ethdev.h:26] (Linux mirrors
  `ENA_MIN_MTU = 128` [kernel/linux/ena/ena_netdev.h:87]). (explicit — absence
  of any max constant)
- **It is the L3 payload MTU** (IP-payload bytes, excluding Ethernet
  header/CRC). The driver adds framing on top:
  `max_rx_pktlen = max_mtu + RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN`
  [ena_ethdev.c:2638-2639]. (explicit)
- **Contract: `configured_mtu <= max_mtu`.** DPDK does not clamp in the PMD —
  `ena_mtu_set` forwards the request via SET_FEATURE `ENA_ADMIN_MTU`
  [ena_ethdev.c:1284-1304], [ena_com.c:2699-2716]; the framework enforces the
  bound against `dev_info->max_mtu = adapter->max_mtu` /
  `min_mtu = ENA_MIN_MTU` [ena_ethdev.c:2640-2641]. Linux clamps explicitly:
  `ena_change_mtu` rejects `> adapter->max_mtu || < ENA_MIN_MTU`
  [kernel/linux/ena/ena_netdev.c:207-212], with a probe-time check failing if
  `dev_attr.max_mtu < netdev->mtu` [ena_netdev.c:4205-4209]. (explicit)
- **9001 is not a device attribute.** The well-known EC2 jumbo MTU 9001 is an
  instance/ENI L3-payload size set by the network config (encapsulation
  headroom), not anything the device reports; it appears nowhere in the
  reference. The device only needs to advertise `max_mtu >=` the guest's
  intended MTU. Our emulation hardcodes `ENA_DEV_MAX_MTU = 9001` [hw/ena.c:41]
  — a valid (not reference-mandated) ceiling that matches the common EC2 jumbo
  payload, so a guest configuring up to 9001 still passes `configured_mtu <=
  max_mtu`. The true value real hardware reports is control-plane /
  instance-type assigned and not derivable from these sources. (inferred)

It then reads further features in sequence (the device must answer or return
unsupported): `MAX_QUEUES_EXT` (if `supported_features` bit set) or
`MAX_QUEUES_NUM` [userspace/dpdk/ena/ena_com.c:2315-2339] (see
[queue-setup.md](queue-setup.md) §7), `AENQ_CONFIG`
[userspace/dpdk/ena/ena_com.c:2341-2347], `STATELESS_OFFLOAD_CONFIG`
[userspace/dpdk/ena/ena_com.c:2349-2355], `HW_HINTS` (optional — 0 if
unsupported) [userspace/dpdk/ena/ena_com.c:2357-2368], and `LLQ`
(optional) [userspace/dpdk/ena/ena_com.c:2370-2378]. `tx_max_header_size` is
taken from the queue feature [userspace/dpdk/ena/ena_com.c:2327-2335].

### Step 7 — Interrupt moderation init
`ena_com_init_interrupt_moderation` [userspace/dpdk/ena/ena_com.c:3394] does a
`GET_FEATURE ENA_ADMIN_INTERRUPT_MODERATION`. If unsupported it disables
adaptive moderation and returns 0 (non-fatal)
[userspace/dpdk/ena/ena_com.c:3400-3416]; otherwise it records
`intr_delay_resolution` [userspace/dpdk/ena/ena_com.c:3419-3423]. (Called from
the start/queue-config path rather than `ena_device_init`.)

### Step 8 — Switch admin to interrupt mode + enable AENQ
After IO-queue counts are known and RSS/stats are set up, DPDK either registers
the control-path interrupt and calls
`ena_com_set_admin_polling_mode(ena_dev, false)` (writes `INTR_MASK=0`)
[userspace/dpdk/ena/ena_ethdev.c:2436-2438] or stays in polling mode, then calls
`ena_com_admin_aenq_enable` [userspace/dpdk/ena/ena_ethdev.c:2448] which writes
the AENQ head doorbell (`AENQ_HEAD_DB=q_depth`) — see [aenq.md](aenq.md) §3.
Adapter state becomes `ENA_ADAPTER_STATE_INIT`
[userspace/dpdk/ena/ena_ethdev.c:2452].

### Step 9 — Keep-alive watchdog start
Keep-alive is driven by the `ENA_ADMIN_KEEP_ALIVE` AENQ group. The driver arms
the watchdog timestamp at start [userspace/dpdk/ena/ena_ethdev.c:1339] and the
`ena_keep_alive` AENQ handler refreshes `timestamp_wd`
[userspace/dpdk/ena/ena_ethdev.c:4082-4092]. Timeout detection and the reset it
triggers are in §4 and [aenq.md](aenq.md) §7.

---

## 3. Device attachment readiness

A device is "attached / ready" when all of the following hold:

1. **`DEV_STS.READY` set** before admin init — checked in both
   `ena_com_dev_reset` [userspace/dpdk/ena/ena_com.c:2530-2533] and
   `ena_com_admin_init` [userspace/dpdk/ena/ena_com.c:2086-2089]. Not ready ⇒
   `ENA_COM_NO_DEVICE`, probe aborts.
2. **Admin queue running** — `admin_queue->running_state = true` after a
   successful `ena_com_admin_init` [userspace/dpdk/ena/ena_com.c:2151-2155]. The
   watchdog later treats `running_state == false` as a fatal condition
   (`check_for_admin_com_state` → reset reason `ADMIN_TO`)
   [userspace/dpdk/ena/ena_ethdev.c:1988-1994].
3. **GET_FEATURE DEVICE_ATTRIBUTES succeeded** (mac/mtu/feature bitmap
   obtained) [userspace/dpdk/ena/ena_ethdev.c:1911-1916].
4. **Link state** — DPDK derives link from the `ENA_ADMIN_LINK_CHANGE` AENQ
   event: `ena_update_on_link_change` reads
   `get_ena_admin_aenq_link_change_desc_link_status`, stores
   `adapter->link_status`, and fires an LSC callback
   [userspace/dpdk/ena/ena_ethdev.c:4041-4056]. `ena_link_update` reports that
   cached status [userspace/dpdk/ena/ena_ethdev.c:1058-1065]. LSC is only
   advertised if the device supports the `LINK_CHANGE` group, otherwise the
   `RTE_ETH_DEV_INTR_LSC` flag is cleared
   [userspace/dpdk/ena/ena_ethdev.c:2354-2356]. The kernel additionally reads a
   `LINK_CONFIG` get-feature for link speed; DPDK reports
   `RTE_ETH_SPEED_NUM_NONE` [userspace/dpdk/ena/ena_ethdev.c:1065].

So for the emulated device: set `DEV_STS.READY`, accept the admin-queue
register programming, answer DEVICE_ATTRIBUTES, and emit a `LINK_CHANGE` AENQ
entry with link-up so the driver marks the port up.

---

## 4. Teardown / detach and reset paths

### Normal close (`ena_close`) [userspace/dpdk/ena/ena_ethdev.c:892]
Order: stop the port if running [userspace/dpdk/ena/ena_ethdev.c:907-908];
disable/unregister the control-path interrupt or cancel the poll alarm
[userspace/dpdk/ena/ena_ethdev.c:911-918]; release rx/tx queues
[userspace/dpdk/ena/ena_ethdev.c:920-921]; then the ena_com teardown:
- `ena_com_set_admin_running_state(false)`
  [userspace/dpdk/ena/ena_ethdev.c:926].
- `ena_com_rss_destroy`, `ena_com_delete_debug_area`,
  `ena_com_delete_host_info` [userspace/dpdk/ena/ena_ethdev.c:928-931].
- `ena_com_abort_admin_commands` + `ena_com_wait_for_abort_completion`
  [userspace/dpdk/ena/ena_ethdev.c:933-934].
- `ena_com_admin_destroy` — frees admin SQ/CQ and the AENQ ring
  [userspace/dpdk/ena/ena_ethdev.c:935; userspace/dpdk/ena/ena_com.c:1707-1735].
- `ena_com_mmio_reg_read_request_destroy` — **writes `MMIO_RESP_LO/HI = 0`**
  and frees the response buffer
  [userspace/dpdk/ena/ena_ethdev.c:936; userspace/dpdk/ena/ena_com.c:2043-2058].

Note `ena_com_admin_destroy` does **not** issue a `DEV_CTL` reset; it just frees
host memory and clears register-programmed base addresses indirectly via the
MMIO-RESP zero write. A full device reset only happens on the reset path below.

### Reset path (`ena_dev_reset` / `eth_ena_dev_uninit` → re-init)
`ena_dev_reset` calls `eth_ena_dev_uninit` (→ `ena_close`) then re-runs init
[userspace/dpdk/ena/ena_ethdev.c:948-960]. The actual hardware reset is issued
by `ena_com_dev_reset(ena_dev, adapter->reset_reason)` inside the
start/restart flow [userspace/dpdk/ena/ena_ethdev.c:1383]. Mechanism:
[registers.md](registers.md) § "Device reset sequence".

### Reset triggers (reason written into `DEV_CTL.RESET_REASON`)
`ena_trigger_reset` records the reason; the timer-based watchdog and datapath
detect failures and request a reset:

| Trigger | Reason enum | Evidence |
|---------|-------------|----------|
| Keep-alive timeout | `ENA_REGS_RESET_KEEP_ALIVE_TO` (1) | [userspace/dpdk/ena/ena_ethdev.c:1971-1984] |
| Admin queue not running | `ENA_REGS_RESET_ADMIN_TO` (2) | [userspace/dpdk/ena/ena_ethdev.c:1988-1994] |
| Missing Tx completions over threshold | `ENA_REGS_RESET_MISS_TX_CMPL` (3) | [userspace/dpdk/ena/ena_ethdev.c:2027-2035] |
| Invalid Tx req_id | `ENA_REGS_RESET_INV_TX_REQ_ID` (5) | [userspace/dpdk/ena/ena_ethdev.c:795-797] |
| FATAL_ERROR AENQ event | (driver-specific) | handler table [userspace/dpdk/ena/ena_ethdev.c:4135-4137]; see [aenq.md](aenq.md) §2 |

The reset reason enum and its `DEV_CTL` MSB/LSB encoding (bits 24-27 ext,
28-31 base, gated by `ENA_ADMIN_EXTENDED_RESET_REASONS`) are in
[registers.md](registers.md) § "Reset reason encoding" and
[userspace/dpdk/ena/ena_com.c:2546-2569]. Kernel reset-reason mapping lives in
`ena_get_reset_reason` (used at [kernel/linux/ena/ena_netdev.c:2919, 4530]); the
kernel reset/restart runs under `rtnl_lock`.

The kernel error-init teardown additionally issues
`ena_com_dev_reset(ena_dev, ENA_REGS_RESET_INIT_ERR)` (7) on probe failure
[kernel/linux/ena/ena_netdev.c:5780] and
`ENA_REGS_RESET_DRIVER_INVALID_STATE` (8) on a bad state
[kernel/linux/ena/ena_netdev.c:4633].

---

## 5. Validation checklist for the emulated device

To pass the reference driver's init the device MUST:

1. Expose vendor `0x1d0f` and one of the ENA device IDs (DPDK: `0xEC20`/`0xEC21`)
   [userspace/dpdk/ena/ena_ethdev.c:216-219].
2. Provide BAR0 (registers) and, if LLQ is offered, BAR2 (mem bar)
   [userspace/dpdk/ena/ena_ethdev.c:2311-2312].
3. Implement the readless MMIO_REG_READ handshake (or set the PCI config
   disable bit) and honor the RESP address writes
   [userspace/dpdk/ena/ena_com.c:2010-2070]; see [registers.md](registers.md).
4. Report `DEV_STS.READY` so reset and admin init proceed
   [userspace/dpdk/ena/ena_com.c:2086-2089, 2530-2533].
5. Report a non-zero `CAPS.RESET_TIMEOUT`
   [userspace/dpdk/ena/ena_com.c:2535-2541] and a `CAPS.DMA_ADDR_WIDTH` in
   `[32,48]` [userspace/dpdk/ena/ena_com.c:1626-1629].
6. Report `CONTROLLER_VERSION >= 0.0.1` (impl-id byte ignored)
   [userspace/dpdk/ena/ena_com.c:1677-1686].
7. Implement the `DEV_CTL.DEV_RESET` ↔ `DEV_STS.RESET_IN_PROGRESS` two-phase
   handshake [userspace/dpdk/ena/ena_com.c:2570-2588].
8. Accept admin AQ/ACQ/AENQ base + caps register programming and run the admin
   queue [userspace/dpdk/ena/ena_com.c:2106-2155]; see
   [queue-setup.md](queue-setup.md) and [aenq.md](aenq.md).
9. Answer admin `GET_FEATURE DEVICE_ATTRIBUTES` with a valid mac/mtu/feature
   bitmap, plus MAX_QUEUES(_EXT), AENQ_CONFIG, STATELESS_OFFLOAD_CONFIG (and
   may return unsupported for HW_HINTS / LLQ / INTERRUPT_MODERATION)
   [userspace/dpdk/ena/ena_com.c:2298-2382, 3400-3416].
10. Accept `SET_FEATURE HOST_ATTR_CONFIG` and `SET_FEATURE AENQ_CONFIG` (or
    cleanly return unsupported for host-attr)
    [userspace/dpdk/ena/ena_com.c:3305-3352; aenq.md §5].
11. Emit `LINK_CHANGE` (and periodic `KEEP_ALIVE`) AENQ events so the port
    comes up and the watchdog stays satisfied
    [userspace/dpdk/ena/ena_ethdev.c:4041-4092]; see [aenq.md](aenq.md) §7.

---

## Claim classification

- **Explicit** (direct from code/headers): all PCI IDs, BAR indices, register
  offsets/writes, struct layouts, constants (queue depths, min ctrl version,
  DMA width bounds, timeouts), the ordered call chain in `ena_device_init`, and
  reset-reason triggers — each carries a file:line.
- **Inferred**: that the LLQ "variant" device IDs do not by themselves select
  LLQ in DPDK (concluded from DPDK binding only the two VF IDs at
  [userspace/dpdk/ena/ena_ethdev.c:216-219] while LLQ is negotiated via the
  `ENA_ADMIN_LLQ` feature in `ena_com_get_dev_attr_feat`
  [userspace/dpdk/ena/ena_com.c:2370-2378]); that MSI-X vector 0 is shared by
  admin completion and AENQ (from the single `ena_control_path_handler`
  servicing both [userspace/dpdk/ena/ena_ethdev.c:1942-1951] and the
  `ENA_ADMIN_MSIX_VEC` + io-queues layout
  [kernel/linux/ena/ena_netdev.h:57]).
- **Ambiguous / open**:
  - DPDK `os_type` is set via a macro that expands to literal `3`
    [userspace/dpdk/ena/ena_ethdev.c:815; base/ena_plat_dpdk.h `#define n 3`];
    the canonical `enum ena_admin_os_type` is not present in the provided
    headers, so the numeric meaning of `3` (`OS_DPDK`?) is unconfirmed from
    source here.
  - DPDK does not read a `LINK_CONFIG` get-feature for link speed (reports
    `SPEED_NUM_NONE`); whether the emulated device must implement
    `LINK_CONFIG` get-feature at all depends on the kernel driver path
    [userspace/dpdk/ena/ena_ethdev.c:1065] — not exercised by DPDK init.
