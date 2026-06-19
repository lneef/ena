/*
 * ENA RX datapath qtests (host placement, polling mode, no IRQ).
 *
 * Exercises the device-side RX flow documented in docs/wiki/rx-path.md and
 * docs/wiki/rx-descriptors.md: the driver posts empty buffers to the RX SQ as
 * self-contained single-buffer descriptors (FIRST|LAST|COMP_REQ, stamped with
 * the SQ phase) and rings the per-SQ doorbell with the new absolute tail. A
 * frame is then delivered to the device from QEMU's network backend; the
 * device must DMA the frame into the posted buffer(s) and post one RX
 * completion descriptor (cdesc) per consumed buffer, FIRST on the first cdesc
 * and LAST on the last, echoing each buffer's req_id and carrying the CQ phase
 * bit (status bit 24). Completions are found purely by the CQ phase bit (no CQ
 * head doorbell, no MSI-X), mirroring ena_com_get_next_rx_cdesc.
 *
 * Packet injection mirrors QEMU's tests/qtest/e1000e-test.c: a socketpair is
 * handed to the device as a "-netdev socket,fd=,id=hs0" backend, and a frame
 * is injected by writing a 4-byte big-endian length prefix followed by the
 * frame bytes to the test's end of the pair.
 *
 * Placement model under test (multi-buffer scatter): the device may split one
 * frame across several posted RX buffers, one cdesc each. If the frame cannot
 * be placed into the posted buffers (insufficient total capacity / no buffer
 * posted), the device DROPS it: it writes no cdesc and leaves the CQ untouched.
 *
 * SCOPE: tests only. hw/ena.c does not implement the RX datapath or a NIC
 * backend yet, so these tests are expected to FAIL until that lands. They
 * encode the device contract the separate RX implementation must satisfy.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the QEMU source tree.
 */
#include "ena_test_queue.h"
#include "qemu/sockets.h"
#include "qemu/iov.h"

#define ENA_RX_TEST_DEPTH       256
#define ENA_RX_TEST_MAC         "52:54:00:12:34:56"

/* Largest frame any test injects; sized for static frame buffers. */
#define ENA_RX_TEST_MAX_FRAME   2048

/* ------------------------------------------------------------------ */
/* Frame construction + injection                                      */
/* ------------------------------------------------------------------ */

/*
 * Fill @frame (>= 14 bytes) with a minimal Ethernet header (broadcast
 * destination so no unicast MAC filter is assumed) plus a @seed-keyed byte
 * pattern, so a placed frame can be byte-compared against the guest buffers.
 */
static void ena_rx_build_frame(uint8_t *frame, size_t len, uint8_t seed)
{
    size_t i;

    g_assert_cmpuint(len, >=, 14);
    memset(frame, 0xff, 6);                  /* dst: broadcast */
    memset(frame + 6, 0x02, 6);              /* src: locally administered */
    frame[12] = 0x08;                        /* ethertype 0x0800 (IPv4) */
    frame[13] = 0x00;
    for (i = 14; i < len; i++) {
        frame[i] = (uint8_t)(seed + i);
    }
}

/*
 * Inject one frame into the device via the socket backend: a 4-byte big-endian
 * length header followed by the frame bytes (the framing net/socket.c expects
 * for a SOCK_STREAM "-netdev socket" backend), exactly as e1000e-test.c does.
 */
static void ena_rx_inject(int *socks, const void *frame, size_t len)
{
    uint32_t hdr = cpu_to_be32((uint32_t)len);
    struct iovec iov[] = {
        { .iov_base = &hdr,            .iov_len = sizeof(hdr) },
        { .iov_base = (void *)frame,   .iov_len = len },
    };
    ssize_t ret = iov_send(socks[0], iov, 2, 0, sizeof(hdr) + len);

    g_assert_cmpint(ret, ==, (ssize_t)(sizeof(hdr) + len));
}

/* ------------------------------------------------------------------ */
/* RX SQ buffer posting / CQ completion helpers                        */
/* ------------------------------------------------------------------ */

/*
 * Post one RX buffer descriptor at the SQ tail, stamped with the SQ's current
 * producer phase, then advance the tail (flipping phase on ring wrap). Every
 * RX buffer is a self-contained single-buffer transaction: FIRST|LAST|COMP_REQ
 * are always set (rx-path.md §2.1). length == 0 would mean 64 KiB, so callers
 * pass an explicit non-zero @len.
 */
static void ena_rx_post_buffer(ENATestCtx *t, ENAQueue *q, uint64_t buf,
                               uint16_t len, uint16_t req_id)
{
    struct ena_eth_io_rx_desc d;
    uint32_t idx = q->sq_tail & (q->depth - 1);

    memset(&d, 0, sizeof(d));
    d.length = cpu_to_le16(len);
    d.ctrl = (q->sq_phase & ENA_ETH_IO_RX_DESC_PHASE_MASK) |
             ENA_ETH_IO_RX_DESC_FIRST_MASK |
             ENA_ETH_IO_RX_DESC_LAST_MASK |
             ENA_ETH_IO_RX_DESC_COMP_REQ_MASK;
    d.req_id = cpu_to_le16(req_id);
    d.buff_addr_lo = cpu_to_le32((uint32_t)buf);
    d.buff_addr_hi = cpu_to_le16((uint16_t)(buf >> 32));

    qtest_memwrite(t->qts, q->sq_base + (uint64_t)idx * ENA_RX_DESC_SIZE,
                   &d, sizeof(d));

    q->sq_tail++;
    if ((q->sq_tail & (q->depth - 1)) == 0) {
        q->sq_phase ^= 1;
    }
}

/* Ring the RX SQ doorbell with the current absolute tail (rx-path.md §2.2). */
static void ena_rx_doorbell(ENATestCtx *t, ENAQueue *q)
{
    ena_reg_write(t, q->sq_doorbell_off, q->sq_tail);
}

/* Allocate and zero a guest RX buffer. */
static uint64_t ena_rx_alloc_buf(ENATestCtx *t, size_t len)
{
    uint64_t buf = guest_alloc(t->alloc, len);

    qtest_memset(t->qts, buf, 0, len);
    return buf;
}

/*
 * Poll the RX CQ for the next completion at q->cq_head and return it in @out,
 * mirroring ena_com_get_next_rx_cdesc: wait until the cdesc's phase bit
 * (status bit 24) matches the expected CQ phase, reject non-zero MBZ7/MBZ17,
 * then advance the consumer (flipping phase on wrap). Fails (no hang) if the
 * device never posts the completion within the poll budget.
 */
static void ena_rx_poll_cdesc(ENATestCtx *t, ENAQueue *q,
                              struct ena_eth_io_rx_cdesc_base *out)
{
    uint32_t idx = q->cq_head & (q->depth - 1);
    int i;

    for (i = 0; i < ENA_TEST_POLL_RETRIES; i++) {
        struct ena_eth_io_rx_cdesc_base c;
        uint32_t status;

        qtest_memread(t->qts, q->cq_base + (uint64_t)idx * ENA_RX_CDESC_SIZE,
                      &c, sizeof(c));
        status = le32_to_cpu(c.status);
        if (((status >> ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT) & 1) ==
            q->cq_phase) {
            /* MBZ bits in status must be zero (rx-descriptors.md). */
            g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_MBZ7_MASK, ==, 0);
            g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_MBZ17_MASK, ==, 0);

            *out = c;
            q->cq_head++;
            if ((q->cq_head & (q->depth - 1)) == 0) {
                q->cq_phase ^= 1;
            }
            return;
        }
        g_usleep(ENA_TEST_POLL_DELAY_US);
    }

    g_assert_not_reached(); /* device never posted the RX completion */
}

/*
 * Assert the device has NOT posted a completion at the current CQ head: the
 * slot's phase bit must still differ from the expected phase. Drop tests call
 * this only after a *subsequent* frame has been completed (see the drop tests),
 * so the device has provably reached past the dropped frame.
 */
static void ena_rx_assert_no_cdesc(ENATestCtx *t, ENAQueue *q)
{
    uint32_t idx = q->cq_head & (q->depth - 1);
    struct ena_eth_io_rx_cdesc_base c;
    uint32_t status;

    qtest_memread(t->qts, q->cq_base + (uint64_t)idx * ENA_RX_CDESC_SIZE,
                  &c, sizeof(c));
    status = le32_to_cpu(c.status);
    g_assert_cmpuint((status >> ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT) & 1, !=,
                     q->cq_phase);
}

static void ena_assert_ready(ENATestCtx *t)
{
    uint32_t sts = ena_reg_read(t, ENA_REGS_DEV_STS_OFF);

    g_assert_cmphex(sts & ENA_REGS_DEV_STS_READY_MASK, ==,
                    ENA_REGS_DEV_STS_READY_MASK);
    g_assert_cmphex(sts & ENA_REGS_DEV_STS_FATAL_ERROR_MASK, ==, 0);
}

/*
 * Read @len bytes the device placed into @buf at @offset and compare against
 * the matching slice of @frame.
 */
static void ena_rx_assert_buf_eq(ENATestCtx *t, uint64_t buf, uint8_t offset,
                                 const uint8_t *frame, uint16_t len)
{
    uint8_t got[ENA_RX_TEST_MAX_FRAME];

    g_assert_cmpuint(len, <=, sizeof(got));
    qtest_memread(t->qts, buf + offset, got, len);
    g_assert_cmpmem(got, len, frame, len);
}

/* ------------------------------------------------------------------ */
/* One buffer big enough -> one completion                             */
/* ------------------------------------------------------------------ */

/*
 * The smallest RX transaction: one posted buffer larger than the frame. The
 * device must place the whole frame into it and post exactly one cdesc with
 * FIRST|LAST, the buffer's req_id, length == frame length, and the placed bytes
 * must equal the injected frame. No second completion may appear.
 */
static void test_rx_single_buffer(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    int *socks = data;
    ENATestCtx t;
    ENAQueue q;
    uint64_t buf;
    uint8_t frame[128];
    const uint16_t req_id = 7;
    const uint16_t buf_len = 2048;
    struct ena_eth_io_rx_cdesc_base c;
    uint32_t status;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_RX,
                             ENA_RX_TEST_DEPTH, &q);
    buf = ena_rx_alloc_buf(&t, buf_len);

    ena_rx_post_buffer(&t, &q, buf, buf_len, req_id);
    ena_rx_doorbell(&t, &q);

    ena_rx_build_frame(frame, sizeof(frame), 0x10);
    ena_rx_inject(socks, frame, sizeof(frame));

    ena_rx_poll_cdesc(&t, &q, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK, ==,
                    ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK, ==,
                    ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK);
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, req_id);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, sizeof(frame));
    ena_rx_assert_buf_eq(&t, buf, c.offset, frame, sizeof(frame));

    ena_rx_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Frame scattered across several buffers -> a cdesc per buffer        */
/* ------------------------------------------------------------------ */

/*
 * A frame larger than a single posted buffer must be scattered across as many
 * posted buffers as needed, one cdesc each. We post four 64-byte buffers with
 * NON-IDENTITY req_ids (proving req_id is a buffer handle decoupled from the SQ
 * slot, rx-path.md §2.3) and inject a 200-byte frame. The device must emit four
 * cdescs in posting order with per-buffer lengths 64,64,64,8 (summing to 200),
 * FIRST on the first and LAST on the last only, each echoing its buffer's
 * req_id; reassembling the buffers must reproduce the frame.
 */
static void test_rx_scatter_multi_buffer(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    int *socks = data;
    ENATestCtx t;
    ENAQueue q;
    const uint16_t req_ids[] = { 5, 9, 2, 7 };
    const uint16_t buf_len = 64;
    const int nbuf = ARRAY_SIZE(req_ids);
    uint64_t bufs[ARRAY_SIZE(req_ids)];
    uint8_t frame[200];
    uint16_t placed = 0;
    int i;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_RX,
                             ENA_RX_TEST_DEPTH, &q);

    for (i = 0; i < nbuf; i++) {
        bufs[i] = ena_rx_alloc_buf(&t, buf_len);
        ena_rx_post_buffer(&t, &q, bufs[i], buf_len, req_ids[i]);
    }
    ena_rx_doorbell(&t, &q);

    ena_rx_build_frame(frame, sizeof(frame), 0x40);
    ena_rx_inject(socks, frame, sizeof(frame));

    for (i = 0; i < nbuf; i++) {
        struct ena_eth_io_rx_cdesc_base c;
        uint32_t status;
        uint16_t seg;

        ena_rx_poll_cdesc(&t, &q, &c);
        status = le32_to_cpu(c.status);

        /* FIRST only on the first cdesc, LAST only on the last. */
        g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK, ==,
                        (i == 0) ? ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK : 0);
        g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK, ==,
                        (i == nbuf - 1) ? ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK : 0);

        g_assert_cmpuint(le16_to_cpu(c.req_id), ==, req_ids[i]);

        seg = le16_to_cpu(c.length);
        g_assert_cmpuint(seg, <=, buf_len);
        /* Data offset is meaningful only on the first cdesc. */
        ena_rx_assert_buf_eq(&t, bufs[i], (i == 0) ? c.offset : 0,
                             frame + placed, seg);
        placed += seg;
    }

    /* Every byte of the frame landed, across exactly nbuf buffers. */
    g_assert_cmpuint(placed, ==, sizeof(frame));

    ena_rx_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* CQ phase bit tracking across a full ring wrap                       */
/* ------------------------------------------------------------------ */

/*
 * The CQ phase bit is the completion-validity signal: the device stamps the
 * current pass's phase on each cdesc and flips it every time its CQ producer
 * wraps the ring (rx-path.md §4.1). Using a tiny depth, receive single-buffer
 * frames one at a time across more than two full laps; ena_rx_poll_cdesc
 * asserts every cdesc carries the expected phase (flipping locally on wrap), so
 * a device that fails to flip -- or flips at the wrong slot -- is caught. We
 * also confirm the expected phase actually toggled on the wrap boundary.
 */
static void test_rx_cq_phase_wrap(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    int *socks = data;
    ENATestCtx t;
    ENAQueue q;
    const uint16_t depth = 4;
    const uint16_t buf_len = 2048;
    const int iters = 2 * depth + 1; /* cross two wrap boundaries */
    uint8_t frame[128];
    int i;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_RX, depth, &q);

    for (i = 0; i < iters; i++) {
        uint16_t req_id = i % depth; /* keep req_id < q_depth */
        uint8_t phase_before = q.cq_phase;
        uint64_t buf = ena_rx_alloc_buf(&t, buf_len);
        struct ena_eth_io_rx_cdesc_base c;
        uint32_t status;

        ena_rx_post_buffer(&t, &q, buf, buf_len, req_id);
        ena_rx_doorbell(&t, &q);

        ena_rx_build_frame(frame, sizeof(frame), (uint8_t)i);
        ena_rx_inject(socks, frame, sizeof(frame));

        /* ena_rx_poll_cdesc asserts cdesc phase == expected. */
        ena_rx_poll_cdesc(&t, &q, &c);
        status = le32_to_cpu(c.status);
        g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK, ==,
                        ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK);
        g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK, ==,
                        ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK);
        g_assert_cmpuint(le16_to_cpu(c.req_id), ==, req_id);
        g_assert_cmpuint(le16_to_cpu(c.length), ==, sizeof(frame));
        ena_rx_assert_buf_eq(&t, buf, c.offset, frame, sizeof(frame));

        /* The expected phase must flip exactly on the ring-wrap boundary. */
        if ((q.cq_head & (depth - 1)) == 0) {
            g_assert_cmpuint(q.cq_phase, !=, phase_before);
        } else {
            g_assert_cmpuint(q.cq_phase, ==, phase_before);
        }
    }

    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Drop: frame too large for the posted buffer capacity                */
/* ------------------------------------------------------------------ */

/*
 * A frame the posted buffers cannot hold must be dropped: the device writes no
 * cdesc and consumes no buffer. We post a single 64-byte buffer and inject a
 * 1500-byte frame that cannot be placed. To detect the drop race-free, we then
 * inject a 40-byte frame that fits the still-posted buffer: the FIRST completion
 * the device produces must be that 40-byte frame (req_id of the posted buffer),
 * proving the 1500-byte frame neither produced a cdesc nor consumed the buffer.
 */
static void test_rx_drop_too_large(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    int *socks = data;
    ENATestCtx t;
    ENAQueue q;
    uint64_t buf;
    uint8_t big[1500];
    uint8_t small[40];
    const uint16_t req_id = 0x11;
    const uint16_t buf_len = 64;
    struct ena_eth_io_rx_cdesc_base c;
    uint32_t status;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_RX,
                             ENA_RX_TEST_DEPTH, &q);
    buf = ena_rx_alloc_buf(&t, buf_len);

    ena_rx_post_buffer(&t, &q, buf, buf_len, req_id);
    ena_rx_doorbell(&t, &q);

    /* Cannot be placed into the lone 64-byte buffer -> dropped. */
    ena_rx_build_frame(big, sizeof(big), 0x80);
    ena_rx_inject(socks, big, sizeof(big));

    /* Fits the still-posted buffer; this is the first completion we expect. */
    ena_rx_build_frame(small, sizeof(small), 0x20);
    ena_rx_inject(socks, small, sizeof(small));

    ena_rx_poll_cdesc(&t, &q, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK, ==,
                    ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK, ==,
                    ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK);
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, req_id);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, sizeof(small));
    ena_rx_assert_buf_eq(&t, buf, c.offset, small, sizeof(small));

    /* Only the small frame completed; the dropped frame added nothing. */
    ena_rx_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* Drop: frame arrives with no buffers posted                          */
/* ------------------------------------------------------------------ */

/*
 * A frame arriving while no RX buffer is posted must be dropped (rx-path.md §7,
 * device-side out-of-buffers). Detected race-free as above: after the drop we
 * post a buffer and inject a second frame; the first (and only) completion must
 * be that second frame.
 */
static void test_rx_drop_no_buffers(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    int *socks = data;
    ENATestCtx t;
    ENAQueue q;
    uint64_t buf;
    uint8_t first[128];
    uint8_t second[100];
    const uint16_t req_id = 0x22;
    const uint16_t buf_len = 256;
    struct ena_eth_io_rx_cdesc_base c;
    uint32_t status;

    ena_bringup(&t, obj, alloc);
    ena_create_io_queue_host(&t, ENA_ADMIN_SQ_DIRECTION_RX,
                             ENA_RX_TEST_DEPTH, &q);

    /* No buffer posted yet -> this frame is dropped. */
    ena_rx_build_frame(first, sizeof(first), 0x55);
    ena_rx_inject(socks, first, sizeof(first));

    /* Now post a buffer and inject a frame that must be the first completion. */
    buf = ena_rx_alloc_buf(&t, buf_len);
    ena_rx_post_buffer(&t, &q, buf, buf_len, req_id);
    ena_rx_doorbell(&t, &q);

    ena_rx_build_frame(second, sizeof(second), 0x66);
    ena_rx_inject(socks, second, sizeof(second));

    ena_rx_poll_cdesc(&t, &q, &c);
    status = le32_to_cpu(c.status);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK, ==,
                    ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK);
    g_assert_cmphex(status & ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK, ==,
                    ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK);
    g_assert_cmpuint(le16_to_cpu(c.req_id), ==, req_id);
    g_assert_cmpuint(le16_to_cpu(c.length), ==, sizeof(second));
    ena_rx_assert_buf_eq(&t, buf, c.offset, second, sizeof(second));

    ena_rx_assert_no_cdesc(&t, &q);
    ena_assert_ready(&t);
    ena_destroy_io_queue(&t, &q);
}

/* ------------------------------------------------------------------ */
/* qgraph wiring: own node with a socket netdev backend                */
/* ------------------------------------------------------------------ */

/*
 * Dedicated "ena-rxpath" node so this file can attach a netdev backend without
 * disturbing the shared node the other ENA test files use. Mirrors
 * ena_qos_node_register() but adds "netdev=hs0" + a fixed MAC to the device
 * options; the matching "-netdev socket,id=hs0" is appended per test by
 * rx_data_init().
 *
 * Unlike ena_qos_node_register(), this node does NOT qos_node_produces()
 * "pci-device": doing so would route the framework's generic pci-device "nop"
 * test through this node, and that test has no rx_data_init() before-hook to
 * append "-netdev socket,id=hs0", so QEMU would refuse to start (netdev=hs0
 * unresolved). The e1000e node omits produces for the same reason. Our own
 * tests still receive the node's QOSGraphObject (a QENA) as obj.
 */
static void ena_rxpath_node_register(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0,netdev=hs0,mac=" ENA_RX_TEST_MAC,
    };
    add_qpci_address(&opts, &(QPCIAddress) { .devfn = QPCI_DEVFN(4, 0) });

    qos_node_create_driver_named("ena-rxpath", "ena", ena_create);
    qos_node_consumes("ena-rxpath", "pci-bus", &opts);
}

static void rx_sockets_cleanup(void *p)
{
    int *socks = p;

    close(socks[0]);
    qos_invalidate_command_line();
    close(socks[1]);
    g_free(socks);
}

/*
 * Per-test "before" hook: create a socketpair, hand the device end to QEMU as a
 * stream "-netdev socket" backend, and pass the test end to the test as @data.
 */
static void *rx_data_init(GString *cmd_line, void *arg)
{
    int *socks = g_new(int, 2);
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, socks);

    g_assert_cmpint(ret, !=, -1);
    g_string_append_printf(cmd_line, " -netdev socket,fd=%d,id=hs0 ", socks[1]);
    g_test_queue_destroy(rx_sockets_cleanup, socks);

    return socks;
}

static void ena_register_nodes(void)
{
    QOSGraphTestOptions opts = { .before = rx_data_init };

    ena_rxpath_node_register();

    qos_add_test("rxpath/single-buffer", "ena-rxpath",
                 test_rx_single_buffer, &opts);
    qos_add_test("rxpath/scatter-multi-buffer", "ena-rxpath",
                 test_rx_scatter_multi_buffer, &opts);
    qos_add_test("rxpath/cq-phase-wrap", "ena-rxpath",
                 test_rx_cq_phase_wrap, &opts);
    qos_add_test("rxpath/drop-too-large", "ena-rxpath",
                 test_rx_drop_too_large, &opts);
    qos_add_test("rxpath/drop-no-buffers", "ena-rxpath",
                 test_rx_drop_no_buffers, &opts);
}

libqos_init(ena_register_nodes);
