/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 and only version 2.1 as published by the Free Software Foundation
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#ifndef SRC_MODULES_GROUP_MODULES_MODULES_CLOCK_H_
#define SRC_MODULES_GROUP_MODULES_MODULES_CLOCK_H_

#include "pulse/sample.h"

inline static pa_nsec_t ts_clock_now();

#if defined(__aarch64__)

inline static pa_nsec_t ts_clock_now() {
    uint64_t ticks;
    asm volatile("mrs %0, cntvct_el0"
                 : "=r"(ticks));
    // 1000000000(ns/s) / 19200000(tick/s) == 625/12 ns/tick
    return (ticks * 625 / 12);
}

#elif defined(__arm__)

inline static pa_nsec_t ts_clock_now() {
    uint64_t ticks;
    asm volatile("mrrc p15, 1, %Q0, %R0, c14"
                 : "=r"(ticks));
    // 1000000000(ns/s) / 19200000(tick/s) == 625/12 ns/tick
    return (ticks * 625 / 12);
}

#else
#    error "Unsupported platform"
#endif

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_CLOCK_H_
