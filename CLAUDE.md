# QEMU ENA Emulation
The goal of this project is a fully functioning device emulation of ENA based primarily on the DPDK driver published by AWS.
## Project Structure
```
amzn-drivers/ Reference Driver implementation for AWS ENA
hw/ ENA device implementation, including source and local headers
tests/ test suite runnable with qtest
docs/wiki/ wiki holding device specifiction
docs/ENA.md is a short overview on ENA taken from the Linux Kernel
```
## Guidelines
- Always consult the wiki for MMIO-Structure/Descriptor Layouts and Expected Device Behavior 
- Focus on the core features of the Control and Datapath(Implement Offloading/RSS only after these features are working)
- Before finishing a task verify against the wiki if the functionality is complete
- If you findout that you did not use registers/fields that seems vital to your current task check their behavior against the wiki
- The hardware device spec lives in wiki. Use this as first reference point when doing research. PROACTIVELY consult the reference implementation if
the wiki does not hold all information required or is ambiguous. 
- Use proper include paths from the project(subproject) root
- Write idiomatic code interfacing with QEMU:
    - If you are uncertain about exact behavior (preconditions/postconsitions/guarantees) of QEMU-Functionality, look it up
    - Before implementing new functionality at the Device-QEMU-interface yourself, you have to ensure that there is no QEMU-freature you can use

### 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that isn't required.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.

## Setup
The QEMU source lives in the `qemu` submodule (pinned to the tag in the
Makefile). Initialize it once after cloning, before the first build:
```
make setup   # or: git submodule update --init --recursive qemu
```
`make build` does not touch the submodule, so run `make setup` first.

## Building
```
make build   # build emulator + qtest harness, refresh compile_commands.json
make clean   # wipe the build dir and revert the in-tree qemu glue
```

## Verification
```
make build

# Run the qtest harness that compiles tests/ena_test.c.
cd qemu/build
QTEST_QEMU_BINARY=./qemu-system-x86_64 ./tests/qtest/qos-test --tap -k

# Confirm the ENA test source is included in the compile database.
cd ../..
rg "tests_ena_test|ena_test.c" compile_commands.json
```

### Final test: real guest + sockperf datapath

qtest only pokes registers. The final check boots two Linux guests, each with an
emulated ENA NIC linked by a QEMU `socket` netdev, and drives a real sockperf
workload — so the mainline `ena` driver binds (PCI `1d0f:0ec2`), sets up the
admin/IO queues, and pushes traffic through the tx/rx path under load. Run this
before considering a datapath change done; see `tests/sockperf/README.md`.
```
make build
# one-time: build a guest bzImage (ena driver) + rootfs (sockperf) with buildroot
tests/sockperf/mkimage.sh
# two terminals, server first:
KERNEL=tests/sockperf/bzImage ROOTFS=tests/sockperf/rootfs.img tests/sockperf/run.sh server
KERNEL=tests/sockperf/bzImage ROOTFS=tests/sockperf/rootfs.img tests/sockperf/run.sh client
```
