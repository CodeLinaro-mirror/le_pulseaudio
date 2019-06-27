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

#ifndef fooqahwutilsfoo
#define fooqahwutilsfoo

#include <pulse/sample.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-jack.h"
#include "qahw-card.h"

#define KV_PAIR_MAX_LENGTH 100
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

audio_format_t pa_qahw_util_get_qahw_format_from_pa_sample(pa_sample_format_t format);
const char* pa_qahw_util_get_port_name_from_jack_type(pa_qahw_jack_type_t jack_type);
pa_qahw_jack_type_t pa_qahw_util_get_jack_type_from_port_name( const char *port_name);
audio_format_t pa_qahw_util_get_qahw_format_from_pa_encoding(pa_encoding_t pa_format);
audio_channel_mask_t pa_qahw_util_get_channel_mask_from_num_channels(unsigned int num_channels);
unsigned int pa_qahw_util_get_num_channels_from_channel_mask(audio_channel_mask_t channel_mask);
pa_encoding_t pa_qahw_util_get_pa_encoding_from_qahw_format(audio_format_t qahw_format);
const char *pa_qahw_util_audio_device_to_port_name(audio_devices_t audio_device, pa_hashmap *ports);
audio_devices_t pa_qahw_util_port_to_qahw_device(const char *port);
audio_devices_t pa_qahw_util_device_name_to_enum(const char *device);
pa_sample_format_t pa_qahw_util_get_pa_sample_from_qahw_format(audio_format_t format);
int pa_qahw_utils_format_to_sample_spec(pa_format_info *format, pa_sample_spec *ss, pa_channel_map *map,
                                                        pa_sample_spec *default_ss, pa_channel_map *default_map);
bool pa_qahw_channel_map_to_qahw(pa_channel_map *pa_map, struct qahw_out_channel_map_param *qahw_map);
bool pa_qahw_channel_map_from_qahw(struct qahw_out_channel_map_param *qahw_map, pa_channel_map *pa_map);
void pa_qahw_util_channel_allocation_to_pa_channel_map(pa_channel_map *m, uint32_t channel_allocation);
void pa_qahw_util_get_jack_sys_path(pa_qahw_card_port_config *config_port, pa_qahw_jack_in_config *jack_in_config);
pa_channel_map* pa_qahw_util_channel_map_init(pa_channel_map *m, unsigned channels);
int pa_qahw_util_set_qahw_metadata_from_pa_format(const pa_format_info *format);
audio_channel_mask_t pa_qahw_util_in_mask_from_count(uint32_t channel_count);
pa_qahw_card_avoid_processing_config_id_t pa_qahw_utils_get_config_id_from_string(const char *config_str);

#endif
