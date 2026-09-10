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
```

Device properties: `llq-large-header=on|off` (default off) selects whether the
device recommends 256-byte or 128-byte LLQ entries; both sizes are supported.

The multi-queue test boots the guest itself and exits 0 when every frame is
echoed with valid checksums and the guest's per-queue counters match the
Toeplitz distribution computed on the host.
