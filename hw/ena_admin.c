/*
 * QEMU Amazon Elastic Network Adapter (ENA) emulation: admin queue
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/pci/msix.h"
#include "net/eth.h"
#include "hw/ena.h"

#define REG(s, off) ((s)->reg[(off) / 4])

#define ENA_SUPPORTED_FEATURES \
    (BIT(ENA_ADMIN_DEVICE_ATTRIBUTES) | \
     BIT(ENA_ADMIN_MAX_QUEUES_NUM) | \
     BIT(ENA_ADMIN_MAX_QUEUES_EXT) | \
     BIT(ENA_ADMIN_LLQ) | \
     BIT(ENA_ADMIN_RSS_HASH_FUNCTION) | \
     BIT(ENA_ADMIN_STATELESS_OFFLOAD_CONFIG) | \
     BIT(ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG) | \
     BIT(ENA_ADMIN_MTU) | \
     BIT(ENA_ADMIN_RSS_HASH_INPUT) | \
     BIT(ENA_ADMIN_INTERRUPT_MODERATION) | \
     BIT(ENA_ADMIN_AENQ_CONFIG) | \
     BIT(ENA_ADMIN_LINK_CONFIG) | \
     BIT(ENA_ADMIN_HOST_ATTR_CONFIG))

#define ENA_SUPPORTED_AENQ_GROUPS \
    (BIT(ENA_ADMIN_LINK_CHANGE) | BIT(ENA_ADMIN_KEEP_ALIVE))

#define ENA_TX_OFFLOADS \
    (ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L3_CSUM_IPV4_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_PART_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_FULL_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV6_CSUM_PART_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV6_CSUM_FULL_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV4_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV6_MASK)

#define ENA_RX_OFFLOADS \
    (ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L3_CSUM_IPV4_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV4_CSUM_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV6_CSUM_MASK | \
     ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_HASH_MASK)

#define ENA_RSS_L3L4_FIELDS \
    (ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA | \
     ENA_ADMIN_RSS_L4_DP | ENA_ADMIN_RSS_L4_SP)
#define ENA_RSS_L3_FIELDS (ENA_ADMIN_RSS_L3_DA | ENA_ADMIN_RSS_L3_SA)

static bool ena_valid_depth(uint32_t depth)
{
    return is_power_of_2(depth) && depth >= ENA_MIN_QUEUE_DEPTH &&
           depth <= ENA_MAX_QUEUE_DEPTH;
}

static int ena_create_cq(EnaState *s, const struct ena_admin_aq_entry *cmd,
                         struct ena_admin_acq_entry *resp)
{
    const struct ena_admin_aq_create_cq_cmd *c = (const void *)cmd;
    struct ena_admin_acq_create_cq_resp_desc *r = (void *)resp;
    uint16_t depth = le16_to_cpu(c->cq_depth);
    uint8_t words = c->cq_caps_2 & ENA_ADMIN_AQ_CREATE_CQ_CMD_CQ_ENTRY_SIZE_WORDS_MASK;
    uint32_t vector = le32_to_cpu(c->msix_vector);
    EnaCq *cq;
    int i;

    if (!ena_valid_depth(depth) || (words != 2 && words != 4 && words != 8)) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    if (!ena_mem_addr(&c->cq_ba)) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    /* IO vectors are 1..8; -1 means no interrupt */
    if ((c->cq_caps_1 & ENA_ADMIN_AQ_CREATE_CQ_CMD_INTERRUPT_MODE_ENABLED_MASK) &&
        vector != ENA_MSIX_VECTOR_NONE &&
        (vector == ENA_ADMIN_MSIX_VECTOR || vector >= ENA_MSIX_VECTORS)) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    for (i = 0; i < ENA_MAX_CQ && s->cq[i].used; i++) {
    }
    if (i == ENA_MAX_CQ) {
        return ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE;
    }

    cq = &s->cq[i];
    cq->used = true;
    cq->base = ena_mem_addr(&c->cq_ba);
    cq->depth = depth;
    cq->entry_size = words * 4;
    cq->intr_enabled = !!(c->cq_caps_1 &
                          ENA_ADMIN_AQ_CREATE_CQ_CMD_INTERRUPT_MODE_ENABLED_MASK);
    cq->msix_vector = vector;
    cq->tail = 0;
    cq->phase = true;

    r->cq_idx = cpu_to_le16(i);
    r->cq_actual_depth = cpu_to_le16(depth);
    r->numa_node_register_offset = 0;
    r->cq_head_db_register_offset = 0;
    r->cq_interrupt_unmask_register_offset =
        cpu_to_le32(ENA_REG_CQ_UNMASK_BASE + i * 4);
    return ENA_ADMIN_SUCCESS;
}

static int ena_destroy_cq(EnaState *s, const struct ena_admin_aq_entry *cmd,
                          struct ena_admin_acq_entry *resp)
{
    const struct ena_admin_aq_destroy_cq_cmd *c = (const void *)cmd;
    uint16_t idx = le16_to_cpu(c->cq_idx);
    int i;

    if (idx >= ENA_MAX_CQ || !s->cq[idx].used) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    for (i = 0; i < ENA_MAX_SQ; i++) {
        if (s->sq[i].used && s->sq[i].cq_idx == idx) {
            return ENA_ADMIN_RESOURCE_BUSY;
        }
    }
    s->cq[idx].used = false;
    return ENA_ADMIN_SUCCESS;
}

static int ena_create_sq(EnaState *s, const struct ena_admin_aq_entry *cmd,
                         struct ena_admin_acq_entry *resp)
{
    const struct ena_admin_aq_create_sq_cmd *c = (const void *)cmd;
    struct ena_admin_acq_create_sq_resp_desc *r = (void *)resp;
    uint8_t dir = (c->sq_identity & ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_MASK) >>
                  ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_SHIFT;
    uint8_t placement = c->sq_caps_2 & ENA_ADMIN_AQ_CREATE_SQ_CMD_PLACEMENT_POLICY_MASK;
    uint8_t completion = (c->sq_caps_2 & ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_MASK) >>
                         ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_SHIFT;
    uint16_t cq_idx = le16_to_cpu(c->cq_idx);
    uint16_t depth = le16_to_cpu(c->sq_depth);
    bool is_tx = dir == ENA_ADMIN_SQ_DIRECTION_TX;
    bool llq = placement == ENA_ADMIN_PLACEMENT_POLICY_DEV;
    EnaSq *sq;
    int i;

    if (dir != ENA_ADMIN_SQ_DIRECTION_TX && dir != ENA_ADMIN_SQ_DIRECTION_RX) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    if (placement != ENA_ADMIN_PLACEMENT_POLICY_HOST && !llq) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    if (llq && (!is_tx || !s->llq_enabled)) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    if (completion != ENA_ADMIN_COMPLETION_POLICY_DESC) {
        return ENA_ADMIN_UNSUPPORTED_OPCODE;
    }
    if (!(c->sq_caps_3 & ENA_ADMIN_AQ_CREATE_SQ_CMD_IS_PHYSICALLY_CONTIGUOUS_MASK)) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    if (cq_idx >= ENA_MAX_CQ || !s->cq[cq_idx].used || !ena_valid_depth(depth)) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    /* one completion slot per submitted descriptor */
    if (depth > s->cq[cq_idx].depth || (!llq && !ena_mem_addr(&c->sq_ba))) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    if (ena_mem_addr(&c->sq_head_writeback)) {
        ena_unsupported("sq head writeback");
    }
    for (i = 0; i < ENA_MAX_SQ && s->sq[i].used; i++) {
    }
    if (i == ENA_MAX_SQ) {
        return ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE;
    }

    if (llq && (uint32_t)depth * s->llq_entry_size > ENA_LLQ_QUEUE_BYTES) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }

    sq = &s->sq[i];
    memset(sq, 0, sizeof(*sq));
    sq->used = true;
    sq->is_tx = is_tx;
    sq->llq = llq;
    sq->cq_idx = cq_idx;
    sq->base = llq ? 0 : ena_mem_addr(&c->sq_ba);
    sq->depth = depth;
    sq->entry_size = llq ? s->llq_entry_size : 0;

    r->sq_idx = cpu_to_le16(i);
    r->sq_doorbell_offset = cpu_to_le32(ENA_REG_SQ_DB_BASE + i * 4);
    r->llq_descriptors_offset = cpu_to_le32(llq ? i * ENA_LLQ_QUEUE_BYTES : 0);
    r->llq_headers_offset = 0;
    return ENA_ADMIN_SUCCESS;
}

static int ena_destroy_sq(EnaState *s, const struct ena_admin_aq_entry *cmd,
                          struct ena_admin_acq_entry *resp)
{
    const struct ena_admin_aq_destroy_sq_cmd *c = (const void *)cmd;
    uint16_t idx = le16_to_cpu(c->sq.sq_idx);
    uint8_t dir = (c->sq.sq_identity & ENA_ADMIN_SQ_SQ_DIRECTION_MASK) >>
                  ENA_ADMIN_SQ_SQ_DIRECTION_SHIFT;

    if (idx >= ENA_MAX_SQ || !s->sq[idx].used ||
        s->sq[idx].is_tx != (dir == ENA_ADMIN_SQ_DIRECTION_TX)) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    s->sq[idx].used = false;
    return ENA_ADMIN_SUCCESS;
}

/* Control buffer: the driver hands a direct pointer to the data. */
static uint64_t ena_ctrl_buf(uint8_t flags,
                             const struct ena_admin_ctrl_buff_info *info,
                             size_t need)
{
    if (le32_to_cpu(info->length) < need) {
        return 0;
    }
    if (!(flags & ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK)) {
        ena_unsupported("inline admin control data");
    }
    return ena_mem_addr(&info->address);
}

/* Non-IP frames and IPv6 with extension headers (the _EX protocols) are not hashed. */
static uint16_t ena_rss_supported_fields(int proto)
{
    switch (proto) {
    case ENA_ADMIN_RSS_TCP4:
    case ENA_ADMIN_RSS_UDP4:
    case ENA_ADMIN_RSS_TCP6:
    case ENA_ADMIN_RSS_UDP6:
        return ENA_RSS_L3L4_FIELDS;
    case ENA_ADMIN_RSS_IP4:
    case ENA_ADMIN_RSS_IP6:
    case ENA_ADMIN_RSS_IP4_FRAG:
        return ENA_RSS_L3_FIELDS;
    default:
        return 0;
    }
}

static void ena_fill_hash_ctrl(EnaState *s,
                               struct ena_admin_feature_rss_hash_control *hc)
{
    int i;

    memset(hc, 0, sizeof(*hc));
    for (i = 0; i < ENA_ADMIN_RSS_PROTO_NUM; i++) {
        hc->supported_fields[i].fields = cpu_to_le16(ena_rss_supported_fields(i));
        hc->selected_fields[i].fields = cpu_to_le16(s->rss.fields[i]);
    }
}

static int ena_get_feature(EnaState *s, const struct ena_admin_aq_entry *cmd,
                           struct ena_admin_acq_entry *resp)
{
    const struct ena_admin_get_feat_cmd *c = (const void *)cmd;
    struct ena_admin_get_feat_resp *r = (void *)resp;
    uint64_t buf;
    int i;

    if (c->feat_common.flags & ENA_ADMIN_GET_SET_FEATURE_COMMON_DESC_SELECT_MASK) {
        ena_unsupported("feature select 0x%x", c->feat_common.flags);
    }
    switch (c->feat_common.feature_id) {
    case ENA_ADMIN_DEVICE_ATTRIBUTES:
        r->u.dev_attr.impl_id = cpu_to_le32(ENA_CTRL_VERSION_IMPL_ID);
        r->u.dev_attr.device_version = 0;
        r->u.dev_attr.supported_features = cpu_to_le32(ENA_SUPPORTED_FEATURES);
        r->u.dev_attr.capabilities = 0;
        r->u.dev_attr.phys_addr_width = cpu_to_le32(ENA_DMA_ADDR_WIDTH);
        r->u.dev_attr.virt_addr_width = cpu_to_le32(ENA_DMA_ADDR_WIDTH);
        memcpy(r->u.dev_attr.mac_addr, s->conf.macaddr.a, ETH_ALEN);
        r->u.dev_attr.max_mtu = cpu_to_le32(ENA_MAX_MTU);
        return ENA_ADMIN_SUCCESS;

    case ENA_ADMIN_MAX_QUEUES_NUM:
        r->u.max_queue.max_sq_num = cpu_to_le32(ENA_MAX_IO_QUEUES);
        r->u.max_queue.max_sq_depth = cpu_to_le32(ENA_MAX_QUEUE_DEPTH);
        r->u.max_queue.max_cq_num = cpu_to_le32(ENA_MAX_IO_QUEUES);
        r->u.max_queue.max_cq_depth = cpu_to_le32(ENA_MAX_QUEUE_DEPTH);
        r->u.max_queue.max_legacy_llq_num = 0;
        r->u.max_queue.max_legacy_llq_depth = 0;
        r->u.max_queue.max_header_size = cpu_to_le32(ENA_MAX_TX_HEADER_SIZE);
        r->u.max_queue.max_packet_tx_descs = cpu_to_le16(ENA_MAX_PKT_DESCS);
        r->u.max_queue.max_packet_rx_descs = cpu_to_le16(ENA_MAX_PKT_DESCS);
        return ENA_ADMIN_SUCCESS;

    case ENA_ADMIN_MAX_QUEUES_EXT: {
        struct ena_admin_queue_ext_feature_fields *f =
            &r->u.max_queue_ext.max_queue_ext;

        if (c->feat_common.feature_version != 1) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        r->u.max_queue_ext.version = 1;
        f->max_tx_sq_num = cpu_to_le32(ENA_MAX_IO_QUEUES);
        f->max_tx_cq_num = cpu_to_le32(ENA_MAX_IO_QUEUES);
        f->max_rx_sq_num = cpu_to_le32(ENA_MAX_IO_QUEUES);
        f->max_rx_cq_num = cpu_to_le32(ENA_MAX_IO_QUEUES);
        f->max_tx_sq_depth = cpu_to_le32(ENA_MAX_QUEUE_DEPTH);
        f->max_tx_cq_depth = cpu_to_le32(ENA_MAX_QUEUE_DEPTH);
        f->max_rx_sq_depth = cpu_to_le32(ENA_MAX_QUEUE_DEPTH);
        f->max_rx_cq_depth = cpu_to_le32(ENA_MAX_QUEUE_DEPTH);
        f->max_tx_header_size = cpu_to_le32(ENA_MAX_TX_HEADER_SIZE);
        f->max_per_packet_tx_descs = cpu_to_le16(ENA_MAX_PKT_DESCS);
        f->max_per_packet_rx_descs = cpu_to_le16(ENA_MAX_PKT_DESCS);
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_LLQ:
        r->u.llq.max_llq_num = cpu_to_le32(ENA_MAX_IO_QUEUES);
        r->u.llq.max_llq_depth = cpu_to_le32(ENA_MAX_QUEUE_DEPTH);
        r->u.llq.header_location_ctrl_supported = cpu_to_le16(ENA_ADMIN_INLINE_HEADER);
        r->u.llq.header_location_ctrl_enabled =
            cpu_to_le16(s->llq_enabled ? ENA_ADMIN_INLINE_HEADER : 0);
        r->u.llq.entry_size_ctrl_supported =
            cpu_to_le16(ENA_ADMIN_LIST_ENTRY_SIZE_128B | ENA_ADMIN_LIST_ENTRY_SIZE_256B);
        r->u.llq.entry_size_ctrl_enabled =
            cpu_to_le16(s->llq_entry_size == ENA_LLQ_LARGE_ENTRY_SIZE ?
                        ENA_ADMIN_LIST_ENTRY_SIZE_256B :
                        s->llq_enabled ? ENA_ADMIN_LIST_ENTRY_SIZE_128B : 0);
        r->u.llq.desc_num_before_header_supported =
            cpu_to_le16(ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2);
        r->u.llq.desc_num_before_header_enabled =
            cpu_to_le16(s->llq_enabled ? ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2 : 0);
        r->u.llq.descriptors_stride_ctrl_supported =
            cpu_to_le16(ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY);
        r->u.llq.descriptors_stride_ctrl_enabled =
            cpu_to_le16(s->llq_enabled ? ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY : 0);
        r->u.llq.feature_version = MIN(c->feat_common.feature_version,
                                       ENA_ADMIN_LLQ_FEATURE_VERSION_1);
        r->u.llq.entry_size_recommended = s->llq_large ?
            ENA_ADMIN_LIST_ENTRY_SIZE_256B : ENA_ADMIN_LIST_ENTRY_SIZE_128B;
        /* no double-sized memory BAR: the driver halves the depth for 256B */
        r->u.llq.max_wide_llq_depth = 0;
        r->u.llq.accel_mode.u.get.supported_flags =
            cpu_to_le16(BIT(ENA_ADMIN_DISABLE_META_CACHING));
        r->u.llq.accel_mode.u.get.max_tx_burst_size = 0;
        return ENA_ADMIN_SUCCESS;

    case ENA_ADMIN_AENQ_CONFIG:
        r->u.aenq.supported_groups = cpu_to_le32(ENA_SUPPORTED_AENQ_GROUPS);
        r->u.aenq.enabled_groups = cpu_to_le32(s->aenq_groups);
        return ENA_ADMIN_SUCCESS;

    case ENA_ADMIN_LINK_CONFIG:
        r->u.link.speed = cpu_to_le32(ENA_LINK_SPEED_MBPS);
        r->u.link.supported = cpu_to_le32(ENA_ADMIN_LINK_SPEED_10G);
        r->u.link.flags = cpu_to_le32(ENA_ADMIN_GET_FEATURE_LINK_DESC_DUPLEX_MASK);
        return ENA_ADMIN_SUCCESS;

    case ENA_ADMIN_STATELESS_OFFLOAD_CONFIG:
        r->u.offload.tx = cpu_to_le32(ENA_TX_OFFLOADS);
        r->u.offload.rx_supported = cpu_to_le32(ENA_RX_OFFLOADS);
        r->u.offload.rx_enabled = cpu_to_le32(ENA_RX_OFFLOADS);
        return ENA_ADMIN_SUCCESS;

    case ENA_ADMIN_INTERRUPT_MODERATION:
        r->u.intr_moderation.intr_delay_resolution = cpu_to_le16(1);
        return ENA_ADMIN_SUCCESS;

    case ENA_ADMIN_MTU:
        r->u.raw[0] = cpu_to_le32(s->mtu);
        return ENA_ADMIN_SUCCESS;

    case ENA_ADMIN_RSS_HASH_FUNCTION: {
        struct ena_admin_feature_rss_flow_hash_control key;

        r->u.flow_hash_func.supported_func = cpu_to_le32(BIT(ENA_ADMIN_TOEPLITZ));
        r->u.flow_hash_func.selected_func = cpu_to_le32(BIT(ENA_ADMIN_TOEPLITZ));
        r->u.flow_hash_func.init_val = cpu_to_le32(s->rss.init_val);
        /* the driver reads supported_func without a buffer (ena_com.c:2767) */
        buf = ena_ctrl_buf(c->aq_common_descriptor.flags, &c->control_buffer, sizeof(key));
        if (buf) {
            memset(&key, 0, sizeof(key));
            key.key_parts = cpu_to_le32(s->rss.key_parts);
            for (i = 0; i < ENA_ADMIN_RSS_KEY_PARTS; i++) {
                key.key[i] = cpu_to_le32(s->rss.key[i]);
            }
            ena_dma_write(s, buf, &key, sizeof(key));
        }
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_RSS_HASH_INPUT: {
        struct ena_admin_feature_rss_hash_control hc;

        r->u.flow_hash_input.supported_input_sort = 0;
        r->u.flow_hash_input.enabled_input_sort = 0;
        buf = ena_ctrl_buf(c->aq_common_descriptor.flags, &c->control_buffer, sizeof(hc));
        if (buf) {
            ena_fill_hash_ctrl(s, &hc);
            ena_dma_write(s, buf, &hc, sizeof(hc));
        }
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG: {
        struct ena_admin_rss_ind_table_entry tbl[ENA_RSS_IND_TBL_SIZE];

        r->u.ind_table.min_size = cpu_to_le16(ENA_RSS_IND_TBL_LOG_SIZE);
        r->u.ind_table.max_size = cpu_to_le16(ENA_RSS_IND_TBL_LOG_SIZE);
        r->u.ind_table.size = cpu_to_le16(ENA_RSS_IND_TBL_LOG_SIZE);
        r->u.ind_table.flags = 0;
        r->u.ind_table.inline_index = cpu_to_le32(0xffffffff);
        buf = ena_ctrl_buf(c->aq_common_descriptor.flags, &c->control_buffer, sizeof(tbl));
        if (buf) {
            memset(tbl, 0, sizeof(tbl));
            for (i = 0; i < ENA_RSS_IND_TBL_SIZE; i++) {
                tbl[i].cq_idx = cpu_to_le16(s->rss.ind_tbl[i]);
            }
            ena_dma_write(s, buf, tbl, sizeof(tbl));
        }
        return ENA_ADMIN_SUCCESS;
    }

    default:
        return ENA_ADMIN_UNSUPPORTED_OPCODE;
    }
}

static int ena_set_feature(EnaState *s, const struct ena_admin_aq_entry *cmd,
                           struct ena_admin_acq_entry *resp)
{
    const struct ena_admin_set_feat_cmd *c = (const void *)cmd;
    uint64_t buf;
    int i;

    if (c->feat_common.flags & ENA_ADMIN_GET_SET_FEATURE_COMMON_DESC_SELECT_MASK) {
        ena_unsupported("feature select 0x%x", c->feat_common.flags);
    }
    switch (c->feat_common.feature_id) {
    case ENA_ADMIN_HOST_ATTR_CONFIG: {
        struct ena_admin_host_info hi;

        s->host_info_addr = ena_mem_addr(&c->u.host_attr.os_info_ba);
        s->debug_area_addr = ena_mem_addr(&c->u.host_attr.debug_ba);
        s->debug_area_size = le32_to_cpu(c->u.host_attr.debug_area_size);
        if (!s->host_info_addr) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        ena_dma_read(s, s->host_info_addr, &hi, sizeof(hi));
        /* only the DPDK driver's contract is emulated */
        assert(le32_to_cpu(hi.os_type) == ENA_ADMIN_OS_DPDK);
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_AENQ_CONFIG: {
        uint32_t groups = le32_to_cpu(c->u.aenq.enabled_groups);

        if (groups & ~ENA_SUPPORTED_AENQ_GROUPS) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        ena_aenq_config(s, groups);
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_MTU: {
        uint32_t mtu = le32_to_cpu(c->u.mtu.mtu);

        if (mtu < ENA_MIN_MTU || mtu > ENA_MAX_MTU) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        s->mtu = mtu;
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_LLQ: {
        uint16_t entry = le16_to_cpu(c->u.llq.entry_size_ctrl_enabled);

        if (le16_to_cpu(c->u.llq.header_location_ctrl_enabled) != ENA_ADMIN_INLINE_HEADER ||
            (entry != ENA_ADMIN_LIST_ENTRY_SIZE_128B &&
             entry != ENA_ADMIN_LIST_ENTRY_SIZE_256B) ||
            le16_to_cpu(c->u.llq.desc_num_before_header_enabled) !=
                ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2 ||
            le16_to_cpu(c->u.llq.descriptors_stride_ctrl_enabled) !=
                ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        s->llq_enabled = true;
        s->llq_entry_size = entry == ENA_ADMIN_LIST_ENTRY_SIZE_256B ?
                            ENA_LLQ_LARGE_ENTRY_SIZE : ENA_LLQ_ENTRY_SIZE;
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_RSS_HASH_FUNCTION: {
        struct ena_admin_feature_rss_flow_hash_control key;
        uint32_t func = le32_to_cpu(c->u.flow_hash_func.selected_func);

        /* The hash function is fixed to Toeplitz. */
        if (func != BIT(ENA_ADMIN_TOEPLITZ)) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        buf = ena_ctrl_buf(c->aq_common_descriptor.flags, &c->control_buffer, sizeof(key));
        if (!buf) {
            return ENA_ADMIN_MALFORMED_REQUEST;
        }
        ena_dma_read(s, buf, &key, sizeof(key));
        if (le32_to_cpu(key.key_parts) != ENA_ADMIN_RSS_KEY_PARTS) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        s->rss.key_parts = le32_to_cpu(key.key_parts);
        for (i = 0; i < ENA_ADMIN_RSS_KEY_PARTS; i++) {
            s->rss.key[i] = le32_to_cpu(key.key[i]);
        }
        s->rss.init_val = le32_to_cpu(c->u.flow_hash_func.init_val);
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_RSS_HASH_INPUT: {
        struct ena_admin_feature_rss_hash_control hc;
        uint16_t sel[ENA_ADMIN_RSS_PROTO_NUM];

        /*
         * The driver selects unsupported bits (L2, sorting) unconditionally;
         * they are masked. Address-only or port-only selections are rejected.
         */
        buf = ena_ctrl_buf(c->aq_common_descriptor.flags, &c->control_buffer, sizeof(hc));
        if (!buf) {
            return ENA_ADMIN_MALFORMED_REQUEST;
        }
        ena_dma_read(s, buf, &hc, sizeof(hc));
        for (i = 0; i < ENA_ADMIN_RSS_PROTO_NUM; i++) {
            sel[i] = le16_to_cpu(hc.selected_fields[i].fields) &
                     ena_rss_supported_fields(i);
            if (sel[i] != 0 && sel[i] != ENA_RSS_L3_FIELDS &&
                sel[i] != ENA_RSS_L3L4_FIELDS) {
                return ENA_ADMIN_ILLEGAL_PARAMETER;
            }
        }
        memcpy(s->rss.fields, sel, sizeof(sel));
        return ENA_ADMIN_SUCCESS;
    }

    case ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG: {
        struct ena_admin_rss_ind_table_entry tbl[ENA_RSS_IND_TBL_SIZE];

        if (le16_to_cpu(c->u.ind_table.size) != ENA_RSS_IND_TBL_LOG_SIZE) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        if (le32_to_cpu(c->u.ind_table.inline_index) != 0xffffffff) {
            ena_unsupported("inline indirection table update");
        }
        buf = ena_ctrl_buf(c->aq_common_descriptor.flags, &c->control_buffer, sizeof(tbl));
        if (!buf) {
            return ENA_ADMIN_ILLEGAL_PARAMETER;
        }
        ena_dma_read(s, buf, tbl, sizeof(tbl));
        /* entries name RX SQ indices; the ABI field is called cq_idx */
        for (i = 0; i < ENA_RSS_IND_TBL_SIZE; i++) {
            uint16_t idx = le16_to_cpu(tbl[i].cq_idx);

            if (idx >= ENA_MAX_SQ || !s->sq[idx].used || s->sq[idx].is_tx) {
                return ENA_ADMIN_ILLEGAL_PARAMETER;
            }
        }
        for (i = 0; i < ENA_RSS_IND_TBL_SIZE; i++) {
            s->rss.ind_tbl[i] = le16_to_cpu(tbl[i].cq_idx);
        }
        return ENA_ADMIN_SUCCESS;
    }

    default:
        return ENA_ADMIN_UNSUPPORTED_OPCODE;
    }
}

static int ena_get_stats(EnaState *s, const struct ena_admin_aq_entry *cmd,
                         struct ena_admin_acq_entry *resp)
{
    const struct ena_admin_aq_get_stats_cmd *c = (const void *)cmd;
    struct ena_admin_acq_get_stats_resp *r = (void *)resp;
    struct ena_admin_basic_stats *b = &r->u.basic_stats;

    if (c->type != ENA_ADMIN_GET_STATS_TYPE_BASIC) {
        return ENA_ADMIN_UNSUPPORTED_OPCODE;
    }
    /* scope and queue_idx stay 0 in the driver's command and are ignored */
    if (le16_to_cpu(c->device_id) != 0 && le16_to_cpu(c->device_id) != 0xffff) {
        return ENA_ADMIN_ILLEGAL_PARAMETER;
    }
    b->tx_bytes_low = cpu_to_le32(s->tx_bytes);
    b->tx_bytes_high = cpu_to_le32(s->tx_bytes >> 32);
    b->tx_pkts_low = cpu_to_le32(s->tx_pkts);
    b->tx_pkts_high = cpu_to_le32(s->tx_pkts >> 32);
    b->rx_bytes_low = cpu_to_le32(s->rx_bytes);
    b->rx_bytes_high = cpu_to_le32(s->rx_bytes >> 32);
    b->rx_pkts_low = cpu_to_le32(s->rx_pkts);
    b->rx_pkts_high = cpu_to_le32(s->rx_pkts >> 32);
    b->rx_drops_low = cpu_to_le32(s->rx_drops);
    b->rx_drops_high = cpu_to_le32(s->rx_drops >> 32);
    b->tx_drops_low = cpu_to_le32(s->tx_drops);
    b->tx_drops_high = cpu_to_le32(s->tx_drops >> 32);
    return ENA_ADMIN_SUCCESS;
}

static int ena_admin_exec(EnaState *s, const struct ena_admin_aq_entry *cmd,
                          struct ena_admin_acq_entry *resp)
{
    switch (cmd->aq_common_descriptor.opcode) {
    case ENA_ADMIN_CREATE_SQ:
        return ena_create_sq(s, cmd, resp);
    case ENA_ADMIN_DESTROY_SQ:
        return ena_destroy_sq(s, cmd, resp);
    case ENA_ADMIN_CREATE_CQ:
        return ena_create_cq(s, cmd, resp);
    case ENA_ADMIN_DESTROY_CQ:
        return ena_destroy_cq(s, cmd, resp);
    case ENA_ADMIN_GET_FEATURE:
        return ena_get_feature(s, cmd, resp);
    case ENA_ADMIN_SET_FEATURE:
        return ena_set_feature(s, cmd, resp);
    case ENA_ADMIN_GET_STATS:
        return ena_get_stats(s, cmd, resp);
    default:
        return ENA_ADMIN_BAD_OPCODE;
    }
}

void ena_admin_process(EnaState *s)
{
    uint64_t aq_base = ((uint64_t)REG(s, ENA_REGS_AQ_BASE_HI_OFF) << 32) |
                       REG(s, ENA_REGS_AQ_BASE_LO_OFF);
    uint64_t acq_base = ((uint64_t)REG(s, ENA_REGS_ACQ_BASE_HI_OFF) << 32) |
                        REG(s, ENA_REGS_ACQ_BASE_LO_OFF);
    uint16_t aq_depth = REG(s, ENA_REGS_AQ_CAPS_OFF) & ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK;
    uint16_t acq_depth = REG(s, ENA_REGS_ACQ_CAPS_OFF) & ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK;
    uint16_t tail = REG(s, ENA_REGS_AQ_DB_OFF);
    bool completed = false;

    if (!aq_base || !acq_base || !aq_depth || !acq_depth) {
        return;
    }
    if ((uint16_t)(tail - s->aq_head) > aq_depth) {
        qemu_log_mask(LOG_GUEST_ERROR, "ena: aq doorbell %u past depth %u\n",
                      tail, aq_depth);
        return;
    }

    while (s->aq_head != tail) {
        struct ena_admin_aq_entry cmd;
        struct ena_admin_acq_entry resp = {};
        int status;

        ena_dma_read(s, aq_base + (uint64_t)(s->aq_head % aq_depth) * sizeof(cmd),
                     &cmd, sizeof(cmd));
        s->aq_head++;

        status = ena_admin_exec(s, &cmd, &resp);
        if (status != ENA_ADMIN_SUCCESS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ena: admin opcode %u (feature %u) failed: status %d\n",
                          cmd.aq_common_descriptor.opcode,
                          ((struct ena_admin_get_feat_cmd *)&cmd)->feat_common.feature_id,
                          status);
        }
        resp.acq_common_descriptor.command =
            cmd.aq_common_descriptor.command_id &
            cpu_to_le16(ENA_ADMIN_ACQ_COMMON_DESC_COMMAND_ID_MASK);
        resp.acq_common_descriptor.status = status;
        resp.acq_common_descriptor.flags = s->acq_phase;
        resp.acq_common_descriptor.sq_head_indx = cpu_to_le16(s->aq_head);

        ena_dma_write(s, acq_base + (uint64_t)(s->acq_tail % acq_depth) * sizeof(resp),
                      &resp, sizeof(resp));
        s->acq_tail++;
        if (s->acq_tail % acq_depth == 0) {
            s->acq_phase = !s->acq_phase;
        }
        completed = true;
    }

    REG(s, ENA_REGS_ACQ_TAIL_OFF) = s->acq_tail;
    if (completed && !(REG(s, ENA_REGS_INTR_MASK_OFF) & 1)) {
        msix_notify(PCI_DEVICE(s), ENA_ADMIN_MSIX_VECTOR);
    }
}
