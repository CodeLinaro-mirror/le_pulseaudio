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

#ifndef fooqalutilsfoo
#define fooqalutilsfoo

#include <pulse/sample.h>

#include <QalApi.h>
#include <QalDefs.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

qal_device_id_t pa_qal_util_device_name_to_enum(const char *device);
bool pa_qal_channel_map_to_qal(pa_channel_map *pa_map, struct qal_channel_info *qal_map);
#endif
