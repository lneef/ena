/*
 * QTest testcase for the ENA NIC: IO submission/completion queue creation
 * and teardown through the admin queue.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "tests/ena_qos.h"

#define SQ_DB_BASE      0x1000
#define CQ_UNMASK_BASE  0x2000
#define LLQ_QUEUE_BYTES (1024 * 128)
#define RING_BYTES      (1024 * 16)

static void enable_llq(QEna *d)
{
    struct ena_admin_set_feat_cmd cmd = {};

    cmd.feat_common.feature_id = ENA_ADMIN_LLQ;
    cmd.u.llq.header_location_ctrl_enabled = cpu_to_le16(ENA_ADMIN_INLINE_HEADER);
    cmd.u.llq.entry_size_ctrl_enabled = cpu_to_le16(ENA_ADMIN_LIST_ENTRY_SIZE_128B);
    cmd.u.llq.desc_num_before_header_enabled =
        cpu_to_le16(ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2);
    cmd.u.llq.descriptors_stride_ctrl_enabled =
        cpu_to_le16(ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_SUCCESS);
}

static void test_create_destroy_cq(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_acq_create_cq_resp_desc cq;
    uint64_t ring = guest_alloc(alloc, RING_BYTES);
    uint16_t idx;

    ena_bringup(d);
    g_assert_cmpint(ena_create_cq(d, 1024, 4, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    idx = le16_to_cpu(cq.cq_idx);
    g_assert_cmpuint(le16_to_cpu(cq.cq_actual_depth), ==, 1024);
    g_assert_cmphex(le32_to_cpu(cq.cq_interrupt_unmask_register_offset), ==,
                    CQ_UNMASK_BASE + idx * 4);
    /* no NUMA register: the driver only writes it when the offset is nonzero */
    g_assert_cmphex(le32_to_cpu(cq.numa_node_register_offset), ==, 0);

    /* TX completions are 2 words, extended formats 4 or 8 words */
    g_assert_cmpint(ena_create_cq(d, 16, 2, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(cq.cq_idx), !=, idx);
    g_assert_cmpint(ena_create_cq(d, 16, 8, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);

    g_assert_cmpint(ena_destroy_cq(d, idx), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_destroy_cq(d, idx), ==, ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_destroy_cq(d, 0xffff), ==, ENA_ADMIN_ILLEGAL_PARAMETER);
    /* a freed index is handed out again */
    g_assert_cmpint(ena_create_cq(d, 1024, 4, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(cq.cq_idx), ==, idx);
}

static void test_cq_bad_params(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_acq_create_cq_resp_desc cq;
    uint64_t ring = guest_alloc(alloc, RING_BYTES);

    ena_bringup(d);
    g_assert_cmpint(ena_create_cq(d, 1000, 4, 1, ring, &cq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_cq(d, 2048, 4, 1, ring, &cq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_cq(d, 0, 4, 1, ring, &cq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_cq(d, 1024, 5, 1, ring, &cq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_cq(d, 1024, 1, 1, ring, &cq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    /* IO vectors are 1..8 or none (-1); vector 0 belongs to the admin path */
    g_assert_cmpint(ena_create_cq(d, 1024, 4, 0, ring, &cq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_cq(d, 1024, 4, 9, ring, &cq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_cq(d, 1024, 4, 0xffffffff, ring, &cq), ==,
                    ENA_ADMIN_SUCCESS);
}

static void test_create_destroy_sq(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_acq_create_cq_resp_desc cq;
    struct ena_admin_acq_create_sq_resp_desc sq;
    uint64_t ring = guest_alloc(alloc, RING_BYTES);
    uint16_t cq_idx, tx_idx, rx_idx;

    ena_bringup(d);
    g_assert_cmpint(ena_create_cq(d, 1024, 4, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    cq_idx = le16_to_cpu(cq.cq_idx);

    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                  cq_idx, 1024, ring, &sq), ==, ENA_ADMIN_SUCCESS);
    tx_idx = le16_to_cpu(sq.sq_idx);
    g_assert_cmphex(le32_to_cpu(sq.sq_doorbell_offset), ==, SQ_DB_BASE + tx_idx * 4);
    g_assert_cmphex(le32_to_cpu(sq.llq_descriptors_offset), ==, 0);

    g_assert_cmpint(ena_create_sq(d, false, ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                  cq_idx, 1024, ring, &sq), ==, ENA_ADMIN_SUCCESS);
    rx_idx = le16_to_cpu(sq.sq_idx);
    g_assert_cmpuint(rx_idx, !=, tx_idx);
    g_assert_cmphex(le32_to_cpu(sq.sq_doorbell_offset), ==, SQ_DB_BASE + rx_idx * 4);
    /* the CQ stays busy while an SQ completes into it */
    g_assert_cmpint(ena_destroy_cq(d, cq_idx), ==, ENA_ADMIN_RESOURCE_BUSY);

    /* destroy must name the right direction */
    g_assert_cmpint(ena_destroy_sq(d, tx_idx, false), ==, ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_destroy_sq(d, tx_idx, true), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_destroy_sq(d, tx_idx, true), ==, ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_destroy_sq(d, rx_idx, false), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_destroy_cq(d, cq_idx), ==, ENA_ADMIN_SUCCESS);
}

static void test_sq_bad_params(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_acq_create_cq_resp_desc cq;
    struct ena_admin_acq_create_sq_resp_desc sq;
    struct ena_admin_aq_create_sq_cmd cmd = {};
    uint64_t ring = guest_alloc(alloc, RING_BYTES);
    uint16_t cq_idx;

    ena_bringup(d);
    /* the completion queue must exist first */
    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_HOST, 0,
                                  1024, ring, &sq), ==, ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_cq(d, 1024, 4, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    cq_idx = le16_to_cpu(cq.cq_idx);

    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                  cq_idx, 1000, ring, &sq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_sq(d, true, 0, cq_idx, 1024, ring, &sq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    /* RX completions need 4 or 8 words */
    g_assert_cmpint(ena_create_cq(d, 16, 2, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_create_sq(d, false, ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                  le16_to_cpu(cq.cq_idx), 16, ring, &sq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    /* the SQ may not be deeper than its CQ; a host ring needs an address */
    g_assert_cmpint(ena_create_cq(d, 16, 4, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                  le16_to_cpu(cq.cq_idx), 32, ring, &sq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                  cq_idx, 1024, 0, &sq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    /* device placement before the LLQ feature was negotiated */
    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_DEV,
                                  cq_idx, 1024, 0, &sq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);

    /* invalid direction encoding */
    cmd.aq_common_descriptor.opcode = ENA_ADMIN_CREATE_SQ;
    cmd.sq_identity = 3 << ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_SHIFT;
    cmd.sq_caps_2 = ENA_ADMIN_PLACEMENT_POLICY_HOST;
    cmd.cq_idx = cpu_to_le16(cq_idx);
    cmd.sq_depth = cpu_to_le16(1024);
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &sq, sizeof(sq)), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);

    /* only per-descriptor completions are supported */
    cmd.sq_identity = ENA_ADMIN_SQ_DIRECTION_TX << ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_SHIFT;
    cmd.sq_caps_2 = ENA_ADMIN_PLACEMENT_POLICY_HOST |
                    (ENA_ADMIN_COMPLETION_POLICY_HEAD <<
                     ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_SHIFT);
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &sq, sizeof(sq)), ==,
                    ENA_ADMIN_UNSUPPORTED_OPCODE);

    /* the ring must be physically contiguous */
    cmd.sq_caps_2 = ENA_ADMIN_PLACEMENT_POLICY_HOST |
                    (ENA_ADMIN_COMPLETION_POLICY_DESC <<
                     ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_SHIFT);
    cmd.sq_caps_3 = 0;
    cmd.sq_ba.mem_addr_low = cpu_to_le32(ring);
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &sq, sizeof(sq)), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
}

static void test_llq_sq(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_acq_create_cq_resp_desc cq;
    struct ena_admin_acq_create_sq_resp_desc sq;
    uint64_t ring = guest_alloc(alloc, RING_BYTES);
    uint16_t cq_idx, sq_idx;
    uint64_t mem_size;

    ena_bringup(d);
    enable_llq(d);
    g_assert_cmpint(ena_create_cq(d, 1024, 4, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    cq_idx = le16_to_cpu(cq.cq_idx);

    /* RX queues cannot live in device memory */
    g_assert_cmpint(ena_create_sq(d, false, ENA_ADMIN_PLACEMENT_POLICY_DEV,
                                  cq_idx, 1024, 0, &sq), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);

    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_DEV,
                                  cq_idx, 1024, 0, &sq), ==, ENA_ADMIN_SUCCESS);
    sq_idx = le16_to_cpu(sq.sq_idx);
    g_assert_cmphex(le32_to_cpu(sq.sq_doorbell_offset), ==, SQ_DB_BASE + sq_idx * 4);
    g_assert_cmphex(le32_to_cpu(sq.llq_descriptors_offset), ==,
                    sq_idx * LLQ_QUEUE_BYTES);
    g_assert_cmphex(le32_to_cpu(sq.llq_headers_offset), ==, 0);

    /* the LLQ memory BAR covers the queue and is plain writable memory */
    qpci_iounmap(&d->dev, d->mem);
    d->mem = qpci_iomap(&d->dev, ENA_TEST_MEM_BAR, &mem_size);
    g_assert_cmpuint(mem_size, >=, (sq_idx + 1) * LLQ_QUEUE_BYTES);
    qpci_io_writel(&d->dev, d->mem, sq_idx * LLQ_QUEUE_BYTES, 0xa5a5a5a5);
    g_assert_cmphex(qpci_io_readl(&d->dev, d->mem, sq_idx * LLQ_QUEUE_BYTES), ==,
                    0xa5a5a5a5);

    g_assert_cmpint(ena_destroy_sq(d, sq_idx, true), ==, ENA_ADMIN_SUCCESS);
}

static void test_exhaust_queues(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_acq_create_cq_resp_desc cq;
    struct ena_admin_acq_create_sq_resp_desc sq;
    struct ena_admin_get_feat_resp feat;
    uint64_t ring = guest_alloc(alloc, RING_BYTES);
    uint32_t pairs;
    int i;

    ena_bringup(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_MAX_QUEUES_EXT, 1, 0, 0, &feat),
                    ==, ENA_ADMIN_SUCCESS);
    pairs = le32_to_cpu(feat.u.max_queue_ext.max_queue_ext.max_tx_sq_num);
    g_assert_cmpuint(pairs, ==,
                     le32_to_cpu(feat.u.max_queue_ext.max_queue_ext.max_rx_sq_num));

    for (i = 0; i < 2 * pairs; i++) {
        g_assert_cmpint(ena_create_cq(d, 16, 4, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
        g_assert_cmpuint(le16_to_cpu(cq.cq_idx), ==, i);
    }
    g_assert_cmpint(ena_create_cq(d, 16, 4, 1, ring, &cq), ==,
                    ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE);

    for (i = 0; i < 2 * pairs; i++) {
        g_assert_cmpint(ena_create_sq(d, i & 1, ENA_ADMIN_PLACEMENT_POLICY_HOST,
                                      i, 16, ring, &sq), ==, ENA_ADMIN_SUCCESS);
        g_assert_cmpuint(le16_to_cpu(sq.sq_idx), ==, i);
    }
    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_HOST, 0,
                                  16, ring, &sq), ==,
                    ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE);

    /* reset releases everything */
    ena_dev_reset(d);
    ena_admin_init(d);
    g_assert_cmpint(ena_create_cq(d, 16, 4, 1, ring, &cq), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(cq.cq_idx), ==, 0);
    g_assert_cmpint(ena_create_sq(d, true, ENA_ADMIN_PLACEMENT_POLICY_HOST, 0,
                                  16, ring, &sq), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(sq.sq_idx), ==, 0);
}

static void register_ena_queue_test(void)
{
    QOSGraphTestOptions opts = {
        .before = ena_test_before,
    };

    qos_add_test("queue/create-destroy-cq", "ena", test_create_destroy_cq, &opts);
    qos_add_test("queue/cq-bad-params", "ena", test_cq_bad_params, &opts);
    qos_add_test("queue/create-destroy-sq", "ena", test_create_destroy_sq, &opts);
    qos_add_test("queue/sq-bad-params", "ena", test_sq_bad_params, &opts);
    qos_add_test("queue/llq-sq", "ena", test_llq_sq, &opts);
    qos_add_test("queue/exhaust", "ena", test_exhaust_queues, &opts);
}

libqos_init(register_ena_queue_test);
