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

#ifndef foopaljackcommonhfoo
#define foopaljackcommonhfoo

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include <pulsecore/thread.h>
#include "pal-jack.h"

#define JACK_HEADSET_DEVICE_PATH "/dev/input"

struct pa_pal_jack_info {
    pa_pal_jack_type_t jack_type;
    char* name;
};

struct pa_pal_jack_data {
    pa_module *module;
    pa_pal_jack_type_t jack_type;
    pa_hook *event_hook;

    void *client_data;
    void *prv_data;

    int ref_count;
};

typedef enum {
    JACK_PARAM_KEY_DEVICE_CONNECTION = 1,
    JACK_PARAM_KEY_A2DP_SUSPEND,
    JACK_PARAM_KEY_DEVICE_SAMPLERATE,
    JACK_PARAM_KEY_MAX
} jack_param_key_t;

typedef struct {
    jack_param_key_t key;
    char *value;
} jack_prm_kvpair_t;

struct pa_pal_jack_data* pa_pal_hdmi_out_jack_detection_enable(pa_pal_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                           pa_pal_jack_callback_t callback, pa_pal_jack_in_config *jack_in_config, void *client_data);
void pa_pal_hdmi_out_jack_detection_disable(struct pa_pal_jack_data *jdata, pa_module *m);
struct pa_pal_jack_data* pa_pal_external_jack_detection_enable(pa_pal_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                                                                          pa_pal_jack_callback_t callback, void *client_data);
void pa_pal_external_jack_detection_disable(struct pa_pal_jack_data *jdata, pa_module *m);
int pa_pal_external_jack_parse_kvpair(const char *kvpair, jack_prm_kvpair_t *kv);

#endif


