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
#ifndef SRC_MODULES_GROUP_MODULES_LIBPULSE_GROUP_NULL_GROUP_SINK_IMPL_H_
#define SRC_MODULES_GROUP_MODULES_LIBPULSE_GROUP_NULL_GROUP_SINK_IMPL_H_

#include <pulse/cdecl.h>
PA_C_DECL_BEGIN
#include <pulsecore/memchunk.h>
PA_C_DECL_END
#include <pulsemodules/group_sink.h>

#define MOD_EXPORT __attribute__((visibility("default")))
MOD_EXPORT group_sink_init_proto group_sink_init;
MOD_EXPORT group_sink_done_proto group_sink_done;

#endif  // SRC_MODULES_GROUP_MODULES_LIBPULSE_GROUP_NULL_GROUP_SINK_IMPL_H_
