/*
 * QTest testcase for the ENA NIC: asynchronous event notification queue.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "tests/ena_qos.h"

#define KEEP_ALIVE_NS (1000 * 1000 * 1000LL)

static void set_aenq_groups(QEna *d, uint32_t groups)
{
    struct ena_admin_set_feat_cmd cmd = {};

    cmd.feat_common.feature_id = ENA_ADMIN_AENQ_CONFIG;
    cmd.u.aenq.enabled_groups = cpu_to_le32(groups);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_SUCCESS);
}

static void test_link_change(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_aenq_entry e;
    struct ena_admin_aenq_link_change_desc *link = (void *)&e;

    ena_bringup(d);
    g_assert_false(ena_aenq_poll(d, &e));
    ena_msix_clear(d, ENA_TEST_ADMIN_VECTOR);

    /* enabling the link change group reports the current link state */
    set_aenq_groups(d, BIT(ENA_ADMIN_LINK_CHANGE));
    g_assert_true(ena_aenq_poll(d, &e));
    g_assert_cmpuint(le16_to_cpu(e.aenq_common_desc.group), ==,
                     ENA_ADMIN_LINK_CHANGE);
    g_assert_cmphex(le32_to_cpu(link->flags) &
                    ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK, !=, 0);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_AENQ_TAIL_OFF), ==, 1);
    g_assert_true(ena_msix_fired(d, ENA_TEST_ADMIN_VECTOR));
    g_assert_false(ena_aenq_poll(d, &e));
}

static void test_keep_alive(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_aenq_entry e;
    uint64_t ts_prev;

    ena_bringup(d);
    set_aenq_groups(d, BIT(ENA_ADMIN_KEEP_ALIVE));
    ena_msix_clear(d, ENA_TEST_ADMIN_VECTOR);
    g_assert_false(ena_aenq_poll(d, &e));

    qtest_clock_step(d->dev.bus->qts, KEEP_ALIVE_NS);
    g_assert_true(ena_aenq_poll(d, &e));
    g_assert_cmpuint(le16_to_cpu(e.aenq_common_desc.group), ==,
                     ENA_ADMIN_KEEP_ALIVE);
    g_assert_true(ena_msix_fired(d, ENA_TEST_ADMIN_VECTOR));
    ts_prev = ((uint64_t)le32_to_cpu(e.aenq_common_desc.timestamp_high) << 32) |
              le32_to_cpu(e.aenq_common_desc.timestamp_low);
    g_assert_false(ena_aenq_poll(d, &e));

    /* keep-alive is periodic */
    qtest_clock_step(d->dev.bus->qts, KEEP_ALIVE_NS);
    g_assert_true(ena_aenq_poll(d, &e));
    g_assert_cmpuint(le16_to_cpu(e.aenq_common_desc.group), ==,
                     ENA_ADMIN_KEEP_ALIVE);
    g_assert_cmpuint(((uint64_t)le32_to_cpu(e.aenq_common_desc.timestamp_high) << 32) |
                     le32_to_cpu(e.aenq_common_desc.timestamp_low), >, ts_prev);

    /* disabling the group stops the events */
    set_aenq_groups(d, 0);
    qtest_clock_step(d->dev.bus->qts, 3 * KEEP_ALIVE_NS);
    g_assert_false(ena_aenq_poll(d, &e));
}

static void test_masked_interrupt(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_aenq_entry e;

    ena_bringup(d);
    ena_reg_write(d, ENA_REGS_INTR_MASK_OFF, 1);
    set_aenq_groups(d, BIT(ENA_ADMIN_LINK_CHANGE));
    g_assert_true(ena_aenq_poll(d, &e));
    g_assert_false(ena_msix_fired(d, ENA_TEST_ADMIN_VECTOR));
}

/* A head doorbell more than depth ahead of the tail grants no credit. */
static void test_head_past_depth(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_aenq_entry e;

    ena_bringup(d);
    set_aenq_groups(d, BIT(ENA_ADMIN_KEEP_ALIVE));
    ena_reg_write(d, ENA_REGS_AENQ_HEAD_DB_OFF, ENA_TEST_AENQ_DEPTH + 1);
    qtest_clock_step(d->dev.bus->qts, KEEP_ALIVE_NS);
    g_assert_false(ena_aenq_poll(d, &e));
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_AENQ_TAIL_OFF), ==, 0);

    ena_reg_write(d, ENA_REGS_AENQ_HEAD_DB_OFF, ENA_TEST_AENQ_DEPTH);
    qtest_clock_step(d->dev.bus->qts, KEEP_ALIVE_NS);
    g_assert_true(ena_aenq_poll(d, &e));
}

static void test_queue_full_and_wrap(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_aenq_entry e;
    int i;

    ena_bringup(d);
    set_aenq_groups(d, BIT(ENA_ADMIN_KEEP_ALIVE));

    /* the driver does not consume: the device fills all entries then drops */
    qtest_clock_step(d->dev.bus->qts, (ENA_TEST_AENQ_DEPTH + 4) * KEEP_ALIVE_NS);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_AENQ_TAIL_OFF), ==,
                     ENA_TEST_AENQ_DEPTH);
    for (i = 0; i < ENA_TEST_AENQ_DEPTH; i++) {
        g_assert_true(ena_aenq_poll(d, &e));
        g_assert_cmpuint(le16_to_cpu(e.aenq_common_desc.group), ==,
                         ENA_ADMIN_KEEP_ALIVE);
    }
    g_assert_false(ena_aenq_poll(d, &e));

    /* after the head doorbell moved, events land again with flipped phase */
    qtest_clock_step(d->dev.bus->qts, KEEP_ALIVE_NS);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_AENQ_TAIL_OFF), ==,
                     ENA_TEST_AENQ_DEPTH + 1);
    g_assert_true(ena_aenq_poll(d, &e));
    g_assert_cmpuint(e.aenq_common_desc.flags &
                     ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK, ==, 0);
    g_assert_false(ena_aenq_poll(d, &e));
}

static void test_events_before_enable_dropped(void *obj, void *data,
                                              QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_aenq_entry e;

    ena_dev_reset(d);
    ena_admin_init(d);
    ena_msix_setup(d, ENA_TEST_ADMIN_VECTOR);
    /* no head doorbell yet: the queue has no free entries */
    set_aenq_groups(d, BIT(ENA_ADMIN_LINK_CHANGE));
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_AENQ_TAIL_OFF), ==, 0);
    qtest_memread(d->dev.bus->qts, d->aenq, &e, sizeof(e));
    g_assert_cmpuint(e.aenq_common_desc.flags &
                     ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK, ==, 0);

    ena_aenq_enable(d);
    set_aenq_groups(d, BIT(ENA_ADMIN_LINK_CHANGE));
    g_assert_true(ena_aenq_poll(d, &e));
    g_assert_cmpuint(le16_to_cpu(e.aenq_common_desc.group), ==,
                     ENA_ADMIN_LINK_CHANGE);
}

static void register_ena_aenq_test(void)
{
    QOSGraphTestOptions opts = {
        .before = ena_test_before,
    };

    qos_add_test("aenq/link-change", "ena", test_link_change, &opts);
    qos_add_test("aenq/keep-alive", "ena", test_keep_alive, &opts);
    qos_add_test("aenq/masked-interrupt", "ena", test_masked_interrupt, &opts);
    qos_add_test("aenq/head-past-depth", "ena", test_head_past_depth, &opts);
    qos_add_test("aenq/queue-full-and-wrap", "ena", test_queue_full_and_wrap,
                 &opts);
    qos_add_test("aenq/events-before-enable-dropped", "ena",
                 test_events_before_enable_dropped, &opts);
}

libqos_init(register_ena_aenq_test);
