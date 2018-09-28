/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
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

#ifndef fooqahwutilsfoo
#define fooqahwutilsfoo

#include <pulse/sample.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-jack.h"

#define KV_PAIR_MAX_LENGTH 100
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

audio_format_t pa_qahw_util_get_qahw_format_from_pa_sample(pa_sample_format_t format);
const char* pa_qahw_util_jack_type_to_port_name(pa_qahw_jack_type_t jack_type);
audio_format_t pa_qahw_util_get_qahw_format_from_pa_encoding(pa_encoding_t pa_format);
audio_channel_mask_t pa_qahw_util_get_channel_mask_from_num_channels(unsigned int num_channels);
unsigned int pa_qahw_util_get_num_channels_from_channel_mask(audio_channel_mask_t channel_mask);
pa_encoding_t pa_qahw_util_get_pa_encoding_from_qahw_format(audio_format_t qahw_format);
const char *pa_qahw_util_audio_device_to_port_name(audio_devices_t audio_device, pa_hashmap *ports);
audio_devices_t pa_qahw_util_port_to_qahw_device(const char *port); /* FIXME remove if not needed */
audio_devices_t pa_qahw_util_device_name_convert_string_to_enum(const char *device);
pa_sample_format_t pa_qahw_util_get_pa_sample_from_qahw_format(audio_format_t format);
int pa_qahw_utils_convert_format_to_sample_spec(pa_format_info *format, pa_sample_spec *ss, pa_channel_map *map, pa_sample_spec *default_ss, pa_channel_map *default_map,
                                                int rate_idx, int sample_format_idx);
bool pa_qahw_channel_map_to_qahw(pa_channel_map *pa_map, struct qahw_out_channel_map_param *qahw_map);
bool pa_qahw_channel_map_from_qahw(struct qahw_out_channel_map_param *qahw_map, pa_channel_map *pa_map);
#endif
