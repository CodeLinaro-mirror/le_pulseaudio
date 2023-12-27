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
 *
 */

#ifndef foopalutilsfoo
#define foopalutilsfoo

#include <pulse/sample.h>

#include <PalApi.h>
#include <PalDefs.h>
#include "qal-jack.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

pal_device_id_t pa_pal_util_device_name_to_enum(const char *device);
uint32_t pa_pal_get_channel_count(pa_channel_map *pa_map);
bool pa_pal_channel_map_to_pal(pa_channel_map *pa_map, struct pal_channel_info *pal_map);
pal_audio_fmt_t pa_pal_util_get_pal_format_from_pa_encoding(pa_encoding_t pa_format, pal_snd_dec_t *pal_snd_dec);
int pa_pal_util_set_pal_metadata_from_pa_format(const pa_format_info *format);
pa_channel_map* pa_pal_util_channel_map_init(pa_channel_map *m, unsigned channels);
pa_pal_jack_type_t pa_pal_util_get_jack_type_from_port_name(const char *port_name);
const char* pa_pal_util_get_port_name_from_jack_type(pa_pal_jack_type_t jack_type);
pa_channel_map pa_pal_map_remove_invalid_channels(pa_channel_map *def_map_with_inval_ch);
void pa_pal_util_get_jack_sys_path(pa_pal_card_port_config *config_port, pa_pal_jack_in_config *jack_in_config);
int pa_pal_set_volume(pal_stream_handle_t *handle, uint32_t num_channels, float value);
int pa_pal_set_device_connection_state(pal_device_id_t pal_dev_id, bool connection_state);
#endif
