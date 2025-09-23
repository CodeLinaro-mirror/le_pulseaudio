/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef foopalutilsfoo
#define foopalutilsfoo

#include <pulse/sample.h>

#include <PalApi.h>
#include <PalDefs.h>
#include "pal-jack.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

pal_device_id_t pa_pal_util_device_name_to_enum(const char *device);
pal_device_id_t pa_pal_util_port_name_to_enum(const char *port_name);
uint32_t pa_pal_get_channel_count(pa_channel_map *pa_map);
bool pa_pal_channel_map_to_pal(pa_channel_map *pa_map, struct pal_channel_info *pal_map);
pal_audio_fmt_t pa_pal_util_get_pal_format_from_pa_encoding(pa_encoding_t pa_format, pal_snd_dec_t *pal_snd_dec);
#ifndef PAL_DISABLE_COMPRESS_AUDIO_SUPPORT
int pa_pal_util_set_pal_metadata_from_pa_format(const pa_format_info *format);
#endif
pa_channel_map* pa_pal_util_channel_map_init(pa_channel_map *m, unsigned channels);
pa_pal_jack_type_t pa_pal_util_get_jack_type_from_port_name(const char *port_name);
const char* pa_pal_util_get_port_name_from_jack_type(pa_pal_jack_type_t jack_type);
pa_channel_map pa_pal_map_remove_invalid_channels(pa_channel_map *def_map_with_inval_ch);
void pa_pal_util_get_jack_sys_path(pa_pal_card_port_config *config_port, pa_pal_jack_in_config *jack_in_config);
int pa_pal_set_volume(pal_stream_handle_t *handle, uint32_t num_channels, float value);
int pa_pal_set_device_connection_state(pal_device_id_t pal_dev_id, bool connection_state);
pa_pal_card_avoid_processing_config_id_t pa_pal_utils_get_config_id_from_string(const char *config_str);
#endif
