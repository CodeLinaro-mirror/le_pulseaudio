/*
 * Copyright (c) 2018, 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
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

#ifndef fooqaljackformathfoo
#define fooqaljackformathfoo

#include <pulsecore/core-util.h>

#include <pulsecore/thread.h>
#include "qal-jack.h"

typedef struct pa_pal_jack_config {
    pa_encoding_t encoding;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_pal_jack_type_t active_jack;
    int32_t preemph_status;
    uint32_t dsd_rate;
} pa_pal_jack_out_config;

bool pa_pal_format_detection_get_value_from_path(const char* path, int *node_value);
void pa_pal_format_set_jack_default_config(pa_pal_jack_out_config *config);

#endif

