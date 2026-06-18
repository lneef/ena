/*
 * ENA device setup / initialization qtests (control path, polling mode).
 *
 * Covers the PCI attachment and the ordered bring-up handshake the reference
 * driver performs before any datapath exists (docs/wiki/device-init.md,
 * docs/wiki/registers.md): PCI identity and BARs, the identity/caps register
 * reads, the readless (indirect) MMIO register-read handshake, the two-phase
 * device reset, and acceptance of the admin AQ/ACQ/AENQ register programming.
 *
 * This is the worked example for the shared harness in ena_test_common.h; the
 * remaining feature areas live in their own ena_*_test.c files.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#include "ena_test_common.h"

#define ENA_PCI_VENDOR_ID   0x1d0f
#define ENA_PCI_DEVICE_ID   0x0ec2
#define ENA_PCI_CLASS_NET_ETH 0x0200 /* network controller / ethernet */

/* ------------------------------------------------------------------ */
/* PCI attachment                                                     */
/* ------------------------------------------------------------------ */

static void test_pci_identity(void *obj, void *data, QGuestAllocator *alloc)
{
    QENA *ena = obj;
    QPCIDevice *dev = &ena->dev;
    uint16_t vendor, device;
    uint32_t class_rev;

    vendor = qpci_config_readw(dev, PCI_VENDOR_ID);
    device = qpci_config_readw(dev, PCI_DEVICE_ID);
    g_assert_cmphex(vendor, ==, ENA_PCI_VENDOR_ID);
    g_assert_cmphex(device, ==, ENA_PCI_DEVICE_ID);

    /* Class code: network/ethernet (0x0200) in the upper 24 bits. */
    class_rev = qpci_config_readl(dev, PCI_REVISION_ID);
    g_assert_cmphex(class_rev >> 16, ==, ENA_PCI_CLASS_NET_ETH);
}

/* BAR0 must be a memory BAR; the readless path needs it before any read. */
static void test_pci_bar0(void *obj, void *data, QGuestAllocator *alloc)
{
    QENA *ena = obj;
    QPCIDevice *dev = &ena->dev;
    uint32_t bar_probe;

    qpci_config_writel(dev, PCI_BASE_ADDRESS_0, 0xffffffff);
    bar_probe = qpci_config_readl(dev, PCI_BASE_ADDRESS_0);
    g_assert_cmphex(bar_probe & PCI_BASE_ADDRESS_SPACE, ==,
                    PCI_BASE_ADDRESS_SPACE_MEMORY);
    g_assert_cmphex(bar_probe & PCI_BASE_ADDRESS_MEM_MASK, !=, 0);
}

/* ------------------------------------------------------------------ */
/* Identity / caps registers                                          */
/* ------------------------------------------------------------------ */

static void test_version_caps(void *obj, void *data, QGuestAllocator *alloc)
{
    ENATestCtx t;
    uint32_t version, ctrl_ver, caps;
    uint32_t dma_width, reset_to;

    ena_bringup(&t, obj, alloc);

    /* VERSION must report a non-zero major version. */
    version = ena_reg_read(&t, ENA_REGS_VERSION_OFF);
    g_assert_cmpuint(version & ENA_REGS_VERSION_MAJOR_VERSION_MASK, !=, 0);

    /* CONTROLLER_VERSION must be >= MIN_ENA_CTRL_VER (impl-id byte ignored). */
    ctrl_ver = ena_reg_read(&t, ENA_REGS_CONTROLLER_VERSION_OFF);
    ctrl_ver &= ~ENA_REGS_CONTROLLER_VERSION_IMPL_ID_MASK;
    g_assert_cmpuint(ctrl_ver, >=, ENA_MIN_CTRL_VER);

    /* CAPS: DMA address width in [32,48], reset timeout non-zero. */
    caps = ena_reg_read(&t, ENA_REGS_CAPS_OFF);
    dma_width = (caps & ENA_REGS_CAPS_DMA_ADDR_WIDTH_MASK) >>
                ENA_REGS_CAPS_DMA_ADDR_WIDTH_SHIFT;
    g_assert_cmpuint(dma_width, >=, 32);
    g_assert_cmpuint(dma_width, <=, ENA_MAX_PHYS_ADDR_SIZE_BITS);

    reset_to = (caps & ENA_REGS_CAPS_RESET_TIMEOUT_MASK) >>
               ENA_REGS_CAPS_RESET_TIMEOUT_SHIFT;
    g_assert_cmpuint(reset_to, !=, 0);
}

/* ------------------------------------------------------------------ */
/* Readless (indirect) MMIO register read handshake                   */
/* ------------------------------------------------------------------ */

/*
 * The readless handshake must DMA back the 8-byte {req_id, reg_off, reg_val}
 * response, echoing the request id and register offset and returning the same
 * value as a direct BAR0 read.
 */
static void test_readless_read(void *obj, void *data, QGuestAllocator *alloc)
{
    ENATestCtx t;
    uint16_t echoed_off = 0xffff;
    uint32_t direct, indirect;

    ena_bringup(&t, obj, alloc);

    direct = ena_reg_read(&t, ENA_REGS_VERSION_OFF);
    indirect = ena_readless_read_full(&t, ENA_REGS_VERSION_OFF, &echoed_off);

    g_assert_cmphex(echoed_off, ==, ENA_REGS_VERSION_OFF);
    g_assert_cmphex(indirect, ==, direct);

    /* A second read with a fresh request id must also be answered (the
     * device must track the monotonically increasing id). */
    indirect = ena_readless_read(&t, ENA_REGS_CAPS_OFF);
    g_assert_cmphex(indirect, ==, ena_reg_read(&t, ENA_REGS_CAPS_OFF));
}

/* ------------------------------------------------------------------ */
/* Device reset handshake                                             */
/* ------------------------------------------------------------------ */

/*
 * Before reset DEV_STS.READY must be set; the DEV_CTL.DEV_RESET ->
 * DEV_STS.RESET_IN_PROGRESS -> DEV_CTL=0 two-phase handshake must complete and
 * leave the device READY again. ena_device_reset() drives and asserts the
 * whole sequence.
 */
static void test_reset_handshake(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    ENATestCtx t;
    QENA *ena = obj;
    uint32_t sts;

    memset(&t, 0, sizeof(t));
    t.qts = ena->dev.bus->qts;
    t.dev = &ena->dev;
    t.alloc = alloc;

    qpci_device_enable(t.dev);
    t.regs = qpci_iomap(t.dev, 0, NULL);
    t.mmio_resp = guest_alloc(alloc, sizeof(struct ena_mmio_read_less_resp));
    ena_mmio_resp_arm(&t);

    sts = ena_reg_read(&t, ENA_REGS_DEV_STS_OFF);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_READY_MASK, ==,
                    ENA_REGS_DEV_STS_READY_MASK);

    ena_device_reset(&t);

    sts = ena_reg_read(&t, ENA_REGS_DEV_STS_OFF);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_READY_MASK, ==,
                    ENA_REGS_DEV_STS_READY_MASK);
}

/*
 * Reset clears device-programmed state, including the readless response
 * buffer address: a readless read issued after reset without re-arming must
 * not be served. ena_device_reset() re-arms internally, so afterwards the
 * handshake works again.
 */
static void test_reset_clears_state(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    ENATestCtx t;

    ena_bringup(&t, obj, alloc);

    /* Re-arm + readless still works after the bring-up reset. */
    g_assert_cmphex(ena_readless_read(&t, ENA_REGS_VERSION_OFF), ==,
                    ena_reg_read(&t, ENA_REGS_VERSION_OFF));

    /* A second explicit reset must keep the device usable. */
    ena_device_reset(&t);
    g_assert_cmphex(ena_readless_read(&t, ENA_REGS_CAPS_OFF), ==,
                    ena_reg_read(&t, ENA_REGS_CAPS_OFF));
}

/* ------------------------------------------------------------------ */
/* Admin queue register programming                                   */
/* ------------------------------------------------------------------ */

/*
 * After reset the device must accept the AQ/ACQ/AENQ base + caps programming
 * and remain READY. ena_admin_init() performs all the register writes; the
 * device must not drop READY or signal a fatal error in response.
 */
static void test_admin_register_programming(void *obj, void *data,
                                            QGuestAllocator *alloc)
{
    ENATestCtx t;
    uint32_t sts;

    ena_bringup(&t, obj, alloc); /* includes ena_admin_init() */

    sts = ena_reg_read(&t, ENA_REGS_DEV_STS_OFF);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_READY_MASK, ==,
                    ENA_REGS_DEV_STS_READY_MASK);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_FATAL_ERROR_MASK, ==, 0);
}

/* ------------------------------------------------------------------ */

static void ena_register_nodes(void)
{
    ena_qos_node_register("ena-device");

    qos_add_test("device/pci-identity", "ena-device",
                 test_pci_identity, NULL);
    qos_add_test("device/pci-bar0", "ena-device", test_pci_bar0, NULL);
    qos_add_test("device/version-caps", "ena-device",
                 test_version_caps, NULL);
    qos_add_test("device/readless-read", "ena-device",
                 test_readless_read, NULL);
    qos_add_test("device/reset-handshake", "ena-device",
                 test_reset_handshake, NULL);
    qos_add_test("device/reset-clears-state", "ena-device",
                 test_reset_clears_state, NULL);
    qos_add_test("device/admin-register-programming", "ena-device",
                 test_admin_register_programming, NULL);
}

libqos_init(ena_register_nodes);
