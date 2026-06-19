/*
 * ENA stateless-offload advertisement qtests (control path, polling mode).
 *
 * The emulated device advertises NO stateless offloads — no TX/RX checksum, no
 * TSO, no RX hash/RSS. RX scatter (multi-buffer Rx) is the only "offload" the
 * driver ends up with, and it is inherent to the datapath, never advertised via
 * an admin feature (docs/wiki/offloads.md §1, §6). These tests lock that down:
 *
 *  - GET_FEATURE(STATELESS_OFFLOAD_CONFIG) is answered (not UNSUPPORTED) with an
 *    all-zero ena_admin_feature_offload_desc {tx, rx_supported, rx_enabled}.
 *  - The RSS/hash features (RSS_HASH_FUNCTION, RSS_INDIRECTION_TABLE_CONFIG,
 *    RSS_HASH_INPUT) are NOT advertised — GET_FEATURE of each is rejected.
 *  - DEVICE_ATTRIBUTES carries capabilities == 0 and no RSS/offload feature bits
 *    in supported_features (other than the zero-offload STATELESS_OFFLOAD_CONFIG).
 *  - STATELESS_OFFLOAD_CONFIG IS the only offload-related supported_features bit:
 *    it must be set (the driver's init GET(11) is gated on it) yet carries the
 *    all-zero descriptor above — the exact "feature present, no offloads" boundary.
 *  - GET_FEATURE(LLQ) advertises no acceleration: accel_mode == 0, i.e. no TX
 *    meta-caching / burst-limit offload (offloads.md §1, §3).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#include "ena_test_common.h"

/* ------------------------------------------------------------------ */
/* Feature response framing (offloads.md §1, admin-queue.md §6)        */
/* ------------------------------------------------------------------ */

/* GET_FEATURE(STATELESS_OFFLOAD_CONFIG) response: acq_common_desc then
 * struct ena_admin_feature_offload_desc (3 x u32). */
typedef struct QEMU_PACKED ENAOffloadResp {
    struct ena_admin_acq_common_desc acq_common;
    uint32_t tx;            /* TX-side supported offloads (csum/TSO bits) */
    uint32_t rx_supported;  /* RX-side supported offloads (csum + RX_HASH) */
    uint32_t rx_enabled;    /* RX-side currently-enabled offloads */
} ENAOffloadResp;

/* GET_FEATURE(LLQ) response, trimmed to the accel_mode tail. accel_mode lives at
 * offset 28 of struct ena_admin_feature_llq_desc (offloads.md §3, llq.md §2) and
 * carries the TX acceleration flags (disable_meta_caching, limit_tx_burst); the
 * fields before it (counts / ctrl bitmaps) are exercised by ena_llq_test.c. */
typedef struct QEMU_PACKED ENALlqAccelResp {
    struct ena_admin_acq_common_desc acq_common;
    uint8_t  before_accel[28];   /* max_llq_num .. max_wide_llq_depth */
    uint32_t accel_mode[2];       /* offset 28: TX acceleration flags */
} ENALlqAccelResp;

/* GET_FEATURE(DEVICE_ATTRIBUTES) response, trimmed to the fields under test. */
typedef struct QEMU_PACKED ENADeviceAttrResp {
    struct ena_admin_acq_common_desc acq_common;
    uint32_t impl_id;
    uint32_t device_version;
    uint32_t supported_features;   /* bitmap of ena_admin_aq_feature_id */
    uint32_t capabilities;         /* bitmap of ena_admin_aq_caps_id */
    uint32_t phys_addr_width;
    uint32_t virt_addr_width;
    uint8_t  mac_addr[6];
    uint8_t  reserved7[2];
    uint32_t max_mtu;
} ENADeviceAttrResp;

/* ------------------------------------------------------------------ */
/* No stateless offloads advertised                                    */
/* ------------------------------------------------------------------ */

/*
 * GET_FEATURE(STATELESS_OFFLOAD_CONFIG) must SUCCEED (the driver's init GET is
 * mandatory; an UNSUPPORTED status is fatal) and report a fully zero offload
 * descriptor: no TX/RX checksum, no TSO, no RX hash.
 */
static void test_offloads_stateless_none(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAOffloadResp resp;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    memset(&resp, 0, sizeof(resp));
    status = ena_get_feature(&t, ENA_ADMIN_STATELESS_OFFLOAD_CONFIG, 0,
                             &resp, sizeof(resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    g_assert_cmphex(le32_to_cpu(resp.tx), ==, 0);
    g_assert_cmphex(le32_to_cpu(resp.rx_supported), ==, 0);
    g_assert_cmphex(le32_to_cpu(resp.rx_enabled), ==, 0);
}

/*
 * The RSS / hash features must NOT be advertised: GET_FEATURE of each is
 * rejected (the device only advertises the zero-offload STATELESS_OFFLOAD_CONFIG
 * among the offload-ish features). Guards against an RX-hash/RSS offload being
 * introduced.
 */
static void test_offloads_no_rss(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    static const uint8_t rss_features[] = {
        ENA_ADMIN_RSS_HASH_FUNCTION,
        ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG,
        ENA_ADMIN_RSS_HASH_INPUT,
    };
    ENATestCtx t;
    int i;

    ena_bringup(&t, obj, alloc);

    for (i = 0; i < ARRAY_SIZE(rss_features); i++) {
        uint8_t status = ena_get_feature(&t, rss_features[i], 0, NULL, 0);

        g_assert_cmpuint(status, !=, ENA_ADMIN_SUCCESS);
    }
}

/*
 * DEVICE_ATTRIBUTES must carry no offload-ish capabilities: capabilities == 0
 * (that bitmap is stats/info, never an offload) and supported_features must
 * expose none of the RSS/hash/interrupt-moderation feature bits. The only
 * offload-related bit allowed is STATELESS_OFFLOAD_CONFIG, which advertises the
 * all-zero (no-offload) descriptor checked above.
 */
static void test_offloads_device_attr_clean(void *obj, void *data,
                                            QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENADeviceAttrResp resp;
    uint32_t supported;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    memset(&resp, 0, sizeof(resp));
    status = ena_get_feature(&t, ENA_ADMIN_DEVICE_ATTRIBUTES, 0,
                             &resp, sizeof(resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    g_assert_cmphex(le32_to_cpu(resp.capabilities), ==, 0);

    supported = le32_to_cpu(resp.supported_features);
    g_assert_cmphex(supported & (1u << ENA_ADMIN_RSS_HASH_FUNCTION), ==, 0);
    g_assert_cmphex(supported & (1u << ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG),
                    ==, 0);
    g_assert_cmphex(supported & (1u << ENA_ADMIN_RSS_HASH_INPUT), ==, 0);
    g_assert_cmphex(supported & (1u << ENA_ADMIN_INTERRUPT_MODERATION), ==, 0);
}

/*
 * The lone offload-related feature the device advertises is
 * STATELESS_OFFLOAD_CONFIG, and it carries no offloads. The driver's init GET(11)
 * is gated on this supported_features bit (an absent bit makes the mandatory GET
 * return UNSUPPORTED, which aborts probe — offloads.md §1), so the bit MUST be
 * set; test_offloads_stateless_none above proves the descriptor it answers with
 * is all-zero. Together they pin the "feature present, no offloads" boundary.
 */
static void test_offloads_stateless_feature_present(void *obj, void *data,
                                                    QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENADeviceAttrResp resp;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    memset(&resp, 0, sizeof(resp));
    status = ena_get_feature(&t, ENA_ADMIN_DEVICE_ATTRIBUTES, 0,
                             &resp, sizeof(resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    g_assert_cmphex(le32_to_cpu(resp.supported_features) &
                    (1u << ENA_ADMIN_STATELESS_OFFLOAD_CONFIG), !=, 0);
}

/*
 * GET_FEATURE(LLQ) must advertise no acceleration: the accel_mode field (offset
 * 28 of feature_llq_desc) holds the TX acceleration flags — disable_meta_caching
 * and limit_tx_burst. A zero-offload device advertises none, so both accel_mode
 * words read back 0 (offloads.md §1 "leave LLQ accel_mode = 0", §3). Guards
 * against a TX meta-caching/burst acceleration offload being introduced via LLQ.
 */
static void test_offloads_llq_no_accel(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENALlqAccelResp resp;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    memset(&resp, 0, sizeof(resp));
    status = ena_get_feature(&t, ENA_ADMIN_LLQ, 0, &resp, sizeof(resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    g_assert_cmphex(le32_to_cpu(resp.accel_mode[0]), ==, 0);
    g_assert_cmphex(le32_to_cpu(resp.accel_mode[1]), ==, 0);
}

/* ------------------------------------------------------------------ */

static void ena_register_nodes(void)
{
    ena_qos_node_register("ena-offloads");

    qos_add_test("offloads/stateless-none", "ena-offloads",
                 test_offloads_stateless_none, NULL);
    qos_add_test("offloads/no-rss", "ena-offloads",
                 test_offloads_no_rss, NULL);
    qos_add_test("offloads/device-attr-clean", "ena-offloads",
                 test_offloads_device_attr_clean, NULL);
    qos_add_test("offloads/stateless-feature-present", "ena-offloads",
                 test_offloads_stateless_feature_present, NULL);
    qos_add_test("offloads/llq-no-accel", "ena-offloads",
                 test_offloads_llq_no_accel, NULL);
}

libqos_init(ena_register_nodes);
