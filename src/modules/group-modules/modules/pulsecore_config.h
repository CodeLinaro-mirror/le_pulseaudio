/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
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
#ifndef SRC_MODULES_GROUP_MODULES_MODULES_PULSECORE_CONFIG_H_
#define SRC_MODULES_GROUP_MODULES_MODULES_PULSECORE_CONFIG_H_

extern "C" {
#if __has_include("pulsecore/config.h")
#    include "pulsecore/config.h"  // LE build environment (using installed pulsecore)
#elif __has_include("config.h")
#    include "config.h"  // Desktop environment (using PulseAudio source tree)
#else
#    error "Cannot find config.h"
#endif
}

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_PULSECORE_CONFIG_H_
