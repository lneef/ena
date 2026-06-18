/*
 * Shared control-path test harness for the ENA PCI device emulation.
 *
 * Provides the qgraph plumbing (QENA node), a per-test driver context
 * (ENATestCtx), and the bring-up primitives every control-path test needs:
 * the readless MMIO register-read handshake, the device reset handshake,
 * admin-queue (AQ/ACQ/AENQ) ring setup, admin command submit/poll/exec, and
 * GET/SET_FEATURE framing. Everything runs in polling mode: no MSI-X
 * interrupts are configured; completions are found purely by the phase bit.
 *
 * Behaviour mirrored here is documented in docs/wiki/{device-init,registers,
 * admin-queue,aenq}.md. All helpers are static inline so each test file can
 * include this header and register its own uniquely-named qgraph node via
 * ena_qos_node_register() / qos_node_create_driver_named(..., "ena", ...).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#ifndef TESTS_ENA_TEST_COMMON_H
#define TESTS_ENA_TEST_COMMON_H

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qemu/module.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "libqos/libqos-malloc.h"
#include "hw/pci/pci_regs.h"

#include "../hw/ena_regs.h"
#include "../hw/ena_admin_desc.h"
#include "../hw/ena_io_desc.h"

/* ------------------------------------------------------------------ */
/* qgraph node                                                        */
/* ------------------------------------------------------------------ */

typedef struct QENA {
    QOSGraphObject obj;
    QPCIDevice dev;
} QENA;

static void *ena_get_driver(void *obj, const char *interface)
{
    QENA *ena = obj;

    if (!g_strcmp0(interface, "pci-device")) {
        return &ena->dev;
    }

    fprintf(stderr, "%s not present in ena\n", interface);
    g_assert_not_reached();
}

static void *ena_create(void *pci_bus, QGuestAllocator *alloc, void *addr)
{
    QENA *ena = g_new0(QENA, 1);
    QPCIBus *bus = pci_bus;

    qpci_device_init(&ena->dev, bus, addr);
    ena->obj.get_driver = ena_get_driver;

    return &ena->obj;
}

/*
 * Register a qgraph driver node named @node backed by the real "ena" device
 * at PCI 04.0. Each test file uses a distinct @node so several ENA test
 * translation units can be linked into one qos-test binary.
 */
static inline void ena_qos_node_register(const char *node)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0",
    };
    add_qpci_address(&opts, &(QPCIAddress) { .devfn = QPCI_DEVFN(4, 0) });

    qos_node_create_driver_named(node, "ena", ena_create);
    qos_node_consumes(node, "pci-bus", &opts);
    qos_node_produces(node, "pci-device");
}

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

/* Bounded poll budget for memory the device is expected to DMA into.
 * The qtest device acts synchronously on MMIO accesses, so a completion is
 * normally visible on the first read; the budget just keeps a never-completing
 * command (e.g. against the current stub) from hanging the test. */
#define ENA_TEST_POLL_RETRIES   200
#define ENA_TEST_POLL_DELAY_US  500

#define ENA_ADMIN_QUEUE_DEPTH   32      /* AQ/ACQ depth, power of 2 */
#define ENA_AENQ_QUEUE_DEPTH    16      /* AENQ depth, power of 2 */
#define ENA_ADMIN_ENTRY_SIZE    64      /* sizeof(ena_admin_aq_entry) */
#define ENA_ACQ_ENTRY_SIZE      64      /* sizeof(ena_admin_acq_entry) */
#define ENA_AENQ_ENTRY_SIZE     64      /* sizeof(ena_admin_aenq_entry) */

/* GET/SET_FEATURE flags.select values (feat_common.flags bits 1:0). */
#define ENA_ADMIN_GET_FEATURE_SELECT_CURRENT  0x1
#define ENA_ADMIN_GET_FEATURE_SELECT_DEFAULT  0x3

/* ------------------------------------------------------------------ */
/* GET/SET_FEATURE command framing (not in hw/ena_admin_desc.h)        */
/* ------------------------------------------------------------------ */

/* struct ena_admin_get_feat_cmd: common(4) + ctrl_buff(12) + feat_common(4)
 * + raw[11] (44) = 64 bytes (admin-queue.md §6). */
typedef struct QEMU_PACKED ENAGetFeatCmd {
    struct ena_admin_aq_common_desc common;
    struct ena_admin_ctrl_buff_info control_buffer;
    struct ena_admin_get_set_feature_common_desc feat_common;
    uint32_t raw[11];
} ENAGetFeatCmd;

/* struct ena_admin_set_feat_cmd: same first three members, then a 44-byte
 * per-feature payload area. */
typedef struct QEMU_PACKED ENASetFeatCmd {
    struct ena_admin_aq_common_desc common;
    struct ena_admin_ctrl_buff_info control_buffer;
    struct ena_admin_get_set_feature_common_desc feat_common;
    uint32_t payload[11];
} ENASetFeatCmd;

/* ------------------------------------------------------------------ */
/* Per-test driver context                                            */
/* ------------------------------------------------------------------ */

typedef struct ENATestCtx {
    QTestState *qts;
    QPCIDevice *dev;
    QGuestAllocator *alloc;
    QPCIBar regs;               /* BAR0 register window */

    uint64_t mmio_resp;         /* readless response buffer (guest phys) */
    uint16_t mmio_seq;          /* readless request id counter */

    uint64_t aq_base;           /* admin SQ ring (guest phys) */
    uint64_t acq_base;          /* admin CQ ring (guest phys) */
    uint64_t aenq_base;         /* AENQ ring (guest phys) */

    uint16_t aq_tail;           /* absolute AQ producer index (-> AQ_DB) */
    uint8_t  aq_phase;          /* current AQ submission phase */
    uint16_t acq_head;          /* absolute ACQ consumer index */
    uint8_t  acq_phase;         /* expected ACQ completion phase */
    uint16_t cmd_id;            /* next admin command_id (mod q_depth) */

    uint16_t aenq_head;         /* absolute AENQ consumer index */
    uint8_t  aenq_phase;        /* expected AENQ phase */
} ENATestCtx;

/* ------------------------------------------------------------------ */
/* Register access                                                    */
/* ------------------------------------------------------------------ */

static inline void ena_reg_write(ENATestCtx *t, uint64_t off, uint32_t val)
{
    qpci_io_writel(t->dev, t->regs, off, val);
}

/* Direct BAR0 read. The device also serves these registers through the
 * readless handshake (ena_readless_read), exercised explicitly by the
 * device-setup tests. */
static inline uint32_t ena_reg_read(ENATestCtx *t, uint64_t off)
{
    return qpci_io_readl(t->dev, t->regs, off);
}

/* ------------------------------------------------------------------ */
/* Readless (indirect) MMIO register read                             */
/* ------------------------------------------------------------------ */

/* Publish the 8-byte readless response buffer address to MMIO_RESP_LO/HI. */
static inline void ena_mmio_resp_arm(ENATestCtx *t)
{
    ena_reg_write(t, ENA_REGS_MMIO_RESP_LO_OFF, t->mmio_resp & 0xffffffff);
    ena_reg_write(t, ENA_REGS_MMIO_RESP_HI_OFF, t->mmio_resp >> 32);
}

/*
 * Perform the readless register read of @reg_off: write the MMIO_REG_READ
 * doorbell with a fresh request id, then poll the response buffer until the
 * device echoes that id. Returns the register value and (optionally) the
 * echoed register offset.
 */
static inline uint32_t ena_readless_read_full(ENATestCtx *t, uint16_t reg_off,
                                              uint16_t *echoed_off)
{
    struct ena_mmio_read_less_resp resp;
    uint16_t req_id = ++t->mmio_seq;
    uint32_t cmd;
    int i;

    /* Poison the buffer so a stale id can't satisfy the poll. */
    memset(&resp, 0, sizeof(resp));
    resp.req_id = req_id ^ 0xffff;
    qtest_memwrite(t->qts, t->mmio_resp, &resp, sizeof(resp));

    cmd = ((uint32_t)reg_off << ENA_REGS_MMIO_REG_READ_REG_OFF_SHIFT) |
          (req_id & ENA_REGS_MMIO_REG_READ_REQ_ID_MASK);
    ena_reg_write(t, ENA_REGS_MMIO_REG_READ_OFF, cmd);

    for (i = 0; i < ENA_TEST_POLL_RETRIES; i++) {
        qtest_memread(t->qts, t->mmio_resp, &resp, sizeof(resp));
        if (le16_to_cpu(resp.req_id) == req_id) {
            if (echoed_off) {
                *echoed_off = le16_to_cpu(resp.reg_off);
            }
            return le32_to_cpu(resp.reg_val);
        }
        g_usleep(ENA_TEST_POLL_DELAY_US);
    }

    g_assert_not_reached(); /* device never answered the readless read */
}

static inline uint32_t ena_readless_read(ENATestCtx *t, uint16_t reg_off)
{
    return ena_readless_read_full(t, reg_off, NULL);
}

/* ------------------------------------------------------------------ */
/* Device reset handshake (device-init.md §1, registers.md)            */
/* ------------------------------------------------------------------ */

static inline uint32_t ena_wait_dev_sts(ENATestCtx *t, uint32_t mask,
                                        bool set)
{
    uint32_t sts = 0;
    int i;

    for (i = 0; i < ENA_TEST_POLL_RETRIES; i++) {
        sts = ena_reg_read(t, ENA_REGS_DEV_STS_OFF);
        if (!!(sts & mask) == set) {
            return sts;
        }
        g_usleep(ENA_TEST_POLL_DELAY_US);
    }

    g_assert_not_reached(); /* DEV_STS bit never reached expected state */
}

/*
 * Two-phase reset handshake: require DEV_STS.READY, assert DEV_CTL.DEV_RESET
 * with the NORMAL reason, re-arm the readless buffer, wait for
 * RESET_IN_PROGRESS, drop DEV_CTL, wait for it to clear.
 */
static inline void ena_device_reset(ENATestCtx *t)
{
    uint32_t ctl;

    ena_wait_dev_sts(t, ENA_REGS_DEV_STS_READY_MASK, true);

    ctl = ENA_REGS_DEV_CTL_DEV_RESET_MASK |
          ((uint32_t)ENA_REGS_RESET_NORMAL << ENA_REGS_DEV_CTL_RESET_REASON_SHIFT);
    ena_reg_write(t, ENA_REGS_DEV_CTL_OFF, ctl);

    /* Reset clears device state including the readless buffer address. */
    ena_mmio_resp_arm(t);

    ena_wait_dev_sts(t, ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK, true);

    ena_reg_write(t, ENA_REGS_DEV_CTL_OFF, 0);

    ena_wait_dev_sts(t, ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK, false);
}

/* ------------------------------------------------------------------ */
/* Admin queue setup (device-init.md §3-§4, admin-queue.md §1)         */
/* ------------------------------------------------------------------ */

static inline void ena_set_mem_addr(struct ena_common_mem_addr *addr,
                                    uint64_t pa)
{
    addr->mem_addr_low = cpu_to_le32((uint32_t)pa);
    addr->mem_addr_high = cpu_to_le16((uint16_t)(pa >> 32));
    addr->reserved16 = 0;
}

/*
 * Allocate and register the admin SQ/ACQ and the AENQ rings, then mask the
 * admin interrupt (polling mode). Mirrors ena_com_admin_init +
 * ena_com_admin_init_aenq + ena_com_set_admin_polling_mode(true).
 */
static inline void ena_admin_init(ENATestCtx *t)
{
    uint32_t caps;

    t->aq_base = guest_alloc(t->alloc, ENA_ADMIN_QUEUE_DEPTH * ENA_ADMIN_ENTRY_SIZE);
    t->acq_base = guest_alloc(t->alloc, ENA_ADMIN_QUEUE_DEPTH * ENA_ACQ_ENTRY_SIZE);
    t->aenq_base = guest_alloc(t->alloc, ENA_AENQ_QUEUE_DEPTH * ENA_AENQ_ENTRY_SIZE);
    qtest_memset(t->qts, t->aq_base, 0,
                 ENA_ADMIN_QUEUE_DEPTH * ENA_ADMIN_ENTRY_SIZE);
    qtest_memset(t->qts, t->acq_base, 0,
                 ENA_ADMIN_QUEUE_DEPTH * ENA_ACQ_ENTRY_SIZE);
    qtest_memset(t->qts, t->aenq_base, 0,
                 ENA_AENQ_QUEUE_DEPTH * ENA_AENQ_ENTRY_SIZE);

    /* AQ base + caps */
    ena_reg_write(t, ENA_REGS_AQ_BASE_LO_OFF, t->aq_base & 0xffffffff);
    ena_reg_write(t, ENA_REGS_AQ_BASE_HI_OFF, t->aq_base >> 32);
    caps = (ENA_ADMIN_QUEUE_DEPTH & ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK) |
           ((uint32_t)ENA_ADMIN_ENTRY_SIZE << ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_SHIFT);
    ena_reg_write(t, ENA_REGS_AQ_CAPS_OFF, caps);

    /* ACQ base + caps */
    ena_reg_write(t, ENA_REGS_ACQ_BASE_LO_OFF, t->acq_base & 0xffffffff);
    ena_reg_write(t, ENA_REGS_ACQ_BASE_HI_OFF, t->acq_base >> 32);
    caps = (ENA_ADMIN_QUEUE_DEPTH & ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK) |
           ((uint32_t)ENA_ACQ_ENTRY_SIZE << ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_SHIFT);
    ena_reg_write(t, ENA_REGS_ACQ_CAPS_OFF, caps);

    /* AENQ base + caps */
    ena_reg_write(t, ENA_REGS_AENQ_BASE_LO_OFF, t->aenq_base & 0xffffffff);
    ena_reg_write(t, ENA_REGS_AENQ_BASE_HI_OFF, t->aenq_base >> 32);
    caps = (ENA_AENQ_QUEUE_DEPTH & ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK) |
           ((uint32_t)ENA_AENQ_ENTRY_SIZE << ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_SHIFT);
    ena_reg_write(t, ENA_REGS_AENQ_CAPS_OFF, caps);

    t->aq_tail = 0;
    t->aq_phase = 1;
    t->acq_head = 0;
    t->acq_phase = 1;
    t->cmd_id = 0;
    t->aenq_head = ENA_AENQ_QUEUE_DEPTH;
    t->aenq_phase = 1;

    /* Polling mode: mask the admin/AENQ interrupt. */
    ena_reg_write(t, ENA_REGS_INTR_MASK_OFF, ENA_REGS_INTR_MASK_INTR_MASK);
}

/* ------------------------------------------------------------------ */
/* Admin command submit / poll / execute (admin-queue.md §3-§4)        */
/* ------------------------------------------------------------------ */

/*
 * Copy a command into the AQ at the current tail, stamp it with the phase bit
 * and a fresh command_id, advance the tail (flipping phase on wrap) and ring
 * AQ_DB with the absolute tail. Returns the command_id assigned.
 */
static inline uint16_t ena_admin_submit(ENATestCtx *t, const void *cmd,
                                        size_t cmd_size)
{
    uint8_t entry[ENA_ADMIN_ENTRY_SIZE];
    struct ena_admin_aq_common_desc *common = (void *)entry;
    uint16_t cmd_id = t->cmd_id;
    uint32_t idx = t->aq_tail & (ENA_ADMIN_QUEUE_DEPTH - 1);

    g_assert_cmpuint(cmd_size, <=, ENA_ADMIN_ENTRY_SIZE);
    memset(entry, 0, sizeof(entry));
    memcpy(entry, cmd, cmd_size);

    common->command_id = cpu_to_le16(cmd_id &
                                     ENA_ADMIN_AQ_COMMON_DESC_COMMAND_ID_MASK);
    common->flags = (common->flags & ~ENA_ADMIN_AQ_COMMON_DESC_PHASE_MASK) |
                    (t->aq_phase & ENA_ADMIN_AQ_COMMON_DESC_PHASE_MASK);

    qtest_memwrite(t->qts, t->aq_base + idx * ENA_ADMIN_ENTRY_SIZE,
                   entry, ENA_ADMIN_ENTRY_SIZE);

    t->cmd_id = (t->cmd_id + 1) & (ENA_ADMIN_QUEUE_DEPTH - 1);
    t->aq_tail++;
    if ((t->aq_tail & (ENA_ADMIN_QUEUE_DEPTH - 1)) == 0) {
        t->aq_phase ^= 1;
    }

    ena_reg_write(t, ENA_REGS_AQ_DB_OFF, t->aq_tail);
    return cmd_id;
}

/*
 * Poll the ACQ for the next completion (matching the expected phase), copy it
 * out and advance the consumer. Returns the completion status byte. If the
 * device never completes, g_assert_not_reached fires (test fails, no hang).
 */
static inline uint8_t ena_admin_poll(ENATestCtx *t, void *resp,
                                     size_t resp_size)
{
    uint8_t entry[ENA_ACQ_ENTRY_SIZE];
    struct ena_admin_acq_common_desc *common = (void *)entry;
    uint32_t idx = t->acq_head & (ENA_ADMIN_QUEUE_DEPTH - 1);
    int i;

    g_assert_cmpuint(resp_size, <=, ENA_ACQ_ENTRY_SIZE);

    for (i = 0; i < ENA_TEST_POLL_RETRIES; i++) {
        qtest_memread(t->qts, t->acq_base + idx * ENA_ACQ_ENTRY_SIZE,
                      entry, ENA_ACQ_ENTRY_SIZE);
        if ((common->flags & ENA_ADMIN_ACQ_COMMON_DESC_PHASE_MASK) ==
            t->acq_phase) {
            t->acq_head++;
            if ((t->acq_head & (ENA_ADMIN_QUEUE_DEPTH - 1)) == 0) {
                t->acq_phase ^= 1;
            }
            if (resp) {
                memcpy(resp, entry, resp_size);
            }
            return common->status;
        }
        g_usleep(ENA_TEST_POLL_DELAY_US);
    }

    g_assert_not_reached(); /* device never produced a completion */
}

/* Submit + poll, returning the status without asserting (caller checks). */
static inline uint8_t ena_admin_try(ENATestCtx *t, const void *cmd,
                                    size_t cmd_size, void *resp,
                                    size_t resp_size)
{
    uint16_t cmd_id = ena_admin_submit(t, cmd, cmd_size);
    uint8_t entry[ENA_ACQ_ENTRY_SIZE];
    struct ena_admin_acq_common_desc *common = (void *)entry;
    uint8_t status;

    status = ena_admin_poll(t, entry, sizeof(entry));
    /* The completion must echo the submitted command_id. */
    g_assert_cmpuint(le16_to_cpu(common->command) &
                     ENA_ADMIN_ACQ_COMMON_DESC_COMMAND_ID_MASK, ==, cmd_id);
    if (resp) {
        memcpy(resp, entry, resp_size);
    }
    return status;
}

/* Submit + poll, asserting a SUCCESS completion. */
static inline void ena_admin_exec(ENATestCtx *t, const void *cmd,
                                  size_t cmd_size, void *resp,
                                  size_t resp_size)
{
    uint8_t status = ena_admin_try(t, cmd, cmd_size, resp, resp_size);

    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* GET/SET_FEATURE helpers (admin-queue.md §6)                         */
/* ------------------------------------------------------------------ */

/* GET_FEATURE @feature_id (select=current), copying the response entry out. */
static inline uint8_t ena_get_feature(ENATestCtx *t, uint8_t feature_id,
                                      uint8_t version, void *resp,
                                      size_t resp_size)
{
    ENAGetFeatCmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.common.opcode = ENA_ADMIN_GET_FEATURE;
    cmd.feat_common.flags = ENA_ADMIN_GET_FEATURE_SELECT_CURRENT;
    cmd.feat_common.feature_id = feature_id;
    cmd.feat_common.feature_version = version;

    return ena_admin_try(t, &cmd, sizeof(cmd), resp, resp_size);
}

/* SET_FEATURE @feature_id with @payload bytes copied into the payload area. */
static inline uint8_t ena_set_feature(ENATestCtx *t, uint8_t feature_id,
                                      uint8_t version, const void *payload,
                                      size_t payload_size, void *resp,
                                      size_t resp_size)
{
    ENASetFeatCmd cmd;

    g_assert_cmpuint(payload_size, <=, sizeof(cmd.payload));
    memset(&cmd, 0, sizeof(cmd));
    cmd.common.opcode = ENA_ADMIN_SET_FEATURE;
    cmd.feat_common.feature_id = feature_id;
    cmd.feat_common.feature_version = version;
    if (payload && payload_size) {
        memcpy(cmd.payload, payload, payload_size);
    }

    return ena_admin_try(t, &cmd, sizeof(cmd), resp, resp_size);
}

/* ------------------------------------------------------------------ */
/* Standard bring-up: map BAR0, arm readless, reset, init admin queue  */
/* ------------------------------------------------------------------ */

static inline void ena_bringup(ENATestCtx *t, void *obj,
                               QGuestAllocator *alloc)
{
    QENA *ena = obj;

    memset(t, 0, sizeof(*t));
    t->qts = ena->dev.bus->qts;
    t->dev = &ena->dev;
    t->alloc = alloc;

    qpci_device_enable(t->dev);
    t->regs = qpci_iomap(t->dev, 0, NULL);

    t->mmio_resp = guest_alloc(alloc, sizeof(struct ena_mmio_read_less_resp));
    qtest_memset(t->qts, t->mmio_resp, 0,
                 sizeof(struct ena_mmio_read_less_resp));
    ena_mmio_resp_arm(t);

    ena_device_reset(t);
    ena_admin_init(t);
}

#endif /* TESTS_ENA_TEST_COMMON_H */
