# Admin (Control) Queue — AQ / ACQ

The ENA admin queue is a host-to-device command channel: a host-owned
**Admin Submission Queue (AQ)** ring of command descriptors, paired with a
host-resident **Admin Completion Queue (ACQ)** ring the device DMA-writes
results into. The host posts a command, rings the AQ_DB doorbell, and learns
of completion either by an admin interrupt (MSI-X vector 0, shared with the
AENQ) or by polling the ACQ phase bit. Every datapath setup and feature
negotiation goes through this queue.

Unless noted, evidence is from the AWS DPDK PMD
(`userspace/dpdk/ena/base/...`). The Linux kernel and FreeBSD `ena_com`
copies are byte-identical for everything documented here; cross-cites are
given where a kernel-only function matters. Paths are relative to
`~/ena/amzn-drivers/`.

Cross-references (not duplicated here):
- AQ/ACQ/AENQ **register** programming (AQ_BASE_LO/HI, AQ_CAPS, ACQ_BASE,
  ACQ_CAPS, AQ_DB doorbell, INTR_MASK), depth/entry-size field masks, and the
  reset/version handshake live in `registers.md`.
- `CREATE_SQ`/`CREATE_CQ`/`DESTROY_SQ`/`DESTROY_CQ` command and response
  payloads, queue-pair ordering, and the MAX_QUEUES limit negotiation live in
  `queue-setup.md`.
- The AENQ ring, its entry layout, AENQ groups, and the `AENQ_CONFIG` feature
  live in `aenq.md`.

---

## 1. Ring geometry and entry sizes

Both rings have a fixed driver-chosen depth of **32 entries**
(`ENA_ADMIN_QUEUE_DEPTH = 32`), set into `admin_queue->q_depth`.
[userspace/dpdk/ena/base/ena_com.c:15, 2091] (explicit)

The depth must be a power of two — the submission/completion code masks
indices with `q_depth - 1` and flips the phase bit on wrap, which only works
for a power of two. [userspace/dpdk/ena/base/ena_com.c:233-235, 272-273,
496-516] (explicit)

### AQ entry — `struct ena_admin_aq_entry` (64 bytes)
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:208-218] (explicit)
```c
struct ena_admin_aq_entry {
    struct ena_admin_aq_common_desc aq_common_descriptor; /* 4 bytes */
    union {
        uint32_t inline_data_w1[3];                       /* 12 bytes */
        struct ena_admin_ctrl_buff_info control_buffer;   /* 12 bytes */
    } u;
    uint32_t inline_data_w4[12];                          /* 48 bytes */
};                                                        /* total 64 */
```
Layout: 4-byte common header, then a 12-byte union (either three inline data
words OR a control-buffer descriptor), then 48 bytes of further inline data.
Total **64 bytes**. The device is told this size via the AQ_CAPS ENTRY_SIZE
field = `sizeof(struct ena_admin_aq_entry)` (see `registers.md`).
[userspace/dpdk/ena/base/ena_com.c:2133-2135] (explicit)

### ACQ entry — `struct ena_admin_acq_entry` (64 bytes)
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:242-246] (explicit)
```c
struct ena_admin_acq_entry {
    struct ena_admin_acq_common_desc acq_common_descriptor; /* 8 bytes */
    uint32_t response_specific_data[14];                    /* 56 bytes */
};                                                          /* total 64 */
```
8-byte common completion header + 56 bytes of command-specific response.
Total **64 bytes**, reported via ACQ_CAPS ENTRY_SIZE =
`sizeof(struct ena_admin_acq_entry)`.
[userspace/dpdk/ena/base/ena_com.c:2141-2143] (explicit)

Both rings are allocated as DMA-coherent host memory; the AQ base is written
to AQ_BASE_LO/HI and the ACQ base to ACQ_BASE_LO/HI (see `registers.md`).
SQ memory is allocated in `ena_com_admin_init_sq`, CQ in
`ena_com_admin_init_cq`. [userspace/dpdk/ena/base/ena_com.c:93-134] (explicit)

---

## 2. Common descriptors and bitfields

### AQ common descriptor — `struct ena_admin_aq_common_desc` (4 bytes)
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:168-185] (explicit)
```c
struct ena_admin_aq_common_desc {
    uint16_t command_id;  /* bits 11:0 command_id; 15:12 reserved */
    uint8_t  opcode;      /* enum ena_admin_aq_opcode */
    uint8_t  flags;       /* bit0 phase; bit1 ctrl_data;
                             bit2 ctrl_data_indirect; 7:3 reserved */
};
```
Field masks [ena_admin_defs.h:1293-1298] (explicit):
```
COMMAND_ID          : command_id bits 11:0   (mask GENMASK(11,0))
PHASE               : flags bit0              (BIT(0))
CTRL_DATA           : flags bit1              (BIT(1)) - control buffer addr valid
CTRL_DATA_INDIRECT  : flags bit2              (BIT(2)) - addr points to a page list
```
- `phase` — submission phase bit (toggles each AQ wrap), see §3.
- `command_id` — host-assigned tag (0..q_depth-1) echoed back in the
  completion's `command` field, see §3.
- `ctrl_data` / `ctrl_data_indirect` — control-buffer mode selectors, see §6.

### ACQ common descriptor — `struct ena_admin_acq_common_desc` (8 bytes)
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:220-240] (explicit)
```c
struct ena_admin_acq_common_desc {
    uint16_t command;          /* bits 11:0 command_id (echoes AQ); 15:12 rsvd */
    uint8_t  status;           /* enum ena_admin_aq_completion_status */
    uint8_t  flags;            /* bit0 phase; 7:1 reserved */
    uint16_t extended_status;  /* additional status (e.g. for ILLEGAL_PARAMETER) */
    uint16_t sq_head_indx;     /* AQ entry the device has consumed/freed */
};
```
Field masks [ena_admin_defs.h:1304-1306] (explicit):
```
COMMAND_ID : command bits 11:0  (mask GENMASK(11,0))
PHASE      : flags bit0         (BIT(0))
```
- `command` — must equal the `command_id` the host put in the matching AQ
  entry; the driver uses it to find the waiting command context
  [ena_com.c:465-468] (explicit).
- `status` — primary completion status (see §4 status enum).
- `extended_status` — "Additional status is provided in ACQ entry
  extended_status" for `ILLEGAL_PARAMETER` and similar; the header comment
  documents this [ena_admin_defs.h:45-46] (explicit). The DPDK/Linux com
  layer stores only `status` (not extended_status) into the command context,
  so the driver does not act on `extended_status` itself
  [ena_com.c:479-480] (inferred — field defined, only `status` consumed).
- `sq_head_indx` — "indicates to the driver which AQ entry has been consumed
  by the device and could be reused" [ena_admin_defs.h:236-239] (explicit).
  See §4 for how head accounting actually works in the reference driver.

---

## 3. Submission semantics

Submission is done by `__ena_com_submit_admin_cmd`
[userspace/dpdk/ena/base/ena_com.c:222-280] (explicit). The host-side ring
state is `struct ena_com_admin_sq { entries; head; tail; phase }`
(`phase` initialised to 1, head/tail to 0 in `ena_com_admin_init_sq`)
[ena_com.c:107-109; ena_com.h:187-198] (explicit).

Step by step:

1. **Queue-full check.** `outstanding_cmds` (an atomic counter) is read; if
   `>= q_depth` the submit returns `ENA_COM_NO_SPACE` and bumps the
   `out_of_space` stat. So the admin queue is considered full when 32
   commands are outstanding (not yet completed), independent of head/tail
   position. [ena_com.c:237-243] (explicit)

2. **Phase bit.** The current SQ phase is OR-ed into the command's
   `flags` bit0: `cmd->flags |= sq.phase & PHASE_MASK`. The device uses this
   per-entry phase bit to know the entry is freshly produced.
   [ena_com.c:247-248] (explicit)

3. **command_id allocation.** `cmd_id = admin_queue->curr_cmd_id`; it is
   written into `cmd->command_id` (bits 11:0). `curr_cmd_id` then advances
   `(curr_cmd_id + 1) & (q_depth - 1)`, i.e. a monotonic counter modulo 32.
   The id indexes the per-command completion context array (`comp_ctx`),
   which is captured (`occupied = true`, `outstanding_cmds++`) by
   `get_comp_ctxt(..., capture=true)`. A capture of an already-occupied id
   fails. [ena_com.c:245-253, 192-220, 266-267] (explicit)

4. **Write the entry.** The whole command (`cmd_size_in_bytes`) is `memcpy`'d
   into `sq.entries[tail & (q_depth-1)]`. [ena_com.c:264] (explicit)

5. **Advance tail + flip phase on wrap.** `sq.tail++` (a free-running 16-bit
   counter). When `(sq.tail & (q_depth-1)) == 0` the ring wrapped, so
   `sq.phase = !sq.phase`. [ena_com.c:269-273] (explicit)

6. **Doorbell.** After a DMA sync, the **absolute** `sq.tail` value is written
   to AQ_DB (`sq.db_addr = reg_bar + ENA_REGS_AQ_DB_OFF`). Writing the new
   tail tells the device how many AQ entries are now valid.
   [ena_com.c:275-277, 2114-2115] (explicit). AQ_DB register details:
   `registers.md`.

The public entry point `ena_com_submit_admin_cmd` wraps the above under the
admin spinlock and refuses to submit if `running_state` is false (returns
`ENA_COM_NO_DEVICE`); a failed submit sets `running_state = false`.
[ena_com.c:304-327] (explicit)

`ena_com_execute_admin_command` is the synchronous helper: submit, then wait
for completion via `ena_com_wait_and_process_admin_cq`.
[ena_com.c:1382-1411] (explicit)

---

## 4. Completion semantics

### Phase-bit detection (no tail register read)
`ena_com_handle_admin_completion` walks the ACQ
[userspace/dpdk/ena/base/ena_com.c:489-524] (explicit). The CQ state is
`struct ena_com_admin_cq { entries; head; phase }` with `phase = 1`,
`head = 0` at init [ena_com.c:130-131; ena_com.h:178-185] (explicit).

- It reads `cqe = cq.entries[head & (q_depth-1)]` and loops while the entry's
  `flags` bit0 (phase) equals the driver's expected `cq.phase`. A completion
  is "new" exactly when its phase bit matches. [ena_com.c:496-503] (explicit)
- A `dma_rmb()` is issued after the phase check, before reading the rest of
  the entry, so the device must write the entry body before (or atomically
  with) flipping the phase bit. [ena_com.c:504-507] (explicit)
- On each consumed entry `head` advances; when `head` reaches `q_depth` it
  wraps to 0 and the expected `phase` toggles. [ena_com.c:510-516] (explicit)

The ACQ_TAIL register (0x30) exists but is never read — completions are found
purely by phase bit. [registers.md] (explicit)

### Per-completion handling + head accounting
`ena_com_handle_single_admin_completion`
[userspace/dpdk/ena/base/ena_com.c:459-487] (explicit):
- `cmd_id = cqe->command & COMMAND_ID_MASK`; look up `comp_ctx[cmd_id]`.
- If the context isn't `occupied`, the completion is ignored.
- Else `status = ENA_CMD_COMPLETED`, `comp_status = cqe->status`, and the
  full `cqe` is `memcpy`'d into the caller's response buffer
  (`comp_size` bytes). In interrupt mode the waiter is then signalled.

After the loop, head accounting is bulk:
```c
cq.head  += comp_num;   /* ACQ consumer advanced */
cq.phase  = phase;
sq.head  += comp_num;   /* AQ entries now reclaimable */
```
[ena_com.c:520-523] (explicit). So the reference driver derives the freed-AQ
position from the **number of completions processed**, advancing `sq.head` by
that count — it does **not** read `sq_head_indx` from the completion to drive
reclaim (the field is defined and DMA-written by the device, but the DPDK/
Linux com layer never reads it). [ena_com.c:459-524] (inferred — no read of
`sq_head_indx` anywhere in the completion path). `outstanding_cmds` is
decremented separately when each command context is released
(`comp_ctxt_release`). [ena_com.c:184-190, 606] (explicit)

### Completion status codes — `enum ena_admin_aq_completion_status`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:39-49] (explicit)

| Val | Name | Driver errno mapping [ena_com.c:526-549] |
|-----|------|------------------------------------------|
| 0 | SUCCESS | ENA_COM_OK |
| 1 | RESOURCE_ALLOCATION_FAILURE | ENA_COM_NO_MEM |
| 2 | BAD_OPCODE | ENA_COM_INVAL |
| 3 | UNSUPPORTED_OPCODE | ENA_COM_UNSUPPORTED |
| 4 | MALFORMED_REQUEST | ENA_COM_INVAL |
| 5 | ILLEGAL_PARAMETER | ENA_COM_INVAL (extra info in `extended_status`) |
| 6 | UNKNOWN_ERROR | ENA_COM_INVAL |
| 7 | RESOURCE_BUSY | ENA_COM_TRY_AGAIN |

Any non-zero status is logged as an admin command failure.
[ena_com.c:529-531] (explicit)

### Polling vs interrupt mode
`ena_com_wait_and_process_admin_cq` dispatches on `admin_queue->polling`
[ena_com.c:907-916] (explicit):

- **Polling** (`polling = true`): `ena_com_wait_and_process_admin_cq_polling`
  repeatedly calls `ena_com_handle_admin_completion` under the lock until the
  command's context leaves `ENA_CMD_SUBMITTED`, with exponential backoff
  (`ENA_MIN_ADMIN_POLL_US`=100 .. `ENA_MAX_ADMIN_POLL_US`=5000 us) up to the
  `completion_timeout`. On timeout it sets `running_state = false` and returns
  `ENA_COM_TIMER_EXPIRED`. [ena_com.c:560-608, 39-41] (explicit)
- **Interrupt** (`polling = false`, the init default): the submitter sleeps on
  a wait event with the same timeout; the device's admin interrupt handler
  (vector 0) calls `ena_com_handle_admin_completion`, which signals the
  waiter. [ena_com.c:779-786, 485-486, 2095] (explicit)

Polling mode is selected with `ena_com_set_admin_polling_mode`, which also
masks/unmasks the admin interrupt via INTR_MASK (see `registers.md`).
[ena_com.c:1737-1747] (explicit). The `completion_timeout` itself is derived
from the CAPS ADMIN_CMD_TO register field after reset (see `registers.md`).

---

## 5. Admin opcodes

`enum ena_admin_aq_opcode`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:29-37] (explicit):

| Op | Name | Command struct | Response struct | Documented in |
|----|------|----------------|-----------------|---------------|
| 1 | CREATE_SQ | `ena_admin_aq_create_sq_cmd` | `ena_admin_acq_create_sq_resp_desc` | `queue-setup.md` §4 |
| 2 | DESTROY_SQ | `ena_admin_aq_destroy_sq_cmd` | `ena_admin_acq_destroy_sq_resp_desc` | `queue-setup.md` §5 |
| 3 | CREATE_CQ | `ena_admin_aq_create_cq_cmd` | `ena_admin_acq_create_cq_resp_desc` | `queue-setup.md` §3 |
| 4 | DESTROY_CQ | `ena_admin_aq_destroy_cq_cmd` | `ena_admin_acq_destroy_cq_resp_desc` | `queue-setup.md` §6 |
| 8 | GET_FEATURE | `ena_admin_get_feat_cmd` | `ena_admin_get_feat_resp` | §6 below |
| 9 | SET_FEATURE | `ena_admin_set_feat_cmd` | `ena_admin_set_feat_resp` | §6 below |
| 11 | GET_STATS | `ena_admin_aq_get_stats_cmd` | `ena_admin_acq_get_stats_resp` | §7 below |

All command structs begin with `ena_admin_aq_common_desc`; all response
structs begin with `ena_admin_acq_common_desc`, so the §2..§4 phase/
command_id/status mechanics apply uniformly regardless of opcode.

---

## 6. GET_FEATURE / SET_FEATURE mechanics

### Control buffer descriptor — `struct ena_admin_ctrl_buff_info` (12 bytes)
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:191-195] (explicit)
```c
struct ena_admin_ctrl_buff_info {
    uint32_t length;                    /* control buffer length in bytes */
    struct ena_common_mem_addr address; /* 48-bit DMA addr (low32 + high16) */
};
```
This sits in the AQ entry's first union slot. When a command needs a payload
larger than the inline space (e.g. an RSS indirection table, extended stats,
RSS key/hash control), the host puts the buffer's DMA address + length here
and sets the `ctrl_data` / `ctrl_data_indirect` flag bits in the common
descriptor. `ena_admin_ctrl_buff_info` is also the chaining element at the
end of an indirect-mode page-list chunk. [ena_admin_defs.h:187-195] (explicit)

- `ctrl_data` (flags bit1): control buffer address is valid (direct buffer).
- `ctrl_data_indirect` (flags bit2): the address points to a **list of pages**
  each holding addresses of control buffers (scatter list).
  [ena_admin_defs.h:177-183] (explicit)

In practice the reference driver always sets `CTRL_DATA_INDIRECT` (not plain
`CTRL_DATA`) whenever it attaches a control buffer to a GET/SET feature
command. [ena_com.c:1054-1056 (GET), 3104-3105 (SET ind table)] (explicit)

### GET_FEATURE command — `struct ena_admin_get_feat_cmd`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1049-1057] (explicit)
```c
struct ena_admin_get_feat_cmd {
    struct ena_admin_aq_common_desc aq_common_descriptor;
    struct ena_admin_ctrl_buff_info control_buffer;
    struct ena_admin_get_set_feature_common_desc feat_common;
    uint32_t raw[11];
};
```

### SET_FEATURE command — `struct ena_admin_set_feat_cmd`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1147-1181] (explicit)
Same first three members as GET, then a `u` union of per-feature payload
structs (mtu, host_attr, aenq, flow_hash_func, flow_hash_input, ind_table,
llq, phc) or `raw[11]`.

### Feature common header — `struct ena_admin_get_set_feature_common_desc`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:538-555] (explicit)
```c
struct ena_admin_get_set_feature_common_desc {
    uint8_t flags;            /* bits1:0 select: 0x1 current, 0x3 default */
    uint8_t feature_id;       /* enum ena_admin_aq_feature_id */
    uint8_t feature_version;  /* driver max version; device replies actual.
                                 zero based */
    uint8_t reserved8;
};
```
`SELECT_MASK = GENMASK(1,0)` of `flags` [ena_admin_defs.h:1321-1322]. The
`feature_version` field is how driver and device negotiate the per-feature
ABI: "The driver specifies the max feature version it supports and the device
responds with the currently supported feature version" [ena_admin_defs.h:
548-552] (explicit). Example: LLQ is fetched at
`ENA_ADMIN_LLQ_FEATURE_VERSION_1`, MAX_QUEUES_EXT at
`ENA_FEATURE_MAX_QUEUE_EXT_VER` (1). [ena_com.c:2316-2318; kernel ena_com.c
2407-2408] (explicit)

### GET path — `ena_com_get_feature_ex` / `ena_com_get_feature`
[userspace/dpdk/ena/base/ena_com.c:1033-1098] (explicit)
- `opcode = ENA_ADMIN_GET_FEATURE`.
- If a control-buffer size is supplied, `flags = CTRL_DATA_INDIRECT_MASK` and
  the buffer address+length are filled; otherwise `flags = 0`.
  [ena_com.c:1052-1068] (explicit)
- `feat_common.feature_id` and `feature_version` set from arguments.
- Before issuing, `ena_com_check_supported_feature_id` rejects unsupported
  features (except DEVICE_ATTRIBUTES which is "always supported") by testing
  bit `feature_id` of `ena_dev->supported_features`. [ena_com.c:1020-1031]
  (explicit)

### GET response — `struct ena_admin_get_feat_resp`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1109-1145] (explicit)
`acq_common_desc` then a union over all feature descriptors
(`raw[14]`, dev_attr, llq, max_queue, max_queue_ext, aenq, link, offload,
flow_hash_func, flow_hash_input, ind_table, intr_moderation, hw_hints, phc,
extra_properties_*). The 56-byte response area holds the selected member.

### SET response — `struct ena_admin_set_feat_resp`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:1183-1189] (explicit) —
just `acq_common_desc` + `u.raw[14]` (status is the meaningful part).

### Feature IDs — `enum ena_admin_aq_feature_id`
[userspace/dpdk/ena/base/ena_defs/ena_admin_defs.h:52-71] (explicit)

| ID | Name | ID | Name |
|----|------|----|------|
| 1 | DEVICE_ATTRIBUTES | 12 | RSS_INDIRECTION_TABLE_CONFIG |
| 2 | MAX_QUEUES_NUM | 14 | MTU |
| 3 | HW_HINTS | 18 | RSS_HASH_INPUT |
| 4 | LLQ | 20 | INTERRUPT_MODERATION |
| 5 | EXTRA_PROPERTIES_STRINGS | 26 | AENQ_CONFIG |
| 6 | EXTRA_PROPERTIES_FLAGS | 27 | LINK_CONFIG |
| 7 | MAX_QUEUES_EXT | 28 | HOST_ATTR_CONFIG |
| 10 | RSS_HASH_FUNCTION | 29 | PHC_CONFIG |
| 11 | STATELESS_OFFLOAD_CONFIG | 32 | FEATURES_OPCODE_NUM (count) |

The device advertises which of these it supports in the
`supported_features` bitmap returned by GET_FEATURE DEVICE_ATTRIBUTES (bit n
= feature id n). [ena_admin_defs.h:562-565; ena_com.c:1020-1031, 2312]
(explicit)

### Key feature payloads

**DEVICE_ATTRIBUTES (1)** — GET only.
`struct ena_admin_device_attr_feature_desc`
[ena_admin_defs.h:557-584] (explicit):
```c
uint32_t impl_id;
uint32_t device_version;
uint32_t supported_features;  /* bitmap of feature_id */
uint32_t capabilities;        /* bitmap of ena_admin_aq_caps_id */
uint32_t phys_addr_width;
uint32_t virt_addr_width;
uint8_t  mac_addr[6];         /* unicast MAC, network byte order */
uint8_t  reserved7[2];
uint32_t max_mtu;
```
Fetched first in `ena_com_get_dev_attr_feat`; `supported_features` and
`capabilities` are cached into `ena_dev`. `capabilities` is a separate bitmap
(`enum ena_admin_aq_caps_id`: ENI_STATS=0, ENA_SRD_INFO=1, CUSTOMER_METRICS=2,
EXTENDED_RESET_REASONS=3, CDESC_MBZ=4) queried via `ena_com_get_cap`.
[ena_admin_defs.h:82-89; ena_com.c:2298-2313] (explicit)

**MAX_QUEUES_NUM (2) / MAX_QUEUES_EXT (7)** — GET. Payload structs
`ena_admin_queue_feature_desc` (legacy) and
`ena_admin_queue_ext_feature_desc`/`..._fields` (preferred). Field-by-field
breakdown and how ring sizes are derived: `queue-setup.md` §7.
[ena_admin_defs.h:709-759, 1059-1070] (explicit)

**MTU (14)** — SET. `struct ena_admin_set_feature_mtu_desc { uint32_t mtu; }`
(L2-excluded MTU). `ena_com_set_dev_mtu` sets `opcode=SET_FEATURE`,
`feature_id=MTU`, `flags=0`, `u.mtu.mtu = mtu`.
[ena_admin_defs.h:761-764; ena_com.c:2699-2729] (explicit)

**LINK_CONFIG (27)** — GET. `struct ena_admin_get_feature_link_desc`
[ena_admin_defs.h:796-808] (explicit):
```c
uint32_t speed;      /* Mb/s */
uint32_t supported;  /* bitfield of ena_admin_link_types (1G..400G) */
uint32_t flags;      /* bit0 autoneg; bit1 duplex (full) */
```
`AUTONEG = flags bit0`, `DUPLEX = flags bit1`
[ena_admin_defs.h:1324-1327]. Link speed enums (1G=0x1 .. 400G=0x200) at
[ena_admin_defs.h:100-111]. Fetched by `ena_com_get_link_params`
[ena_com.c:2243-2247] (explicit).

**AENQ_CONFIG (26)** — GET advertises `supported_groups`; SET writes
`enabled_groups`. `struct ena_admin_feature_aenq_desc { uint32_t
supported_groups; uint32_t enabled_groups; }`
[ena_admin_defs.h:810-816]. Full AENQ group enum and the SET handling are in
`aenq.md`. (explicit)

**HOST_ATTR_CONFIG (28)** — SET only.
`struct ena_admin_set_feature_host_attr_desc`
[ena_admin_defs.h:774-787] (explicit):
```c
struct ena_common_mem_addr os_info_ba;  /* 4KB contiguous host-info page */
struct ena_common_mem_addr debug_ba;    /* debug area base */
uint32_t debug_area_size;
```
`ena_com_set_host_attributes` fills both DMA addresses + debug size and sends
SET_FEATURE/HOST_ATTR_CONFIG. The `os_info_ba` page is a
`struct ena_admin_host_info` (OS type, driver version, BDF, num_cpus,
`driver_supported_features` bits, etc.) [ena_admin_defs.h:937-990].
[ena_com.c:3305-3352] (explicit)

**STATELESS_OFFLOAD_CONFIG (11)** — GET.
`struct ena_admin_feature_offload_desc { uint32_t tx; uint32_t rx_supported;
uint32_t rx_enabled; }` [ena_admin_defs.h:818-841]. `tx` bits: L3 csum IPv4,
L4 IPv4/IPv6 csum part/full, TSO IPv4/IPv6/ECN (bits 0..7). `rx_supported`
bits: L3 csum IPv4, L4 IPv4/IPv6 csum, RX hash (bits 0..3). Masks at
[ena_admin_defs.h:1330-1351]. Fetched by `ena_com_get_offload_settings`
[ena_com.c:2731-2747] (explicit).

**LLQ (4)** — GET/SET, `struct ena_admin_feature_llq_desc`. Negotiates Low
Latency Queue geometry; documented in `llq.md` (and partially in
`queue-setup.md`). [ena_admin_defs.h:648-707] (explicit)

---

## 7. GET_STATS (opcode 11)

`struct ena_admin_aq_get_stats_cmd` [ena_admin_defs.h:400-428] carries a
`type` (BASIC=0, EXTENDED=1, ENI=2, ENA_SRD=3, CUSTOMER_METRICS=4), a
`scope` (SPECIFIC_QUEUE=0 / ETH_TRAFFIC=1), `queue_idx`, `device_id`
(0xFFFF = "mine"), `requested_metrics` bitmap, and a control-buffer union for
extended/string output. The response `ena_admin_acq_get_stats_resp`
[ena_admin_defs.h:522-536] unions `basic_stats`, `eni_stats`, `ena_srd_info`,
`customer_metrics`. `ena_get_dev_stats` sets `opcode=GET_STATS`, `flags=0`,
`type=...`. [ena_com.c:2249-2274] (explicit). Stats types that need a buffer
(customer metrics, extended) attach a control buffer with its DMA
address/length. [ena_com.c:2680-2690] (explicit)

Full per-`type` payload layouts, the device-vs-driver split, and the
customer-metrics negotiation are in [statistics.md](statistics.md).

---

## 8. Device-boundary contract summary

What the device must implement for the admin queue:
- Read AQ entries from the AQ_BASE ring (32 × 64 B) up to the index written to
  AQ_DB; only entries whose `flags.phase` matches the current production phase
  are valid. (§3)
- For each command, DMA-write a 64 B completion into the ACQ_BASE ring,
  setting `command` = the request's `command_id`, `status`, optional
  `extended_status` and `sq_head_indx`, and the per-entry `flags.phase`; write
  the entry body **before** the phase bit becomes visible (host issues a read
  barrier after the phase check). (§4)
- Honor `ctrl_data` / `ctrl_data_indirect` to fetch/return out-of-line payload
  via `ena_admin_ctrl_buff_info`. (§6)
- Raise the admin MSI-X interrupt (vector 0) on completion unless masked via
  INTR_MASK; the host may instead poll. (§4, `registers.md`)

## Open ambiguities
- `extended_status` and `sq_head_indx`: DMA-written by the device per the
  struct layout, but the reference DPDK/Linux com layer reads neither
  (reclaim is driven by completion count, errno by `status`). Their precise
  device-side semantics beyond the header comments are not derivable from this
  driver. (inferred — fields defined, never read in ena_com.c)
- `inline_data_w1[3]` vs `control_buffer` union in the AQ entry: which arm is
  used is opcode-specific and signalled by the `ctrl_data*` flags; there is no
  separate length-of-inline field, so the device must interpret inline data by
  opcode.
