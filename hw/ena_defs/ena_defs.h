/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ENA device/driver shared definitions (BSD-3-Clause, Amazon). */
#ifndef HW_ENA_DEFS_H
#define HW_ENA_DEFS_H

#include "qemu/bitops.h"

#ifndef GENMASK
#define GENMASK(h, l) MAKE_64BIT_MASK(l, (h) - (l) + 1)
#endif

#include "ena_common_defs.h"
#include "ena_regs_defs.h"
#include "ena_admin_defs.h"
#include "ena_eth_io_defs.h"

#endif
