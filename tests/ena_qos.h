/*
 * libqos driver and helpers for the ENA qtests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TESTS_ENA_QOS_H
#define TESTS_ENA_QOS_H

#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "libqos/libqos-malloc.h"
#include "qemu/bswap.h"
#include "hw/ena_defs/ena_defs.h"

#define ENA_TEST_VENDOR_ID      0x1d0f
#define ENA_TEST_DEVICE_ID      0xec20
#define ENA_TEST_REG_BAR        0
#define ENA_TEST_MEM_BAR        2
#define ENA_TEST_AQ_DEPTH       32
#define ENA_TEST_AENQ_DEPTH     16
#define ENA_TEST_ADMIN_VECTOR   0
#define ENA_TEST_MSIX_VECTORS   9
#define ENA_TEST_MSIX_DATA      0x4e41
#define ENA_TEST_MAC            { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 }

typedef struct QEna {
    QOSGraphObject obj;
    QPCIDevice dev;
    QPCIBar regs;
    QPCIBar mem;
    QGuestAllocator *alloc;

    uint64_t mmio_resp;
    uint16_t mmio_seq;

    uint64_t aq;
    uint64_t acq;
    uint64_t aenq;
    uint16_t aq_tail;
    uint16_t acq_head;
    bool acq_phase;
    uint16_t aenq_head;
    bool aenq_phase;

    uint64_t msix_addr[ENA_TEST_MSIX_VECTORS];
} QEna;

/* register access */
uint32_t ena_reg_read(QEna *d, uint32_t off);
void ena_reg_write(QEna *d, uint32_t off, uint32_t val);
uint32_t ena_readless(QEna *d, uint16_t off);
void ena_mmio_resp_setup(QEna *d);

/* control path */
void ena_dev_reset(QEna *d);
void ena_admin_init(QEna *d);
int ena_admin_cmd(QEna *d, void *cmd, size_t cmd_len, void *resp,
                  size_t resp_len);
int ena_get_feature(QEna *d, uint8_t id, uint8_t ver, uint64_t ctrl_buf,
                    uint32_t ctrl_len, struct ena_admin_get_feat_resp *resp);
int ena_set_feature(QEna *d, struct ena_admin_set_feat_cmd *cmd,
                    uint64_t ctrl_buf, uint32_t ctrl_len);
int ena_create_cq(QEna *d, uint16_t depth, uint8_t entry_words,
                  uint32_t msix_vector, uint64_t base,
                  struct ena_admin_acq_create_cq_resp_desc *resp);
int ena_create_sq(QEna *d, bool tx, uint8_t placement, uint16_t cq_idx,
                  uint16_t depth, uint64_t base,
                  struct ena_admin_acq_create_sq_resp_desc *resp);
int ena_destroy_sq(QEna *d, uint16_t sq_idx, bool tx);
int ena_destroy_cq(QEna *d, uint16_t cq_idx);
void ena_aenq_enable(QEna *d);
bool ena_aenq_poll(QEna *d, struct ena_admin_aenq_entry *e);
/* reset + admin init + MSI-X + AENQ enable, mirroring driver bring-up */
void ena_bringup(QEna *d);

/* MSI-X: each vector writes ENA_TEST_MSIX_DATA to a guest scratch word */
void ena_msix_setup(QEna *d, uint16_t vector);
bool ena_msix_fired(QEna *d, uint16_t vector);
void ena_msix_clear(QEna *d, uint16_t vector);

/* network backend: -netdev socket,fd=... with 4-byte BE length framing */
void *ena_test_before(GString *cmd_line, void *arg);
int ena_backend_fd(void *data);
void ena_backend_send(int fd, const void *frame, size_t len);
ssize_t ena_backend_recv(int fd, void *buf, size_t cap);

#endif
