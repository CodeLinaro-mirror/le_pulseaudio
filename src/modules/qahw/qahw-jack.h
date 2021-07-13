/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
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

#ifndef fooqahwjackhfoo
#define fooqahwjackhfoo

#include <pulsecore/module.h>
#include <qahw_api.h>

typedef enum {
    PA_QAHW_JACK_TYPE_INVALID = -1,
    PA_QAHW_JACK_TYPE_WIRED_HEADSET = 0x1,
    PA_QAHW_JACK_TYPE_WIRED_HEADPHONE = 0x2,
    PA_QAHW_JACK_TYPE_LINEOUT = 0x4,
    PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS = 0x8,
    PA_QAHW_JACK_TYPE_HDMI_IN = 0x10,
    PA_QAHW_JACK_TYPE_BTA2DP_OUT = 0x20,
    PA_QAHW_JACK_TYPE_BTA2DP_IN = 0x40,
    PA_QAHW_JACK_TYPE_HDMI_ARC = 0x80,
    PA_QAHW_JACK_TYPE_SPDIF = 0x100,
    PA_QAHW_JACK_TYPE_BTSCO_IN = 0x200,
    PA_QAHW_JACK_TYPE_BTSCO_OUT = 0x400,
    PA_QAHW_JACK_TYPE_HDMI_OUT = 0x800,
    PA_QAHW_JACK_TYPE_SPDIF_OUT_OPTICAL = 0x1000,
    PA_QAHW_JACK_TYPE_SPDIF_OUT_COAXIAL = 0x2000,
    PA_QAHW_JACK_TYPE_LAST = PA_QAHW_JACK_TYPE_SPDIF_OUT_COAXIAL,
    PA_QAHW_JACK_TYPE_MAX = PA_QAHW_JACK_TYPE_LAST,
} pa_qahw_jack_type_t;

typedef enum {
    PA_QAHW_JACK_ERROR,
    PA_QAHW_JACK_AVAILABLE,
    PA_QAHW_JACK_UNAVAILABLE,
    PA_QAHW_JACK_CONFIG_UPDATE,
    PA_QAHW_JACK_NO_VALID_STREAM,
    PA_QAHW_JACK_SET_PARAM,
} pa_qahw_jack_event_t;

typedef struct pa_qahw_jack_event_data {
    pa_qahw_jack_type_t jack_type;
    pa_qahw_jack_event_t event;
    void *pa_qahw_jack_info; /* can be used to send any info related to a jack */
} pa_qahw_jack_event_data_t;

typedef size_t pa_qahw_jack_handle_t;

struct jack_userdata {
    pa_qahw_jack_type_t jack_type;
    pa_hook_slot *hook_slot;
};

typedef struct {
    const char *audio_state;
    const char *audio_format;
    const char *audio_rate;
    const char *audio_size;
    const char *audio_layout;
    const char *audio_channel;
    const char *audio_channel_alloc;
    const char *audio_preemph;
    const char *dsd_rate;

    const char *linkon_0;
    const char *power_on;
    const char *audio_path;
    const char *arc_enable;
    const char *earc_enable;

    const char *arc_audio_state;
    const char *arc_audio_format;
    const char *arc_audio_rate;
    const char *arc_audio_preemph;

    const char *hdmi_tx_state;
    const char *channel_status;
} pa_qahw_jack_sys_path;

typedef struct {
    pa_qahw_jack_sys_path jack_sys_path;
    char **linked_ports;
} pa_qahw_jack_in_config;

typedef pa_hook_result_t (* pa_qahw_jack_callback_t) (void *dummy __attribute__((unused)), pa_qahw_jack_event_data_t *event_data, void *client_data);

pa_qahw_jack_handle_t *pa_qahw_jack_register_event_callback(pa_qahw_jack_type_t jack_type, pa_qahw_jack_callback_t callback, pa_module *m,
                         pa_qahw_jack_in_config *jack_in_config, void *client_data, bool is_external, qahw_module_handle_t *module_handle);
bool pa_qahw_jack_deregister_event_callback(pa_qahw_jack_handle_t *handle, pa_module *m, bool is_external);

#endif
