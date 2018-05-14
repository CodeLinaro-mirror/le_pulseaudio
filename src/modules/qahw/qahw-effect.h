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

#ifndef fooqahwpaeffectfoo
#define fooqahwpaeffectfoo

#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>
#include "qahw-sink.h"

#define QAHW_EFFECT_OBJECT_PATH "/org/pulseaudio/core1/effect"
#define QAHW_EFFECT_MODULE_IFACE "org.PulseAudio.Core1.Effect"
#define QAHW_EFFECT_SESSION_IFACE "org.PulseAudio.Core1.Effect.Session"

typedef enum {
    PA_QAHW_EFFECT_BASSBOOST = 0,
    PA_QAHW_EFFECT_VIRTUALIZER,
    PA_QAHW_EFFECT_EQUALIZER,
    PA_QAHW_EFFECT_PRESET_REVERB,
    PA_QAHW_EFFECT_AUDIOSPHERE,
    PA_QAHW_EFFECT_MAX
} pa_qahw_effect_t;

typedef struct {
    int flags;
    bool effect_supported[PA_QAHW_EFFECT_MAX];
} pa_qahw_effect_data;

typedef struct {
    const char *port_name;
    bool effect_supported[PA_QAHW_EFFECT_MAX];
} pa_qahw_port_effect_data;

typedef struct {
    uint32_t sink_id;
    sink_handle_t *handle;
    bool effect_loaded[PA_QAHW_EFFECT_MAX];
} pa_qahw_effect_status;

typedef void* pa_qahw_effect_handle_t;

void pa_qahw_free_sink_effects(pa_qahw_effect_handle_t handle, uint32_t sink_id);
pa_qahw_effect_handle_t pa_qahw_init_effect(char *dbus_path, pa_dbus_protocol *dbus_protocol, pa_qahw_effect_data *sink_effects,
                                            pa_qahw_port_effect_data *port_effects, pa_qahw_effect_status *status, pa_card *card,
                                            uint32_t max_sinks, uint32_t max_ports);
void pa_qahw_deinit_effect(pa_qahw_effect_handle_t handle);

#endif
