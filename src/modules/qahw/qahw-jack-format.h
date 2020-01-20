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

#ifndef fooqahwjackformathfoo
#define fooqahwjackformathfoo

#include <pulsecore/core-util.h>

#include "qahw-jack.h"

typedef struct pa_qahw_jack_config {
    pa_encoding_t encoding;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_qahw_jack_type_t active_jack;
    int32_t preemph_status;
    uint32_t dsd_rate;
} pa_qahw_jack_out_config;

bool pa_qahw_format_detection_get_value_from_path(const char* path, int *node_value);

int pa_qahw_hdmi_jack_get_config(pa_qahw_jack_type_t jack_type, pa_qahw_jack_sys_path sys_path,
                                                          pa_qahw_jack_out_config *jack_config);

int pa_qahw_spdif_jack_get_config(pa_qahw_jack_type_t jack_type, pa_qahw_jack_sys_path sys_path,
                                                           pa_qahw_jack_out_config *jack_config);

#endif

