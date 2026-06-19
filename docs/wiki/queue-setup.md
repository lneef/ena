# IO Queue Setup (CREATE/DESTROY SQ & CQ)

How the driver builds and tears down a datapath queue pair via the admin
queue, what the device must return, and the ordering / handle conventions.

Reference driver: AWS DPDK PMD (`userspace/dpdk/ena`). Paths below are
relative to `amzn-drivers/`.

For the on-wire TX SQ descriptor, TX metadata descriptor, and TX completion
descriptor bit layouts (the contents of the SQ/CQ slots created here) see
[tx-descriptors.md](tx-descriptors.md).

---

## 1. Queue-pair model and qid convention

Each datapath queue is a **(CQ, SQ) pair**. The CQ holds completion
descriptors written by the device; the SQ holds submission descriptors
(Tx descriptors / Rx buffer descriptors) posted by the driver.

- The driver keeps **separate Tx and Rx queues**, each with its own qid.
  TX qid = `2*q`, RX qid = `2*q + 1`.
  [userspace/dpdk/ena/ena_ethdev.h:L76-L77] (explicit)
  ```c
  #define ENA_IO_TXQ_IDX(q)   (2 * (q))
  #define ENA_IO_RXQ_IDX(q)   (2 * (q) + 1)
  ```
- Max addressable queue slots in the driver:
  `ENA_MAX_NUM_IO_QUEUES = 128`, `ENA_TOTAL_NUM_QUEUES = 256`
  (Tx and Rx counted separately). qid passed to admin commands must be
  `< ENA_TOTAL_NUM_QUEUES`.
  [userspace/dpdk/ena/base/ena_com.h:L11-L13],
  [userspace/dpdk/ena/base/ena_com.c:L2169-L2173] (explicit)

Note: the `qid` above is the **driver-side array index**. It is NOT the
device-assigned queue index. The device assigns `cq_idx` / `sq_idx` in the
CREATE responses (see below). (explicit, see L1314/L1451)

### Memory addresses

All `ena_common_mem_addr` are 48-bit, split low32 / high16:
[userspace/dpdk/ena/base/ena_defs/ena_common_defs.h:L12-L20] (explicit)
```c
struct ena_common_mem_addr {
    uint32_t mem_addr_low;
    uint16_t mem_addr_high;
    uint16_t reserved16;   /* MBZ */
};
```

---

## 2. Ordering constraint: CQ before SQ

`ena_com_create_io_queue()` always creates the CQ first, then the SQ,
passing the device-returned `cq_idx` into the SQ command. On SQ failure it
destroys the CQ; on success both exist.
[userspace/dpdk/ena/base/ena_com.c:L2206-L2220] (explicit)

```c
ret = ena_com_create_io_cq(ena_dev, io_cq);          /* CQ first  */
...
ret = ena_com_create_io_sq(ena_dev, io_sq, io_cq->idx); /* then SQ */
```

The command header itself documents the requirement: "associated completion
queue id. This CQ must be created prior to SQ creation".
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L282-L285] (explicit)

Admin opcodes:
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L30-L33] (explicit)
```
ENA_ADMIN_CREATE_SQ  = 1
ENA_ADMIN_DESTROY_SQ = 2
ENA_ADMIN_CREATE_CQ  = 3
ENA_ADMIN_DESTROY_CQ = 4
```

---

## 3. CREATE_CQ (opcode 3)

### Command: `ena_admin_aq_create_cq_cmd`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L342-L368] (explicit)
```c
struct ena_admin_aq_create_cq_cmd {
    struct ena_admin_aq_common_desc aq_common_descriptor;
    uint8_t  cq_caps_1;   /* bit5: interrupt_mode_enabled */
    uint8_t  cq_caps_2;   /* bits4:0: cq_entry_size_words (valid 4 or 8) */
    uint16_t cq_depth;    /* # entries, must be power of 2 */
    uint32_t msix_vector; /* msix vector assigned to this cq */
    struct ena_common_mem_addr cq_ba; /* CQ phys base, must be contiguous */
};
```

`cq_caps_1` / `cq_caps_2` bitfields:
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L1316-L1319] (explicit)
```
INTERRUPT_MODE_ENABLED : cq_caps_1 bit5  (1=interrupt mode, 0=polling)
CQ_ENTRY_SIZE_WORDS    : cq_caps_2 bits4:0  (size of a CQ entry in 32-bit words)
```

What the driver sets:
[userspace/dpdk/ena/base/ena_com.c:L1423-L1435] (explicit)
- `opcode = ENA_ADMIN_CREATE_CQ`
- `cq_caps_2 = (cdesc_entry_size_in_bytes / 4) & 0x1F` — entry size in words.
  Tx CQ uses `sizeof(ena_eth_io_tx_cdesc)`, Rx CQ uses
  `sizeof(ena_eth_io_rx_cdesc_base)`.
  [userspace/dpdk/ena/base/ena_com.c:L424-L427]
- `cq_caps_1 |= INTERRUPT_MODE_ENABLED` — **always set unconditionally**
  in this driver (interrupt mode is requested for every IO CQ; whether the
  vector is actually used is a separate driver decision).
- `msix_vector = io_cq->msix_vector` (set to `-1` for Tx and for Rx when
  datapath interrupts are disabled). [ena_ethdev.c:L1423-L1436]
- `cq_depth = io_cq->q_depth` (the ring size; a power of 2, see limits §7).
- `cq_ba` = physical base of the driver-allocated, contiguous CQ ring.

### Response: `ena_admin_acq_create_cq_resp_desc`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L370-L383] (explicit)
```c
struct ena_admin_acq_create_cq_resp_desc {
    struct ena_admin_acq_common_desc acq_common_desc;
    uint16_t cq_idx;                            /* device-assigned CQ index */
    uint16_t cq_actual_depth;                   /* actual depth granted */
    uint32_t numa_node_register_offset;         /* offset into REG BAR */
    uint32_t cq_head_db_register_offset;        /* offset into REG BAR */
    uint32_t cq_interrupt_unmask_register_offset;/* offset into REG BAR */
};
```

What the driver consumes:
[userspace/dpdk/ena/base/ena_com.c:L1451-L1459] (explicit)
- `io_cq->idx = cq_idx`. This index is later passed as the SQ's `cq_idx`.
- `unmask_reg = reg_bar + cq_interrupt_unmask_register_offset` — MMIO
  register the driver writes to unmask the CQ's interrupt.
- `numa_node_cfg_reg = reg_bar + numa_node_register_offset` — only stored if
  the offset is non-zero. Written via `ena_com_update_numa_node`.
  [userspace/dpdk/ena/base/ena_eth_com.h:L183-L197]

Notes (device behavior, inferred from how the driver uses the response):
- `cq_head_db_register_offset` is returned by the device but this DPDK
  driver does **not** store/use it for IO CQs (no head doorbell write in the
  IO path; completions are consumed by phase bit). The field exists in the
  layout and must be returned. (inferred — driver reads only unmask/numa at
  L1451-L1459; cq_head_db_register_offset never dereferenced for IO CQs)
- `cq_actual_depth` is the depth actually granted; the driver requests a
  power-of-2 depth (§7) and treats `q_depth` as authoritative thereafter.
  (inferred)
- All three returned offsets are **offsets into the PCIe REG BAR**, so the
  device chooses where these per-CQ registers live. (explicit — comments at
  L378-L382 say "register offset"; driver adds `reg_bar`)

---

## 4. CREATE_SQ (opcode 1)

### Command: `ena_admin_aq_create_sq_cmd`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L248-L304] (explicit)
```c
struct ena_admin_aq_create_sq_cmd {
    struct ena_admin_aq_common_desc aq_common_descriptor;
    uint8_t  sq_identity;  /* bits7:5: sq_direction (1=Tx, 2=Rx) */
    uint8_t  reserved8_w1;
    uint8_t  sq_caps_2;    /* bits3:0: placement_policy
                              bits6:4: completion_policy */
    uint8_t  sq_caps_3;    /* bit0: is_physically_contiguous */
    uint16_t cq_idx;       /* CQ this SQ feeds; CQ created first */
    uint16_t sq_depth;     /* # entries */
    struct ena_common_mem_addr sq_ba;            /* SQ phys base (HOST only,
                                                    page aligned) */
    struct ena_common_mem_addr sq_head_writeback;/* head writeback location
                                                    (head completion policy) */
    uint32_t reserved0_w7;
    uint32_t reserved0_w8;
};
```

Bitfields:
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L1308-L1314] (explicit)
```
SQ_DIRECTION            : sq_identity bits7:5
PLACEMENT_POLICY        : sq_caps_2   bits3:0
COMPLETION_POLICY       : sq_caps_2   bits6:4
IS_PHYSICALLY_CONTIGUOUS: sq_caps_3   bit0
```

Enums:
- Direction [L306-L309]: `TX=1`, `RX=2`.
- Placement policy [L91-L98]: `HOST=1` (descriptors+headers in host memory),
  `DEV=3` (descriptors+headers in **device** memory — Low Latency Queue/LLQ).
- Completion policy [L113-L126]:
  `DESC=0` (cqe per sq desc), `DESC_ON_DEMAND=1`,
  `HEAD_ON_DEMAND=2` (head writeback to host on request),
  `HEAD=3` (head writeback to host every desc).

What the driver sets:
[userspace/dpdk/ena/base/ena_com.c:L1268-L1302] (explicit)
- `opcode = ENA_ADMIN_CREATE_SQ`
- `sq_identity` direction = TX(1)/RX(2) from `io_sq->direction`.
- `sq_caps_2` placement = `io_sq->mem_queue_type` (HOST or DEV).
- `sq_caps_2` completion policy = **`ENA_ADMIN_COMPLETION_POLICY_DESC` (0)**
  always — one CQE per SQ descriptor. Consequently `sq_head_writeback` is
  left zero (writeback is only relevant for HEAD/HEAD_ON_DEMAND policies).
  (explicit — only DESC is ever written; sq_head_writeback never populated)
- `sq_caps_3` = `IS_PHYSICALLY_CONTIGUOUS` — always set.
- `cq_idx` = the `io_cq->idx` returned by CREATE_CQ.
- `sq_depth` = `io_sq->q_depth`.
- `sq_ba` = SQ ring physical base **only when placement == HOST**. For LLQ
  (DEV) placement, `sq_ba` is left zero because the descriptor ring lives in
  device memory. [ena_com.c:L1294-L1302]

### Response: `ena_admin_acq_create_sq_resp_desc`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L311-L330] (explicit)
```c
struct ena_admin_acq_create_sq_resp_desc {
    struct ena_admin_acq_common_desc acq_common_desc;
    uint16_t sq_idx;                /* device-assigned SQ index */
    uint16_t reserved;
    uint32_t sq_doorbell_offset;    /* offset into REG BAR */
    uint32_t llq_descriptors_offset;/* offset into LLQ_MEM BAR */
    uint32_t llq_headers_offset;    /* offset into LLQ_MEM BAR */
};
```

What the driver consumes:
[userspace/dpdk/ena/base/ena_com.c:L1314-L1323] (explicit)
- `io_sq->idx = sq_idx`.
- `io_sq->db_addr = reg_bar + sq_doorbell_offset` — the per-SQ doorbell
  register. The device chooses the offset; driver writes the SQ tail here.
- For LLQ (DEV) placement only:
  `io_sq->desc_addr.pbuf_dev_addr = mem_bar + llq_descriptors_offset` — the
  in-device descriptor ring the driver writes descriptors into.
- `llq_headers_offset` is part of the layout but **not consumed** by this
  driver in `ena_com_create_io_sq` (headers are handled through the LLQ
  bounce-buffer / desc_list_entry layout negotiated separately). (inferred —
  field present in struct, not dereferenced at L1314-L1323)

### IO doorbell register layout (offset computation)

The device returns an absolute REG-BAR offset per SQ
(`sq_doorbell_offset`); the driver does **not** compute the doorbell offset
from `sq_idx` itself — it simply uses `reg_bar + sq_doorbell_offset`.
[userspace/dpdk/ena/base/ena_com.c:L1316-L1317] (explicit)

The doorbell write is a single 32-bit MMIO store of the SQ tail value:
[userspace/dpdk/ena/base/ena_eth_com.h:L162-L181] (explicit)
```c
ENA_REG_WRITE32(io_sq->bus, tail, io_sq->db_addr);  /* tail = io_sq->tail */
```
So the doorbell semantics are: writing the current SQ tail index tells the
device how many descriptors are now valid. (explicit)

---

## 5. DESTROY_SQ (opcode 2)

### Command: `ena_admin_aq_destroy_sq_cmd`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L332-L336] (explicit)
```c
struct ena_admin_aq_destroy_sq_cmd {
    struct ena_admin_aq_common_desc aq_common_descriptor;
    struct ena_admin_sq sq;   /* { uint16 sq_idx; uint8 sq_identity; ... } */
};
```
`ena_admin_sq` [L197-L206]: `sq_idx`, plus `sq_identity` bits7:5 = direction.

What the driver sets:
[userspace/dpdk/ena/base/ena_com.c:L927-L940] (explicit)
- `opcode = ENA_ADMIN_DESTROY_SQ`
- `sq.sq_idx = io_sq->idx` (the **device-assigned** index from CREATE_SQ).
- `sq.sq_identity` direction = TX(1)/RX(2).

So DESTROY_SQ identifies the queue by its device-assigned `sq_idx` plus
direction. (explicit)

---

## 6. DESTROY_CQ (opcode 4)

### Command: `ena_admin_aq_destroy_cq_cmd`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L385-L391] (explicit)
```c
struct ena_admin_aq_destroy_cq_cmd {
    struct ena_admin_aq_common_desc aq_common_descriptor;
    uint16_t cq_idx;       /* device-assigned CQ index */
    uint16_t reserved1;
};
```

What the driver sets:
[userspace/dpdk/ena/base/ena_com.c:L1525-L1528] (explicit)
- `opcode = ENA_ADMIN_DESTROY_CQ`
- `cq_idx = io_cq->idx`.

### Destruction ordering

`ena_com_destroy_io_queue()` destroys the **SQ first, then the CQ**
(reverse of creation), then frees host rings.
[userspace/dpdk/ena/base/ena_com.c:L2237-L2240] (explicit)
```c
ena_com_destroy_io_sq(ena_dev, io_sq);
ena_com_destroy_io_cq(ena_dev, io_cq);
ena_com_io_queue_free(ena_dev, io_sq, io_cq);
```
Both destroy commands tolerate `ENA_COM_NO_DEVICE` (device already gone).
[ena_com.c:L948-L949, L1536]

---

## 7. Limits negotiation (MAX_QUEUES_NUM vs MAX_QUEUES_EXT)

Two feature IDs describe queue limits; the driver prefers the EXT form when
the device advertises it in `supported_features`.
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L54,L59],
[userspace/dpdk/ena/ena_ethdev.c:L1129] (explicit)
```
ENA_ADMIN_MAX_QUEUES_NUM = 2   (legacy)
ENA_ADMIN_MAX_QUEUES_EXT = 7   (preferred; separate Tx/Rx limits)
```

### EXT form: `ena_admin_queue_ext_feature_fields`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L709-L735] (explicit)
```
max_tx_sq_num, max_tx_cq_num, max_rx_sq_num, max_rx_cq_num   (# queues)
max_tx_sq_depth, max_tx_cq_depth, max_rx_sq_depth, max_rx_cq_depth (depths)
max_tx_header_size                       (Tx header size limit, bytes)
max_per_packet_tx_descs (incl. meta)     (Tx SGL limit)
max_per_packet_rx_descs                  (Rx SGL limit)
```
Wrapper `ena_admin_queue_ext_feature_desc` carries a `version` byte
[L1059-L1070].

### Legacy form: `ena_admin_queue_feature_desc`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:L737-L759] (explicit)
```
max_sq_num, max_sq_depth, max_cq_num, max_cq_depth
max_legacy_llq_num, max_legacy_llq_depth
max_header_size
max_packet_tx_descs (incl. meta), max_packet_rx_descs
```

### How the driver derives the ring sizes
[userspace/dpdk/ena/ena_ethdev.c:L1129-L1173] (explicit)
- EXT path:
  - `max_rx_queue_size = min(max_rx_cq_depth, max_rx_sq_depth)`
  - `max_tx_queue_size = max_tx_cq_depth`, then
    `min(..., max_llq_depth)` if Tx placement is LLQ(DEV), else
    `min(..., max_tx_sq_depth)`.
  - `max_rx_sgl_size = min(ENA_PKT_MAX_BUFS, max_per_packet_rx_descs)`
  - `max_tx_sgl_size = min(ENA_PKT_MAX_BUFS, max_per_packet_tx_descs)`
- Legacy path mirrors this with the legacy field names.
- Both results are rounded **down to a power of 2**
  (`rte_align32prevpow2`), which is why CREATE_CQ/SQ `*_depth` are powers
  of 2. [ena_ethdev.c:L1171-L1173]

Tx header size limit is also clamped in ena_com when a Tx queue is created:
`tx_max_header_size = min(ena_dev->tx_max_header_size, 256)` ("header length
is limited to 8 bits").
[userspace/dpdk/ena/base/ena_com.c:L2194-L2197] (explicit)

Queue count: `io_rx_num = min(max_rx_sq_num, max_rx_cq_num)`, Tx counts from
`max_tx_sq_num` / `max_tx_cq_num`.
[userspace/dpdk/ena/ena_ethdev.c:L2175-L2180] (explicit)

---

## 8. Driver-side init state (head/tail/phase) and post-create actions

### Before the admin commands (init)
`ena_com_init_io_sq` / `ena_com_init_io_cq` reset the ring tracking BEFORE
the CREATE commands are issued:
[userspace/dpdk/ena/base/ena_com.c:L408-L410, L453-L454] (explicit)
- SQ: `tail = 0`, `next_to_comp = 0`, `phase = 1`.
- CQ: `head = 0`, `phase = 1`.

Descriptor/CDESC entry sizes are direction-dependent:
[userspace/dpdk/ena/base/ena_com.c:L338-L341, L424-L427] (explicit)
- SQ desc entry: `ena_eth_io_tx_desc` (Tx) / `ena_eth_io_rx_desc` (Rx).
- CQ cdesc entry: `ena_eth_io_tx_cdesc` (Tx) / `ena_eth_io_rx_cdesc_base`
  (Rx).

For LLQ (DEV) SQs the init also sets up bounce buffers and copies
`llq_info` (desc_list_entry_size, descs_num_before_header,
max_entries_in_tx_burst). [ena_com.c:L367-L406]

### Phase bit usage (device <-> driver contract)
The CQ phase starts at 1; the driver flips its expected phase each time
`head` wraps the ring. The device writes the matching phase into each CDESC,
so a CDESC is "new" when its phase equals the driver's expected phase.
[userspace/dpdk/ena/base/ena_eth_com.h:L204-L211] (explicit)
```c
io_cq->head++;
if ((io_cq->head & (io_cq->q_depth - 1)) == 0)
    io_cq->phase ^= 1;
```

### After creation
- For Tx queues the driver writes the NUMA-node config register if the
  device returned a `numa_node_register_offset`.
  [ena_ethdev.c:L1464-L1465], [ena_eth_com.h:L183-L197] (explicit)
- For Rx queues with datapath interrupts, the driver starts with the Rx
  interrupt **masked**. [ena_ethdev.c:L1467-L1469] (explicit)
- The first real device activity is the **Rx doorbell**: after a queue
  starts, the driver fills the Rx SQ with buffers and rings the SQ doorbell
  (writes `io_sq->tail`). [ena_ethdev.c:L1530, L1772-L1831] (explicit)
- Tx doorbells are rung from the Tx burst path after posting descriptors.
  [ena_ethdev.c:L3170, L3303] (explicit)

### On destruction
DESTROY_SQ then DESTROY_CQ (admin), then `ena_com_io_queue_free` frees the
host-side CQ cdesc ring, SQ desc ring (HOST only), and LLQ bounce buffers.
[userspace/dpdk/ena/base/ena_com.c:L954-L1000, L2237-L2240] (explicit)

---

## 9. Index/handle conventions summary

| Concept                  | Who assigns | Where it appears |
|--------------------------|-------------|------------------|
| driver qid (array slot)  | driver      | `ENA_IO_TXQ_IDX`/`RXQ_IDX`, bounds `< 256` |
| `cq_idx` (device handle) | **device**  | CREATE_CQ resp -> SQ cmd `cq_idx`, DESTROY_CQ |
| `sq_idx` (device handle) | **device**  | CREATE_SQ resp -> DESTROY_SQ `sq.sq_idx` |
| sq doorbell offset       | **device**  | CREATE_SQ resp, REG BAR offset |
| cq unmask/numa/head_db   | **device**  | CREATE_CQ resp, REG BAR offsets |
| llq desc/header offsets  | **device**  | CREATE_SQ resp, LLQ_MEM BAR offsets |

Pairing: an SQ is bound to its CQ exclusively through the `cq_idx` field in
the CREATE_SQ command, using the device-assigned CQ index. (explicit)

---

## Open ambiguities

- **`cq_head_db_register_offset`**: returned by the device but never used by
  this DPDK driver for IO CQs (no IO CQ head doorbell write found). Whether
  any other ENA driver uses it for IO completions is unverified here; for the
  DPDK reference the device may return any value the driver ignores.
  (inferred from absence of use at ena_com.c:L1451-L1459)
- **`llq_headers_offset`**: present in the CREATE_SQ response but not
  dereferenced in `ena_com_create_io_sq`. Its consumption (if any) lives in
  the LLQ header-placement path negotiated via the LLQ feature, not the
  create path. See [llq.md](llq.md) §5-§6 for header layout (inline-header
  mode means `llq_headers_offset` stays unused).
- **`completion_policy`**: the DPDK driver hard-codes `DESC` (one CQE per
  descriptor) and never uses `sq_head_writeback`. The HEAD / HEAD_ON_DEMAND
  policies and the `sq_head_writeback` address are defined but unexercised by
  this driver.
