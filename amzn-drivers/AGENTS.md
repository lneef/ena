# ENA reference directory
This directory holds the ENA-driver reference implementations for DPDK, FreeBSD and Linux.
For us only DPDK is relevant.

## Structure
```
./userspace/dpdk/ena/ ENA implementation dir for DPDK
./userspace/dpdk/ena/base/ Hardware Layer directly interacting with the device
./userspace/dpdk/ena/base/ena_defs/ Structure Definitions/Descriptor Layouts for ENA
./userspace/dpdk/ena/ena_ethdev.c Device Semantics/User expected behavior of Data and Control path
./userspace/dpdk/ena/ena_ethdev.h Higher level structures for device management in the runtime/kernel
./kernel/fbsd/ Driver implementation for FreeBSD Kernel
./kernel/linux/ Driver implementation for Linux Kernel
```
## Guidelines
- The main reference is the driver implementation for DPDK
- If a feature is not implemented/missing in DPDK search other implementation in the order:
    - kernel/fbsd/
    - kernel/linux/

