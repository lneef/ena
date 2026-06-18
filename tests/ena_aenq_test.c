/*
 * ENA asynchronous event notification queue (AENQ) qtests
 * (control path, polling mode).
 *
 * Covers the device-to-host AENQ ring (docs/wiki/aenq.md): acceptance of the
 * AENQ base/caps register programming and the initial head doorbell, the
 * AENQ_CONFIG feature negotiation (supported/enabled groups, done over the
 * admin queue), and the phase-bit consumption protocol for device-produced
 * events (link-change, keep-alive) plus the head-doorbell ack.
 *
 * Polling mode: the admin/AENQ MSI-X vector is masked (ena_admin_init()), so
 * events are discovered purely by the AENQ phase bit, never by interrupt. AQ
 * register programming and device setup live in ena_device_test.c; the admin
 * submit/complete mechanics in ena_admin_test.c.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#include "ena_test_common.h"

/* ------------------------------------------------------------------ */
/* AENQ_CONFIG feature framing (aenq.md §5)                            */
/* ------------------------------------------------------------------ */

/* struct ena_admin_feature_aenq_desc: supported (GET) / enabled (SET) group
 * bitmasks, bit positions = enum ena_admin_aenq_group. */
typedef struct QEMU_PACKED ENAAenqConfig {
    uint32_t supported_groups;
    uint32_t enabled_groups;
} ENAAenqConfig;

/* GET_FEATURE(AENQ_CONFIG) response: acq_common_desc then the desc. */
typedef struct QEMU_PACKED ENAAenqConfigResp {
    struct ena_admin_acq_common_desc acq_common;
    ENAAenqConfig aenq;
} ENAAenqConfigResp;

/* ------------------------------------------------------------------ */
/* AENQ consumption helpers (aenq.md §4)                               */
/* ------------------------------------------------------------------ */

/*
 * Publish that all `depth` AENQ entries are initially available to the device
 * by writing the absolute head (= depth) to AENQ_HEAD_DB
 * (ena_com_admin_aenq_enable, aenq.md §3). ena_admin_init() left t->aenq_head
 * == ENA_AENQ_QUEUE_DEPTH and t->aenq_phase == 1.
 */
static inline void ena_aenq_enable(ENATestCtx *t)
{
    g_assert_cmpuint(t->aenq_head, ==, ENA_AENQ_QUEUE_DEPTH);
    ena_reg_write(t, ENA_REGS_AENQ_HEAD_DB_OFF, t->aenq_head);
}

/*
 * Poll the AENQ for the next device-produced event at the current head. On a
 * matching phase bit, copy the entry out, advance the head (flipping phase on
 * wrap) and return the event group. Returns -1 if no event becomes ready
 * within the bounded poll budget.
 */
static inline int ena_aenq_poll(ENATestCtx *t,
                                struct ena_admin_aenq_entry *out)
{
    uint32_t idx = t->aenq_head & (ENA_AENQ_QUEUE_DEPTH - 1);
    struct ena_admin_aenq_entry e;
    int i;

    for (i = 0; i < ENA_TEST_POLL_RETRIES; i++) {
        qtest_memread(t->qts, t->aenq_base + idx * ENA_AENQ_ENTRY_SIZE,
                      &e, sizeof(e));
        if ((e.aenq_common_desc.flags &
             ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK) == t->aenq_phase) {
            t->aenq_head++;
            if ((t->aenq_head & (ENA_AENQ_QUEUE_DEPTH - 1)) == 0) {
                t->aenq_phase ^= 1;
            }
            if (out) {
                *out = e;
            }
            return le16_to_cpu(e.aenq_common_desc.group);
        }
        g_usleep(ENA_TEST_POLL_DELAY_US);
    }

    return -1; /* device produced no event within the budget */
}

/* Ack all consumed AENQ entries by writing the absolute head doorbell. */
static inline void ena_aenq_ack(ENATestCtx *t)
{
    ena_reg_write(t, ENA_REGS_AENQ_HEAD_DB_OFF, t->aenq_head);
}

/* ------------------------------------------------------------------ */
/* Register programming + initial doorbell                             */
/* ------------------------------------------------------------------ */

/*
 * ena_admin_init() programs AENQ_BASE_LO/HI and AENQ_CAPS (depth 16, entry
 * size 64). The device must accept it and remain READY with no fatal error,
 * and must accept the initial AENQ_HEAD_DB = depth publishing all entries.
 */
static void test_aenq_register_programming(void *obj, void *data,
                                           QGuestAllocator *alloc)
{
    ENATestCtx t;
    uint32_t sts;

    ena_bringup(&t, obj, alloc); /* includes AENQ base/caps programming */
    ena_aenq_enable(&t);

    sts = ena_reg_read(&t, ENA_REGS_DEV_STS_OFF);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_READY_MASK, ==,
                    ENA_REGS_DEV_STS_READY_MASK);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_FATAL_ERROR_MASK, ==, 0);
}

/* ------------------------------------------------------------------ */
/* AENQ_CONFIG feature negotiation (aenq.md §5)                        */
/* ------------------------------------------------------------------ */

/*
 * GET_FEATURE(AENQ_CONFIG) must advertise the set of groups the device can
 * ever produce in supported_groups. A device that emits any AENQ event must
 * at least support LINK_CHANGE and KEEP_ALIVE (the groups the reference driver
 * relies on for link state and the watchdog).
 */
static void test_aenq_config_get(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAAenqConfigResp resp;
    uint32_t supported;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    memset(&resp, 0, sizeof(resp));
    status = ena_get_feature(&t, ENA_ADMIN_AENQ_CONFIG, 0,
                             &resp, sizeof(resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    supported = le32_to_cpu(resp.aenq.supported_groups);
    g_assert_cmpuint(supported & (1u << ENA_ADMIN_LINK_CHANGE), !=, 0);
    g_assert_cmpuint(supported & (1u << ENA_ADMIN_KEEP_ALIVE), !=, 0);
}

/*
 * SET_FEATURE(AENQ_CONFIG) selecting a subset of the supported groups must
 * complete SUCCESSfully; the device must thereafter only emit events for the
 * enabled groups (aenq.md §5).
 */
static void test_aenq_config_set(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAAenqConfigResp get_resp;
    ENAAenqConfig set;
    uint32_t supported, want;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    memset(&get_resp, 0, sizeof(get_resp));
    status = ena_get_feature(&t, ENA_ADMIN_AENQ_CONFIG, 0,
                             &get_resp, sizeof(get_resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    supported = le32_to_cpu(get_resp.aenq.supported_groups);

    /* Enable the groups the driver actually wants, masked by what the device
     * supports (mirrors ena_com_set_aenq_config). */
    want = (1u << ENA_ADMIN_LINK_CHANGE) | (1u << ENA_ADMIN_KEEP_ALIVE) |
           (1u << ENA_ADMIN_NOTIFICATION);
    want &= supported;

    memset(&set, 0, sizeof(set));
    set.enabled_groups = cpu_to_le32(want);
    status = ena_set_feature(&t, ENA_ADMIN_AENQ_CONFIG, 0,
                             &set, sizeof(set), NULL, 0);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Event consumption: phase bit + ack                                  */
/* ------------------------------------------------------------------ */

/* Enable AENQ groups (LINK_CHANGE | KEEP_ALIVE) and turn on the ring. */
static void ena_aenq_bringup(ENATestCtx *t, void *obj, QGuestAllocator *alloc)
{
    ENAAenqConfig set;

    ena_bringup(t, obj, alloc);

    memset(&set, 0, sizeof(set));
    set.enabled_groups = cpu_to_le32((1u << ENA_ADMIN_LINK_CHANGE) |
                                     (1u << ENA_ADMIN_KEEP_ALIVE));
    ena_set_feature(t, ENA_ADMIN_AENQ_CONFIG, 0, &set, sizeof(set), NULL, 0);

    ena_aenq_enable(t);
}

/*
 * With LINK_CHANGE enabled the device is expected to report the initial link
 * state via an AENQ link-change event. The entry's phase bit must match the
 * host's expected phase, its group must be LINK_CHANGE, and acking it via the
 * head doorbell must be accepted.
 */
static void test_aenq_link_change_event(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    ENATestCtx t;
    struct ena_admin_aenq_entry e = { 0 };
    struct ena_admin_aenq_link_change_desc *lc;
    int group;

    ena_aenq_bringup(&t, obj, alloc);

    group = ena_aenq_poll(&t, &e);
    g_assert_cmpint(group, ==, ENA_ADMIN_LINK_CHANGE);

    /* Link-change payload carries the link status in flags bit0. */
    lc = (struct ena_admin_aenq_link_change_desc *)&e;
    g_assert_cmpuint(le32_to_cpu(lc->flags) &
                     ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK, !=, 0);

    ena_aenq_ack(&t);
}

/*
 * With KEEP_ALIVE enabled the device must periodically emit a keep-alive
 * heartbeat (aenq.md §7); the host watchdog resets the device otherwise. Poll
 * the AENQ until a KEEP_ALIVE event appears (skipping an initial link-change
 * if present), validating the phase-bit consumption + ack protocol.
 */
static void test_aenq_keep_alive_event(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    ENATestCtx t;
    int group;
    int seen = 0;

    ena_aenq_bringup(&t, obj, alloc);

    /* Allow a leading link-change before the first keep-alive. */
    for (;;) {
        group = ena_aenq_poll(&t, NULL);
        g_assert_cmpint(group, >=, 0); /* an event must arrive */
        if (group == ENA_ADMIN_KEEP_ALIVE) {
            break;
        }
        g_assert_cmpint(group, ==, ENA_ADMIN_LINK_CHANGE);
        g_assert_cmpint(++seen, <=, 1); /* at most one non-keepalive first */
    }

    ena_aenq_ack(&t);
}

/* ------------------------------------------------------------------ */

static void ena_register_nodes(void)
{
    ena_qos_node_register("ena-aenq");

    qos_add_test("aenq/register-programming", "ena-aenq",
                 test_aenq_register_programming, NULL);
    qos_add_test("aenq/config-get", "ena-aenq",
                 test_aenq_config_get, NULL);
    qos_add_test("aenq/config-set", "ena-aenq",
                 test_aenq_config_set, NULL);
    qos_add_test("aenq/link-change-event", "ena-aenq",
                 test_aenq_link_change_event, NULL);
    qos_add_test("aenq/keep-alive-event", "ena-aenq",
                 test_aenq_keep_alive_event, NULL);
}

libqos_init(ena_register_nodes);
