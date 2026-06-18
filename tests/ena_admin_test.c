/*
 * ENA admin (control) queue qtests — submission/completion semantics
 * (control path, polling mode).
 *
 * Covers the AQ/ACQ command channel the reference driver drives once the
 * admin rings are programmed (docs/wiki/admin-queue.md): the phase-bit
 * submit/complete handshake, command_id echo, sequential commands and ring
 * wrap (phase flip), GET_FEATURE / SET_FEATURE round-trips, and the error
 * status returned for an unknown opcode. Device attach/setup and AQ register
 * programming are covered by ena_device_test.c; AENQ behaviour by
 * ena_aenq_test.c.
 *
 * All commands are completed by polling the ACQ phase bit — no MSI-X is
 * configured (the admin interrupt is masked in ena_admin_init()).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#include "ena_test_common.h"

/* ------------------------------------------------------------------ */
/* Feature response framing (admin-queue.md §6)                        */
/* ------------------------------------------------------------------ */

/* GET_FEATURE(DEVICE_ATTRIBUTES) response: acq_common_desc (8 bytes) then
 * struct ena_admin_device_attr_feature_desc overlaid on the response area. */
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

/* SET_FEATURE(MTU) payload: struct ena_admin_set_feature_mtu_desc. */
typedef struct QEMU_PACKED ENASetMtuPayload {
    uint32_t mtu;
} ENASetMtuPayload;

/* An opcode value outside enum ena_admin_aq_opcode (1,2,3,4,8,9,11). */
#define ENA_ADMIN_OPCODE_INVALID    0x3f

/* ------------------------------------------------------------------ */
/* Single command: command_id echo + SUCCESS                           */
/* ------------------------------------------------------------------ */

/*
 * A GET_FEATURE(DEVICE_ATTRIBUTES) must complete SUCCESSfully with the
 * completion echoing the submitted command_id (asserted inside ena_admin_try)
 * and carrying plausible device attributes: a non-zero MAC, a DMA address
 * width in [32,48] and a non-zero max MTU.
 */
static void test_get_device_attributes(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENADeviceAttrResp resp;
    uint8_t status;
    bool mac_nonzero;
    int i;

    ena_bringup(&t, obj, alloc);

    memset(&resp, 0, sizeof(resp));
    status = ena_get_feature(&t, ENA_ADMIN_DEVICE_ATTRIBUTES, 0,
                             &resp, sizeof(resp));
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);

    mac_nonzero = false;
    for (i = 0; i < 6; i++) {
        mac_nonzero |= resp.mac_addr[i] != 0;
    }
    g_assert_true(mac_nonzero);

    g_assert_cmpuint(le32_to_cpu(resp.phys_addr_width), >=, 32);
    g_assert_cmpuint(le32_to_cpu(resp.phys_addr_width), <=,
                     ENA_MAX_PHYS_ADDR_SIZE_BITS);
    g_assert_cmpuint(le32_to_cpu(resp.max_mtu), !=, 0);

    /* DEVICE_ATTRIBUTES is always supported, so its own bit must be set in
     * the advertised supported_features bitmap (admin-queue.md §6). */
    g_assert_cmpuint(le32_to_cpu(resp.supported_features) &
                     (1u << ENA_ADMIN_DEVICE_ATTRIBUTES), !=, 0);
}

/* ------------------------------------------------------------------ */
/* Sequential commands: command_id / ACQ phase tracking                */
/* ------------------------------------------------------------------ */

/*
 * Issuing several admin commands back-to-back: each must complete in order
 * with its own command_id echoed. ena_admin_try advances the host-side
 * command_id and ACQ consumer/phase and asserts the echo, so a clean run of
 * the loop proves the device keeps ACQ ordering and phase across commands.
 */
static void test_sequential_commands(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENADeviceAttrResp resp;
    int i;

    ena_bringup(&t, obj, alloc);

    for (i = 0; i < 8; i++) {
        uint8_t status = ena_get_feature(&t, ENA_ADMIN_DEVICE_ATTRIBUTES, 0,
                                         &resp, sizeof(resp));
        g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    }

    /* 8 commands < depth (32): no wrap yet, phase unchanged. */
    g_assert_cmpuint(t.aq_tail, ==, 8);
    g_assert_cmpuint(t.acq_phase, ==, 1);
}

/*
 * Submit more than q_depth commands so both the AQ and the ACQ wrap. The
 * device must flip the per-entry phase bit on the wrap; ena_admin_poll only
 * accepts a completion whose phase matches the host's toggled expectation, so
 * completing every command past the wrap proves the device handles the phase
 * flip correctly.
 */
static void test_ring_wrap_phase_flip(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENADeviceAttrResp resp;
    int i;
    const int n = ENA_ADMIN_QUEUE_DEPTH + 8; /* force one full wrap */

    ena_bringup(&t, obj, alloc);

    for (i = 0; i < n; i++) {
        uint8_t status = ena_get_feature(&t, ENA_ADMIN_DEVICE_ATTRIBUTES, 0,
                                         &resp, sizeof(resp));
        g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
    }

    /* After one wrap the host-tracked phases have toggled back-and-forth: a
     * single wrap of the 32-deep ring leaves both phases at 0. */
    g_assert_cmpuint(t.aq_tail, ==, n);
    g_assert_cmpuint(t.aq_phase, ==, 0);
    g_assert_cmpuint(t.acq_phase, ==, 0);
}

/* ------------------------------------------------------------------ */
/* SET_FEATURE round-trip                                              */
/* ------------------------------------------------------------------ */

/* SET_FEATURE(MTU) with a valid MTU must complete SUCCESSfully. */
static void test_set_feature_mtu(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENASetMtuPayload payload = { .mtu = cpu_to_le32(1500) };
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    status = ena_set_feature(&t, ENA_ADMIN_MTU, 0,
                             &payload, sizeof(payload), NULL, 0);
    g_assert_cmpuint(status, ==, ENA_ADMIN_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Error path: unknown opcode                                          */
/* ------------------------------------------------------------------ */

/*
 * A command with an opcode the device does not implement must still be
 * completed (command_id echoed, phase bit set) but with a non-success status
 * — BAD_OPCODE or UNSUPPORTED_OPCODE (admin-queue.md §4). The device must not
 * silently drop the command, which would hang a real driver.
 */
static void test_unknown_opcode(void *obj, void *data,
                                QGuestAllocator *alloc)
{
    ENATestCtx t;
    ENAGetFeatCmd cmd;
    uint8_t status;

    ena_bringup(&t, obj, alloc);

    memset(&cmd, 0, sizeof(cmd));
    cmd.common.opcode = ENA_ADMIN_OPCODE_INVALID;

    /* ena_admin_try asserts the completion echoes the command_id. */
    status = ena_admin_try(&t, &cmd, sizeof(cmd), NULL, 0);

    g_assert_cmpuint(status, !=, ENA_ADMIN_SUCCESS);
    g_assert_true(status == ENA_ADMIN_BAD_OPCODE ||
                  status == ENA_ADMIN_UNSUPPORTED_OPCODE);
}

/* ------------------------------------------------------------------ */
/* sq_head_indx accounting                                             */
/* ------------------------------------------------------------------ */

/*
 * Each completion reports sq_head_indx — the AQ entry the device has consumed
 * (admin-queue.md §2, §8). After N sequential commands it must reflect that
 * the device has consumed all N submitted entries, i.e. equal the AQ tail
 * (modulo nothing: it is the absolute consumed count, which here has not
 * wrapped).
 */
static void test_sq_head_advances(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    ENATestCtx t;
    struct ena_admin_acq_common_desc resp;
    uint16_t sq_head = 0;
    int i;

    ena_bringup(&t, obj, alloc);

    for (i = 0; i < 4; i++) {
        ENAGetFeatCmd cmd;

        memset(&cmd, 0, sizeof(cmd));
        cmd.common.opcode = ENA_ADMIN_GET_FEATURE;
        cmd.feat_common.flags = ENA_ADMIN_GET_FEATURE_SELECT_CURRENT;
        cmd.feat_common.feature_id = ENA_ADMIN_DEVICE_ATTRIBUTES;

        memset(&resp, 0, sizeof(resp));
        g_assert_cmpuint(ena_admin_try(&t, &cmd, sizeof(cmd),
                                       &resp, sizeof(resp)), ==,
                         ENA_ADMIN_SUCCESS);
        sq_head = le16_to_cpu(resp.sq_head_indx);
    }

    /* The device has consumed all 4 entries posted to the AQ. */
    g_assert_cmpuint(sq_head, ==, t.aq_tail);
}

/* ------------------------------------------------------------------ */

static void ena_register_nodes(void)
{
    ena_qos_node_register("ena-admin");

    qos_add_test("admin/get-device-attributes", "ena-admin",
                 test_get_device_attributes, NULL);
    qos_add_test("admin/sequential-commands", "ena-admin",
                 test_sequential_commands, NULL);
    qos_add_test("admin/ring-wrap-phase-flip", "ena-admin",
                 test_ring_wrap_phase_flip, NULL);
    qos_add_test("admin/set-feature-mtu", "ena-admin",
                 test_set_feature_mtu, NULL);
    qos_add_test("admin/unknown-opcode", "ena-admin",
                 test_unknown_opcode, NULL);
    qos_add_test("admin/sq-head-advances", "ena-admin",
                 test_sq_head_advances, NULL);
}

libqos_init(ena_register_nodes);
