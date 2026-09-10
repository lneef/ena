/*
 * QTest testcase for the ENA NIC: PCI identity, register file, reset,
 * readless MMIO.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "standard-headers/linux/pci_regs.h"
#include "tests/ena_qos.h"

#define MIN_CTRL_VER 0x000001

static void test_pci_identity(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    uint32_t class_rev = qpci_config_readl(&d->dev, PCI_CLASS_REVISION);

    g_assert_cmphex(qpci_config_readw(&d->dev, PCI_VENDOR_ID), ==,
                    ENA_TEST_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(&d->dev, PCI_DEVICE_ID), ==,
                    ENA_TEST_DEVICE_ID);
    /* Ethernet controller, programming interface bit 0 clear: readless on */
    g_assert_cmphex(class_rev >> 16, ==, 0x0200);
    g_assert_cmphex((class_rev >> 8) & 1, ==, 0);
    g_assert_cmpuint(qpci_msix_table_size(&d->dev), ==, ENA_TEST_MSIX_VECTORS);
    g_assert_false(d->regs.is_io);
    g_assert_false(d->mem.is_io);
}

static void test_registers(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    uint32_t ver = ena_reg_read(d, ENA_REGS_VERSION_OFF);
    uint32_t ctrl = ena_reg_read(d, ENA_REGS_CONTROLLER_VERSION_OFF);
    uint32_t caps = ena_reg_read(d, ENA_REGS_CAPS_OFF);

    g_assert_cmpuint((ver & ENA_REGS_VERSION_MAJOR_VERSION_MASK) >>
                     ENA_REGS_VERSION_MAJOR_VERSION_SHIFT, >=, 2);
    g_assert_cmpuint(ctrl & ~ENA_REGS_CONTROLLER_VERSION_IMPL_ID_MASK, >=,
                     MIN_CTRL_VER);
    g_assert_cmpuint((caps & ENA_REGS_CAPS_DMA_ADDR_WIDTH_MASK) >>
                     ENA_REGS_CAPS_DMA_ADDR_WIDTH_SHIFT, ==, 48);
    g_assert_cmpuint((caps & ENA_REGS_CAPS_RESET_TIMEOUT_MASK) >>
                     ENA_REGS_CAPS_RESET_TIMEOUT_SHIFT, !=, 0);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_DEV_STS_OFF) &
                    ENA_REGS_DEV_STS_READY_MASK, ==, ENA_REGS_DEV_STS_READY_MASK);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_ACQ_TAIL_OFF), ==, 0);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_AENQ_TAIL_OFF), ==, 0);

    /* read-only registers ignore writes */
    ena_reg_write(d, ENA_REGS_VERSION_OFF, 0xdeadbeef);
    ena_reg_write(d, ENA_REGS_CAPS_OFF, 0xdeadbeef);
    ena_reg_write(d, ENA_REGS_DEV_STS_OFF, 0);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_VERSION_OFF), ==, ver);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_CAPS_OFF), ==, caps);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_DEV_STS_OFF), ==,
                    ENA_REGS_DEV_STS_READY_MASK);

    /* base address registers latch their value */
    ena_reg_write(d, ENA_REGS_AQ_BASE_LO_OFF, 0x12345000);
    ena_reg_write(d, ENA_REGS_AQ_BASE_HI_OFF, 0x1);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_AQ_BASE_LO_OFF), ==, 0x12345000);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_AQ_BASE_HI_OFF), ==, 0x1);
}

static void test_readless(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_ena_mmio_req_read_less_resp resp;

    ena_mmio_resp_setup(d);
    g_assert_cmphex(ena_readless(d, ENA_REGS_VERSION_OFF), ==,
                    ena_reg_read(d, ENA_REGS_VERSION_OFF));
    g_assert_cmphex(ena_readless(d, ENA_REGS_CAPS_OFF), ==,
                    ena_reg_read(d, ENA_REGS_CAPS_OFF));
    g_assert_cmphex(ena_readless(d, ENA_REGS_DEV_STS_OFF), ==,
                    ENA_REGS_DEV_STS_READY_MASK);

    /* the request register itself is readable and echoes the request */
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_MMIO_REG_READ_OFF) >>
                    ENA_REGS_MMIO_REG_READ_REG_OFF_SHIFT, ==, ENA_REGS_DEV_STS_OFF);

    /* a cleared response address disables readless: no DMA happens */
    ena_reg_write(d, ENA_REGS_MMIO_RESP_LO_OFF, 0);
    ena_reg_write(d, ENA_REGS_MMIO_RESP_HI_OFF, 0);
    resp.req_id = 0xffff;
    qtest_memwrite(d->dev.bus->qts, d->mmio_resp, &resp, sizeof(resp));
    ena_reg_write(d, ENA_REGS_MMIO_REG_READ_OFF,
                  (ENA_REGS_CAPS_OFF << ENA_REGS_MMIO_REG_READ_REG_OFF_SHIFT) | 7);
    qtest_memread(d->dev.bus->qts, d->mmio_resp, &resp, sizeof(resp));
    g_assert_cmphex(resp.req_id, ==, 0xffff);
}

static void test_reset(void *obj, void *data, QGuestAllocator *alloc)
{
    QEna *d = obj;

    ena_mmio_resp_setup(d);
    ena_reg_write(d, ENA_REGS_AQ_BASE_LO_OFF, 0x1000);
    ena_reg_write(d, ENA_REGS_INTR_MASK_OFF, 1);

    /* reset request: status shows in-progress and ready is dropped */
    ena_reg_write(d, ENA_REGS_DEV_CTL_OFF, ENA_REGS_DEV_CTL_DEV_RESET_MASK);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_DEV_STS_OFF), ==,
                    ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_DEV_CTL_OFF) &
                    ENA_REGS_DEV_CTL_DEV_RESET_MASK, ==,
                    ENA_REGS_DEV_CTL_DEV_RESET_MASK);
    /* reset wipes the register file, including the readless response address */
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_AQ_BASE_LO_OFF), ==, 0);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_INTR_MASK_OFF), ==, 0);
    g_assert_cmphex(ena_reg_read(d, ENA_REGS_MMIO_RESP_LO_OFF), ==, 0);

    /* driver re-arms readless and completes the handshake */
    ena_mmio_resp_setup(d);
    g_assert_cmphex(ena_readless(d, ENA_REGS_DEV_STS_OFF), ==,
                    ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK);
    ena_reg_write(d, ENA_REGS_DEV_CTL_OFF, 0);
    g_assert_cmphex(ena_readless(d, ENA_REGS_DEV_STS_OFF), ==,
                    ENA_REGS_DEV_STS_READY_MASK);

    /* the full driver-style handshake works repeatedly */
    ena_dev_reset(d);
    ena_dev_reset(d);
}

static void test_reset_clears_admin(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QEna *d = obj;
    struct ena_admin_get_feat_resp resp;

    ena_bringup(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_DEVICE_ATTRIBUTES, 0, 0, 0,
                                    &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_ACQ_TAIL_OFF), ==, 1);

    ena_dev_reset(d);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_ACQ_TAIL_OFF), ==, 0);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_AQ_CAPS_OFF), ==, 0);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_AENQ_CAPS_OFF), ==, 0);

    /* admin queue comes back from scratch: first completion carries id 0 */
    ena_admin_init(d);
    g_assert_cmpint(ena_get_feature(d, ENA_ADMIN_DEVICE_ATTRIBUTES, 0, 0, 0,
                                    &resp), ==, ENA_ADMIN_SUCCESS);
    g_assert_cmpuint(ena_reg_read(d, ENA_REGS_ACQ_TAIL_OFF), ==, 1);
}

static void register_ena_device_test(void)
{
    QOSGraphTestOptions opts = {
        .before = ena_test_before,
    };

    qos_add_test("device/pci-identity", "ena", test_pci_identity, &opts);
    qos_add_test("device/registers", "ena", test_registers, &opts);
    qos_add_test("device/readless", "ena", test_readless, &opts);
    qos_add_test("device/reset", "ena", test_reset, &opts);
    qos_add_test("device/reset-clears-admin", "ena", test_reset_clears_admin,
                 &opts);
}

libqos_init(register_ena_device_test);
