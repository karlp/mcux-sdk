/*
 * Marel hf 2024.
 * Because NXP doesn't see fit to support K70...
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef __FSL_DEVICE_REGISTERS_H__
#define __FSL_DEVICE_REGISTERS_H__

/*
 * Include the cpu specific register header files.
 *
 * The CPU macro should be declared in the project or makefile.
 */
#if (defined(CPU_MK70FX512VMJ12) || defined(CPU_MK70FN1M0VMJ12) || \
     defined(CPU_MK70FX512VMJ15) || defined(CPU_MK70FN1M0VMJ15))

#define K70F12_SERIES

/* CMSIS-style register definitions */
#include "MK70F12.h"
/* CPU specific feature definitions */
// Nope, we're not supporting any of that.
// #include "MK70F12_features.h"

#else
#error "No valid CPU defined!"
#endif

#endif
