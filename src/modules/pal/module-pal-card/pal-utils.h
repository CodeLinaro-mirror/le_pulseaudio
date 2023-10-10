/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
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

#ifndef foopalutilsfoo
#define foopalutilsfoo

#include <pulse/sample.h>

#include <PalApi.h>
#include <PalDefs.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define BITPOOL_MAX                  0xFFFF
#define BITPOOL_MAX_CONC_SESSION_IDS 16

pal_device_id_t pa_pal_util_device_name_to_enum(const char *device);
uint32_t pa_pal_get_channel_count(pa_channel_map *pa_map);
bool pa_pal_channel_map_to_pal(pa_channel_map *pa_map, struct pal_channel_info *pal_map);
int pa_pal_set_volume(pal_stream_handle_t *handle, uint32_t num_channels, float value);
int pa_pal_set_device_connection_state(pal_device_id_t pal_dev_id, bool connection_state);
unsigned short pa_pal_alloc_session_id(void);
void pa_pal_release_session_id(unsigned short session_id);

#endif
