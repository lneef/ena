/*
 * QTest testcase for the ENA NIC: admin queue, feature negotiation,
 * admin interrupt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "tests/ena_qos.h"

static const uint8_t test_mac[] = ENA_TEST_MAC;

#define REQUIRED_FEATURES \
    (BIT(ENA_ADMIN_DEVICE_ATTRIBUTES) | BIT(ENA_ADMIN_MAX_QUEUES_EXT) | \
     BIT(ENA_ADMIN_AENQ_CONFIG) | BIT(ENA_ADMIN_STATELESS_OFFLOAD_CONFIG) | \
     BIT(ENA_ADMIN_LINK_CONFIG) | BIT(ENA_ADMIN_HOST_ATTR_CONFIG) | \
     BIT(ENA_ADMIN_MTU) | BIT(ENA_ADMIN_LLQ) | \
     BIT(ENA_ADMIN_RSS_HASH_FUNCTION) | BIT(ENA_ADMIN_RSS_HASH_INPUT) | \
     BIT(ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG) | \
     BIT(ENA_ADMIN_INTERRUPT_MODERATION))

static void test_device_attributes(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_get_feat_resp resp;

    ena_bringup(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_DEVICE_ATTRIBUTES, 0, 0, 0,
                                    &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpmem(resp.u.dev_attr.mac_addr, 6, test_mac, 6);
    g_assert_cmphex(le32_to_cpu(resp.u.dev_attr.supported_features) &
                    REQUIRED_FEATURES, ==, REQUIRED_FEATURES);
    g_assert_cmpuint(le32_to_cpu(resp.u.dev_attr.phys_addr_width), ==, 48);
    g_assert_cmpuint(le32_to_cpu(resp.u.dev_attr.max_mtu), >=, 1500);
}

static void test_max_queues(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_get_feat_resp resp;
    struct ena_admin_queue_ext_feature_fields *f = &resp.u.max_queue_ext.max_queue_ext;

    ena_bringup(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_MAX_QUEUES_EXT, 0, 0, 0,
                                    &resp), ==, ENA_ADMIN_ILLEGAL_PARAMETER);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_MAX_QUEUES_EXT, 1, 0, 0,
                                    &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(resp.u.max_queue_ext.version, ==, 1);
    g_assert_cmpuint(le32_to_cpu(f->max_tx_sq_num), >=, 1);
    g_assert_cmpuint(le32_to_cpu(f->max_rx_sq_num), ==, le32_to_cpu(f->max_rx_cq_num));
    g_assert_cmpuint(le32_to_cpu(f->max_tx_sq_depth), ==, 1024);
    g_assert_cmpuint(le32_to_cpu(f->max_tx_cq_depth), ==, 1024);
    g_assert_cmpuint(le32_to_cpu(f->max_rx_sq_depth), ==, 1024);
    g_assert_cmpuint(le32_to_cpu(f->max_rx_cq_depth), ==, 1024);
    g_assert_cmpuint(le32_to_cpu(f->max_tx_header_size), ==, 96);
    g_assert_cmpuint(le16_to_cpu(f->max_per_packet_tx_descs), >=, 2);
    g_assert_cmpuint(le16_to_cpu(f->max_per_packet_rx_descs), >=, 1);

    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_MAX_QUEUES_NUM, 0, 0, 0,
                                    &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le32_to_cpu(resp.u.max_queue.max_sq_depth), ==, 1024);
    g_assert_cmpuint(le32_to_cpu(resp.u.max_queue.max_header_size), ==, 96);
}

static void test_static_features(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_get_feat_resp resp;
    uint32_t tx, rx;

    ena_bringup(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LINK_CONFIG, 0, 0, 0, &resp),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le32_to_cpu(resp.u.link.speed), >, 0);
    g_assert_cmphex(le32_to_cpu(resp.u.link.flags) &
                    ENA_ADMIN_GET_FEATURE_LINK_DESC_DUPLEX_MASK, !=, 0);

    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_STATELESS_OFFLOAD_CONFIG, 0,
                                    0, 0, &resp), ==, ENA_ADMIN_SUCCESS);
    tx = le32_to_cpu(resp.u.offload.tx);
    rx = le32_to_cpu(resp.u.offload.rx_supported);
    g_assert_cmphex(tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L3_CSUM_IPV4_MASK, !=, 0);
    g_assert_cmphex(tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_FULL_MASK, !=, 0);
    g_assert_cmphex(rx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L3_CSUM_IPV4_MASK, !=, 0);
    g_assert_cmphex(rx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV4_CSUM_MASK, !=, 0);
    g_assert_cmphex(le32_to_cpu(resp.u.offload.rx_enabled), ==, rx);

    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_INTERRUPT_MODERATION, 0, 0, 0,
                                    &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(resp.u.intr_moderation.intr_delay_resolution),
                     ==, 1);
}

static void test_host_attr_and_mtu(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_set_feat_cmd cmd = {};
    struct ena_admin_get_feat_resp resp;
    uint64_t host_info = guest_alloc(alloc, 4096);
    struct ena_admin_host_info hi = { .os_type = cpu_to_le32(3) };

    ena_bringup(d);
    /* os_type 3 is ENA_ADMIN_OS_DPDK, the only driver the device accepts */
    qtest_memwrite(d->dev.bus->qts, host_info, &hi, sizeof(hi));
    cmd.feat_common.feature_id = ENA_ADMIN_HOST_ATTR_CONFIG;
    cmd.u.host_attr.os_info_ba.mem_addr_low = cpu_to_le32(host_info);
    cmd.u.host_attr.os_info_ba.mem_addr_high = cpu_to_le16(host_info >> 32);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_SUCCESS);

    memset(&cmd, 0, sizeof(cmd));
    cmd.feat_common.feature_id = ENA_ADMIN_MTU;
    cmd.u.mtu.mtu = cpu_to_le32(64);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_ILLEGAL_PARAMETER);
    cmd.u.mtu.mtu = cpu_to_le32(1500);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_MTU, 0, 0, 0, &resp), ==,
                    ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le32_to_cpu(resp.u.raw[0]), ==, 1500);
}

static void test_aenq_config(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_set_feat_cmd cmd = {};
    struct ena_admin_get_feat_resp resp;
    uint32_t supported;

    ena_bringup(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_AENQ_CONFIG, 0, 0, 0, &resp),
                    ==, ENA_ADMIN_SUCCESS);
    supported = le32_to_cpu(resp.u.aenq.supported_groups);
    g_assert_cmphex(supported & BIT(ENA_ADMIN_LINK_CHANGE), !=, 0);
    g_assert_cmphex(supported & BIT(ENA_ADMIN_KEEP_ALIVE), !=, 0);
    g_assert_cmphex(le32_to_cpu(resp.u.aenq.enabled_groups), ==, 0);

    cmd.feat_common.feature_id = ENA_ADMIN_AENQ_CONFIG;
    cmd.u.aenq.enabled_groups = cpu_to_le32(~supported);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_ILLEGAL_PARAMETER);

    cmd.u.aenq.enabled_groups = cpu_to_le32(BIT(ENA_ADMIN_KEEP_ALIVE));
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_AENQ_CONFIG, 0, 0, 0, &resp),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_cmphex(le32_to_cpu(resp.u.aenq.enabled_groups), ==,
                    BIT(ENA_ADMIN_KEEP_ALIVE));
}

static void test_llq_feature(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_set_feat_cmd cmd = {};
    struct ena_admin_get_feat_resp resp;
    struct ena_admin_feature_llq_desc *llq = &resp.u.llq;

    ena_bringup(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LLQ,
                                    ENA_ADMIN_LLQ_FEATURE_VERSION_1, 0, 0,
                                    &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le32_to_cpu(llq->max_llq_num), >=, 1);
    g_assert_cmpuint(le32_to_cpu(llq->max_llq_depth), ==, 1024);
    g_assert_cmphex(le16_to_cpu(llq->header_location_ctrl_supported) &
                    ENA_ADMIN_INLINE_HEADER, !=, 0);
    g_assert_cmphex(le16_to_cpu(llq->entry_size_ctrl_supported) &
                    ENA_ADMIN_LIST_ENTRY_SIZE_128B, !=, 0);
    g_assert_cmphex(le16_to_cpu(llq->entry_size_ctrl_supported) &
                    ENA_ADMIN_LIST_ENTRY_SIZE_256B, !=, 0);
    g_assert_cmphex(le16_to_cpu(llq->desc_num_before_header_supported) &
                    ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2, !=, 0);
    g_assert_cmphex(le16_to_cpu(llq->descriptors_stride_ctrl_supported) &
                    ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY, !=, 0);
    g_assert_cmpuint(llq->feature_version, ==, ENA_ADMIN_LLQ_FEATURE_VERSION_1);
    g_assert_cmpuint(llq->entry_size_recommended, ==, ENA_ADMIN_LIST_ENTRY_SIZE_128B);
    g_assert_cmpuint(le16_to_cpu(llq->header_location_ctrl_enabled), ==, 0);

    cmd.feat_common.feature_id = ENA_ADMIN_LLQ;
    cmd.u.llq.header_location_ctrl_enabled = cpu_to_le16(ENA_ADMIN_HEADER_RING);
    cmd.u.llq.entry_size_ctrl_enabled = cpu_to_le16(ENA_ADMIN_LIST_ENTRY_SIZE_128B);
    cmd.u.llq.desc_num_before_header_enabled =
        cpu_to_le16(ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2);
    cmd.u.llq.descriptors_stride_ctrl_enabled =
        cpu_to_le16(ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_ILLEGAL_PARAMETER);

    cmd.u.llq.header_location_ctrl_enabled = cpu_to_le16(ENA_ADMIN_INLINE_HEADER);
    g_assert_cmpint(ena_set_feature(d, &cmd, 0, 0), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LLQ,
                                    ENA_ADMIN_LLQ_FEATURE_VERSION_1, 0, 0,
                                    &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(llq->header_location_ctrl_enabled), ==,
                     ENA_ADMIN_INLINE_HEADER);
    g_assert_cmpuint(le16_to_cpu(llq->entry_size_ctrl_enabled), ==,
                     ENA_ADMIN_LIST_ENTRY_SIZE_128B);
}

static void test_rss_features(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_set_feat_cmd cmd = {};
    struct ena_admin_get_feat_resp resp;
    struct ena_admin_feature_rss_flow_hash_control key;
    struct ena_admin_feature_rss_hash_control hc;
    struct ena_admin_rss_ind_table_entry tbl[128];
    uint64_t buf = guest_alloc(alloc, 4096);
    int i;

    ena_bringup(d);

    /* hash function and key */
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_RSS_HASH_FUNCTION, 0, buf,
                                    sizeof(key), &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmphex(le32_to_cpu(resp.u.flow_hash_func.supported_func) &
                    BIT(ENA_ADMIN_TOEPLITZ), !=, 0);
    g_assert_cmphex(le32_to_cpu(resp.u.flow_hash_func.selected_func), ==,
                    BIT(ENA_ADMIN_TOEPLITZ));
    qtest_memread(d->dev.bus->qts, buf, &key, sizeof(key));
    g_assert_cmpuint(le32_to_cpu(key.key_parts), ==, ENA_ADMIN_RSS_KEY_PARTS);

    for (i = 0; i < ENA_ADMIN_RSS_KEY_PARTS; i++) {
        key.key[i] = cpu_to_le32(0x01010101 * (i + 1));
    }
    qtest_memwrite(d->dev.bus->qts, buf, &key, sizeof(key));
    cmd.feat_common.feature_id = ENA_ADMIN_RSS_HASH_FUNCTION;
    cmd.u.flow_hash_func.selected_func = cpu_to_le32(BIT(ENA_ADMIN_CRC32));
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(key)), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    cmd.u.flow_hash_func.selected_func = cpu_to_le32(BIT(ENA_ADMIN_TOEPLITZ));
    cmd.u.flow_hash_func.init_val = cpu_to_le32(0x55);
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(key)), ==, ENA_ADMIN_SUCCESS);
    qtest_memset(d->dev.bus->qts, buf, 0, sizeof(key));
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_RSS_HASH_FUNCTION, 0, buf,
                                    sizeof(key), &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmphex(le32_to_cpu(resp.u.flow_hash_func.init_val), ==, 0x55);
    qtest_memread(d->dev.bus->qts, buf, &key, sizeof(key));
    for (i = 0; i < ENA_ADMIN_RSS_KEY_PARTS; i++) {
        g_assert_cmphex(le32_to_cpu(key.key[i]), ==, 0x01010101 * (i + 1));
    }
    /* only a full 40-byte key is accepted */
    key.key_parts = cpu_to_le32(5);
    qtest_memwrite(d->dev.bus->qts, buf, &key, sizeof(key));
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(key)), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);

    /* hash input */
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_RSS_HASH_INPUT, 0, buf,
                                    sizeof(hc), &resp), ==, ENA_ADMIN_SUCCESS);
    qtest_memread(d->dev.bus->qts, buf, &hc, sizeof(hc));
    g_assert_cmphex(le16_to_cpu(hc.supported_fields[ENA_ADMIN_RSS_TCP4].fields),
                    ==, ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA |
                    ENA_ADMIN_RSS_L4_DP | ENA_ADMIN_RSS_L4_SP);
    g_assert_cmphex(le16_to_cpu(hc.supported_fields[ENA_ADMIN_RSS_IP4].fields),
                    ==, ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA);
    g_assert_cmphex(le16_to_cpu(hc.selected_fields[ENA_ADMIN_RSS_UDP4].fields),
                    ==, ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA |
                    ENA_ADMIN_RSS_L4_DP | ENA_ADMIN_RSS_L4_SP);
    hc.selected_fields[ENA_ADMIN_RSS_UDP4].fields =
        cpu_to_le16(ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA);
    qtest_memwrite(d->dev.bus->qts, buf, &hc, sizeof(hc));
    memset(&cmd, 0, sizeof(cmd));
    cmd.feat_common.feature_id = ENA_ADMIN_RSS_HASH_INPUT;
    cmd.u.flow_hash_input.enabled_input_sort =
        cpu_to_le16(ENA_ADMIN_FEATURE_RSS_FLOW_HASH_INPUT_L3_SORT_MASK);
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(hc)), ==, ENA_ADMIN_SUCCESS);
    qtest_memset(d->dev.bus->qts, buf, 0, sizeof(hc));
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_RSS_HASH_INPUT, 0, buf,
                                    sizeof(hc), &resp), ==, ENA_ADMIN_SUCCESS);
    /* sorting is not supported: the request is accepted, nothing is enabled */
    g_assert_cmphex(le16_to_cpu(resp.u.flow_hash_input.supported_input_sort),
                    ==, 0);
    g_assert_cmphex(le16_to_cpu(resp.u.flow_hash_input.enabled_input_sort),
                    ==, 0);
    qtest_memread(d->dev.bus->qts, buf, &hc, sizeof(hc));
    g_assert_cmphex(le16_to_cpu(hc.selected_fields[ENA_ADMIN_RSS_UDP4].fields),
                    ==, ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA);

    /* source-only or destination-only selections are rejected */
    hc.selected_fields[ENA_ADMIN_RSS_UDP4].fields =
        cpu_to_le16(ENA_ADMIN_RSS_L3_SA | ENA_ADMIN_RSS_L4_SP | ENA_ADMIN_RSS_L4_DP);
    qtest_memwrite(d->dev.bus->qts, buf, &hc, sizeof(hc));
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(hc)), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);

    /* non-IP frames are not hashed; unsupported bits are masked, not stored */
    g_assert_cmphex(le16_to_cpu(hc.supported_fields[ENA_ADMIN_RSS_NOT_IP].fields),
                    ==, 0);
    hc.selected_fields[ENA_ADMIN_RSS_UDP4].fields =
        cpu_to_le16(ENA_ADMIN_RSS_L2_DA | ENA_ADMIN_RSS_L2_SA |
                    ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA |
                    ENA_ADMIN_RSS_L4_DP | ENA_ADMIN_RSS_L4_SP);
    hc.selected_fields[ENA_ADMIN_RSS_NOT_IP].fields =
        cpu_to_le16(ENA_ADMIN_RSS_L2_DA | ENA_ADMIN_RSS_L2_SA);
    qtest_memwrite(d->dev.bus->qts, buf, &hc, sizeof(hc));
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(hc)), ==, ENA_ADMIN_SUCCESS);
    qtest_memset(d->dev.bus->qts, buf, 0, sizeof(hc));
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_RSS_HASH_INPUT, 0, buf,
                                    sizeof(hc), &resp), ==, ENA_ADMIN_SUCCESS);
    qtest_memread(d->dev.bus->qts, buf, &hc, sizeof(hc));
    g_assert_cmphex(le16_to_cpu(hc.selected_fields[ENA_ADMIN_RSS_UDP4].fields),
                    ==, ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA |
                    ENA_ADMIN_RSS_L4_DP | ENA_ADMIN_RSS_L4_SP);
    g_assert_cmphex(le16_to_cpu(hc.selected_fields[ENA_ADMIN_RSS_NOT_IP].fields),
                    ==, 0);

    /* indirection table */
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG,
                                    ENA_ADMIN_RSS_FEATURE_VERSION_1, 0, 0, &resp),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(resp.u.ind_table.max_size), ==, 7);
    g_assert_cmpuint(le16_to_cpu(resp.u.ind_table.size), ==, 7);
    g_assert_cmphex(le32_to_cpu(resp.u.ind_table.inline_index), ==, 0xffffffff);

    for (i = 0; i < 128; i++) {
        tbl[i].cq_idx = cpu_to_le16(i % 4);
        tbl[i].reserved = 0;
    }
    qtest_memwrite(d->dev.bus->qts, buf, tbl, sizeof(tbl));
    memset(&cmd, 0, sizeof(cmd));
    cmd.feat_common.feature_id = ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG;
    cmd.u.ind_table.size = cpu_to_le16(6);
    cmd.u.ind_table.inline_index = cpu_to_le32(0xffffffff);
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(tbl)), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
    cmd.u.ind_table.size = cpu_to_le16(7);
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(tbl)), ==, ENA_ADMIN_SUCCESS);
    qtest_memset(d->dev.bus->qts, buf, 0xff, sizeof(tbl));
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG,
                                    0, buf, sizeof(tbl), &resp), ==,
                    ENA_ADMIN_SUCCESS);
    qtest_memread(d->dev.bus->qts, buf, tbl, sizeof(tbl));
    for (i = 0; i < 128; i++) {
        g_assert_cmpuint(le16_to_cpu(tbl[i].cq_idx), ==, i % 4);
    }
    tbl[5].cq_idx = cpu_to_le16(0xffff);
    qtest_memwrite(d->dev.bus->qts, buf, tbl, sizeof(tbl));
    g_assert_cmpint(ena_set_feature(d, &cmd, buf, sizeof(tbl)), ==,
                    ENA_ADMIN_ILLEGAL_PARAMETER);
}

static void test_get_stats(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_aq_get_stats_cmd cmd = {};
    struct ena_admin_acq_get_stats_resp resp;

    ena_bringup(d);
    /* the driver zeroes the command and sets only the type */
    cmd.aq_common_descriptor.opcode = ENA_ADMIN_GET_STATS;
    cmd.type = ENA_ADMIN_GET_STATS_TYPE_BASIC;
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &resp, sizeof(resp)),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(resp.u.basic_stats.tx_pkts_low, ==, 0);
    g_assert_cmpuint(resp.u.basic_stats.rx_pkts_low, ==, 0);

    cmd.type = ENA_ADMIN_GET_STATS_TYPE_ENI;
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &resp, sizeof(resp)),
                    ==, ENA_ADMIN_UNSUPPORTED_OPCODE);
}

static void test_bad_commands(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_aq_entry cmd = {};
    struct ena_admin_set_feat_cmd set = {};
    struct ena_admin_acq_entry resp;
    struct ena_admin_get_feat_resp feat;

    ena_bringup(d);
    cmd.aq_common_descriptor.opcode = 0x7f;
    g_assert_cmpint(ena_admin_cmd(d, &cmd, sizeof(cmd), &resp, sizeof(resp)),
                    ==, ENA_ADMIN_BAD_OPCODE);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_PHC_CONFIG, 0, 0, 0, &feat),
                    ==, ENA_ADMIN_UNSUPPORTED_OPCODE);
    set.feat_common.feature_id = ENA_ADMIN_INTERRUPT_MODERATION;
    g_assert_cmpint(ena_set_feature(d, &set, 0, 0), ==,
                    ENA_ADMIN_UNSUPPORTED_OPCODE);
    /* the queue keeps working after failed commands */
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_DEVICE_ATTRIBUTES, 0, 0, 0,
                                    &feat), ==, ENA_ADMIN_SUCCESS);
}

static void test_queue_wrap(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_get_feat_resp resp;
    int i;

    ena_bringup(d);
    /* more commands than the queue depth: phase flips and ids wrap */
    for (i = 0; i < 3 * ENA_TEST_AQ_DEPTH + 5; i++) {
        g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LINK_CONFIG, 0, 0, 0,
                                        &resp), ==, ENA_ADMIN_SUCCESS);
        g_assert_cmpuint(le16_to_cpu(resp.acq_common_desc.sq_head_indx), ==,
                         (uint16_t)(i + 1));
    }
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_ACQ_TAIL_OFF), ==,
                     3 * ENA_TEST_AQ_DEPTH + 5);
}

/* A doorbell more than depth ahead of the device head is ignored. */
static void test_doorbell_past_depth(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_get_feat_resp resp;

    ena_bringup(d);
    ena_reg_write(d, ENA_REGS_AQ_DB_OFF, ENA_TEST_AQ_DEPTH + 1);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_ACQ_TAIL_OFF), ==, 0);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LINK_CONFIG, 0, 0, 0, &resp),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(le16_to_cpu(resp.acq_common_desc.sq_head_indx), ==, 1);
}

static void test_admin_interrupt(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_get_feat_resp resp;

    ena_bringup(d);
    g_assert_false(ena_msix_fired(d, ENA_TEST_ADMIN_VECTOR));
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LINK_CONFIG, 0, 0, 0, &resp),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_true(ena_msix_fired(d, ENA_TEST_ADMIN_VECTOR));
    ena_msix_clear(d, ENA_TEST_ADMIN_VECTOR);

    /* polling mode: the driver masks the admin interrupt */
    ena_reg_write(d, ENA_REGS_INTR_MASK_OFF, 1);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LINK_CONFIG, 0, 0, 0, &resp),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_false(ena_msix_fired(d, ENA_TEST_ADMIN_VECTOR));

    ena_reg_write(d, ENA_REGS_INTR_MASK_OFF, 0);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_LINK_CONFIG, 0, 0, 0, &resp),
                    ==, ENA_ADMIN_SUCCESS);
    g_assert_true(ena_msix_fired(d, ENA_TEST_ADMIN_VECTOR));
}

static void test_doorbell_without_queue(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    QEna *d = obj;

    ena_dev_reset(d);
    /* no admin queue configured: the doorbell must not DMA or complete */
    ena_reg_write(d, ENA_REGS_AQ_DB_OFF, 1);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_ACQ_TAIL_OFF), ==, 0);
}

static void register_ena_admin_test(void)
{
    QOSGraphTestOptions opts = {
        .before = ena_test_before,
    };

    qos_add_test("admin/device-attributes", "ena", test_device_attributes, &opts);
    qos_add_test("admin/max-queues", "ena", test_max_queues, &opts);
    qos_add_test("admin/static-features", "ena", test_static_features, &opts);
    qos_add_test("admin/host-attr-mtu", "ena", test_host_attr_and_mtu, &opts);
    qos_add_test("admin/aenq-config", "ena", test_aenq_config, &opts);
    qos_add_test("admin/llq-feature", "ena", test_llq_feature, &opts);
    qos_add_test("admin/rss-features", "ena", test_rss_features, &opts);
    qos_add_test("admin/get-stats", "ena", test_get_stats, &opts);
    qos_add_test("admin/bad-commands", "ena", test_bad_commands, &opts);
    qos_add_test("admin/queue-wrap", "ena", test_queue_wrap, &opts);
    qos_add_test("admin/doorbell-past-depth", "ena", test_doorbell_past_depth,
                 &opts);
    qos_add_test("admin/interrupt", "ena", test_admin_interrupt, &opts);
    qos_add_test("admin/doorbell-without-queue", "ena",
                 test_doorbell_without_queue, &opts);
}

libqos_init(register_ena_admin_test);
