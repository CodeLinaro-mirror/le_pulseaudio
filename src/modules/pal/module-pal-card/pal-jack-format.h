/*
 * Copyright (c) 2018, 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef foopaljackformathfoo
#define foopaljackformathfoo

#include <pulsecore/core-util.h>

#include <pulsecore/thread.h>
#include "pal-jack.h"

typedef struct pa_pal_jack_config {
    pa_encoding_t encoding;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_pal_jack_type_t active_jack;
    int32_t preemph_status;
    uint32_t dsd_rate;
} pa_pal_jack_out_config;

bool pa_pal_format_detection_get_value_from_path(const char* path, int *node_value);
#endif

