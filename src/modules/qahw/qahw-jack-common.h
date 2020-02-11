/*
 * Copyright (c) 2018, 2020, The Linux Foundation. All rights reserved.
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

#ifndef fooqahwjackcommonhfoo
#define fooqahwjackcommonhfoo

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include "qahw-jack.h"
#include "qahw-utils.h"

#define JACK_HEADSET_DEVICE_PATH "/dev/input"

struct pa_qahw_jack_info {
    pa_qahw_jack_type_t jack_type;
    char* name;
};

struct pa_qahw_jack_data {
    pa_module *module;
    pa_qahw_jack_type_t jack_type;
    pa_hook *event_hook;

    void *client_data;
    void *prv_data;

    int ref_count;
};

struct pa_qahw_jack_data* pa_qahw_evdev_jack_device_open(pa_qahw_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                                                              pa_qahw_jack_callback_t callback, void *client_data);
int pa_qahw_evdev_jack_device_close(struct pa_qahw_jack_data *jdata, pa_module *m);

struct pa_qahw_jack_data* pa_qahw_hdmi_in_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                       pa_qahw_jack_callback_t callback, pa_qahw_jack_in_config *jack_in_config, void *client_data);
void pa_qahw_hdmi_in_jack_detection_disable(struct pa_qahw_jack_data *jdata, pa_module *m);

struct pa_qahw_jack_data* pa_qahw_external_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                                                                  pa_qahw_jack_callback_t callback, void *client_data);
void pa_qahw_external_jack_detection_disable(struct pa_qahw_jack_data *jdata, pa_module *m);

struct pa_qahw_jack_data* pa_qahw_spdif_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                        pa_qahw_jack_callback_t callback, pa_qahw_jack_in_config *jack_in_config, void *client_data);
void pa_qahw_spdif_jack_detection_disable(struct pa_qahw_jack_data *jdata, pa_module *m);

struct pa_qahw_jack_data* pa_qahw_hdmi_out_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                           pa_qahw_jack_callback_t callback, pa_qahw_jack_in_config *jack_in_config, void *client_data);
void pa_qahw_hdmi_out_jack_detection_disable(struct pa_qahw_jack_data *jdata, pa_module *m);

#endif

