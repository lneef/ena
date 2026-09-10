# QEMU ENA emulation

A QEMU device model of the Amazon Elastic Network Adapter (`-device ena`),
implemented against the AWS ENA driver for DPDK.

```
hw/              device model (ena.c: PCI/registers, ena_admin.c: admin queue and
                 features, ena_tx.c: TX and LLQ, ena_rx.c: RX and RSS, ena_defs/: ABI)
tests/           qtests (libqos driver in ena_qos.c) and tests/system/ host-side tests
qemu-ena.patch   meson/Kconfig glue applied to the qemu submodule at build time
qemu/            QEMU submodule (pinned in the Makefile)
```

## Build

Requirements: a QEMU build environment (gcc or clang, ninja, meson, python3,
pkg-config, glib-2.0 and pixman development packages).

```
make setup   # once: check out the qemu submodule
make build   # apply the glue patch, configure, build qemu-system-x86_64 and qos-test
make clean   # remove the build directory and revert the glue patch
```

The device registers as PCI `1d0f:ec20`. Attach it like any NIC:

```
qemu-system-x86_64 ... -device ena,netdev=n0,mac=52:54:00:00:00:02 -netdev user,id=n0
```

If glib and pixman are only available through nix, wrap the build:

```
nix-shell -p pkg-config glib pixman python3 ninja meson zlib --run 'make build'
```

## Tests

Unit tests run inside qtest and need no guest OS:

```
cd qemu/build
QTEST_QEMU_BINARY=./qemu-system-x86_64 ./tests/qtest/qos-test --tap -k 2>&1 | grep 'ena/'
```

System tests drive the emulated NIC with the MiniDPDK ENA PMD inside the miniosv
unikernel (checked out as `miniosv/` next to this README, built with `make` in
that directory). The pong benchmark echoes UDP frames; the host injects them
through a UDP socket netdev, so no root privileges are needed.

```
# bring-up plus single-queue echo
cd miniosv
./scripts/run.py --novnc --nogdb -c 1 --qemu-path ../qemu/build/qemu-system-x86_64 \
    --pass-args="-device ena,netdev=n0,mac=52:54:00:00:00:02" \
    --pass-args="-netdev socket,id=n0,udp=127.0.0.1:1235,localaddr=127.0.0.1:1234"
# in another shell, once the guest printed "0, 0, 0, 0":
python3 .claude/skills/ena-system-test/inject.py --dst-mac 52:54:00:00:00:02 --count 32

# multi-queue: one pong thread per vCPU, concurrent flows, RSS distribution check
python3 tests/system/ena_multiqueue_test.py --vcpus 4 --flows 64
# same with 256-byte LLQ entries (the device recommends them to the driver)
python3 tests/system/ena_multiqueue_test.py --device-opts ,llq-large-header=on
# interrupt-driven guest: build miniosv with `make BENCH_RX_IRQ=1`, then the pong
# threads sleep on their RX queue MSI-X vector and the test checks every queue
# served interrupts
python3 tests/system/ena_multiqueue_test.py --rx-irq
```

If the host drops datagrams before QEMU reads them (the test reports
`RcvbufErrors`), slow the senders down with `--pace-us 20000`.

Device properties: `llq-large-header=on|off` (default off) selects whether the
device recommends 256-byte or 128-byte LLQ entries; both sizes are supported.

The multi-queue test boots the guest itself and exits 0 when every frame is
echoed with valid checksums and the guest's per-queue counters match the
Toeplitz distribution computed on the host.

## Writing a system test script

`tests/system/ena_multiqueue_test.py` is the template. A host-side test needs no
root privileges and no guest changes; it works through four pieces:

1. **Boot the guest.** Start `miniosv/scripts/run.py` with `--novnc --nogdb`,
   `-c <vcpus>` and two `--pass-args`: the device
   (`-device ena,netdev=n0,mac=<dst-mac>` plus optional properties) and a UDP
   socket netdev (`-netdev socket,id=n0,udp=127.0.0.1:<listen>,localaddr=127.0.0.1:<peer>`).
   Read stdout in a thread. The bench prints `queues: N` (one RX/TX pair per
   vCPU), then `0, 0, 0, 0` once the port is up, and `queue i rx=.. tx=.. irq=..`
   every 2 s. Treat `ERR`, `no dev`, `configure failed` or `Starting dev failed`
   as a bring-up failure.
2. **Inject frames.** The socket netdev carries one Ethernet frame per UDP
   datagram: bind a socket to `<listen>` and `sendto` raw frames to `<peer>`;
   frames the guest transmits arrive on the same socket. The pong bench echoes
   IPv4/UDP frames addressed to its MAC and destination port (defaults
   `52:54:00:00:00:02`, `10.0.0.2`, port 1234), swapping MACs, IPs and ports and
   filling in IP and UDP checksums. Everything else is dropped, so a bad frame
   shows up as a missing echo. Put a flow id and sequence number in the payload
   to match echoes to what was sent.
3. **Verify echoes.** Check the swapped addresses, that the IPv4 and UDP
   checksums verify to zero, and that every (flow, sequence) pair came back.
   Pace the senders: the netdev's UDP socket is a plain kernel socket, and
   bursts larger than its buffer are dropped before QEMU reads them
   (`/proc/net/snmp` `Udp: RcvbufErrors` counts them).
4. **Check queue placement.** The bench programs an identity indirection table
   (`entry i -> queue i % N`) and the key `RSS_KEY` in the script. Compute the
   Toeplitz hash over `src ip, dst ip, src port, dst port` and expect the frame
   on queue `(hash % 128) % N`; compare against the last `queue i rx=` report.
   Interrupt-driven guests (`BENCH_RX_IRQ=1`) additionally report `irq=` per
   queue.

Device-side variants come from `--device-opts` (extra `-device ena` options)
and guest-side variants from how miniosv was built.

## Missing for DPDK

Everything the ENA PMD needs to bring the port up and move traffic is
implemented. Not implemented, and either skipped or degraded gracefully by the
PMD:

- extended statistics: ENI, ENA SRD and customer metrics (`xstats`), only basic
  stats exist
- HW hints (driver timeouts) and the AENQ notification, warning, fatal-error and
  configuration-notification groups
- fragment bypass and HW RX timestamping
- RSS: CRC32 hash function, symmetric hashing (`input_sort`), per-field
  source-only/destination-only selection
- LLQ TX burst limit (`LIMIT_TX_BURST` accelerated mode)
- second device id `1d0f:ec21`, live migration
