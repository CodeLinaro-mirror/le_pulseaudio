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
#include <pulsecore/core-util.h>

#include "qahw-sink.h"

#define QAHW_EFFECT_OBJECT_PATH "/org/pulseaudio/core1/effect"
#define QAHW_EFFECT_MODULE_IFACE "org.PulseAudio.Core1.Effect"
#define QAHW_EFFECT_SESSION_IFACE "org.PulseAudio.Core1.Effect.Session"

typedef struct {
    char *name;
    char *description;
    char *type;
    pa_hashmap *sinks;
    pa_hashmap *ports;

    char **endpoint_conf_string;
} pa_qahw_effect_config;

typedef void* pa_qahw_effect_handle_t;

static inline bool pa_qahw_effect_is_supported_type(char *effect_type) {
    pa_assert(effect_type);

    if (pa_streq(effect_type, "port") || pa_streq(effect_type, "sink"))
        return true;

    return false;
}

pa_qahw_effect_handle_t pa_qahw_init_effect(char *dbus_path,
                                            pa_dbus_protocol *dbus_protocol,
                                            pa_hashmap *effects,
                                            pa_card *card);

void pa_qahw_deinit_effect(pa_qahw_effect_handle_t effect_handle);

void pa_qahw_free_sink_effects(pa_qahw_effect_handle_t handle, uint32_t sink_id);

#endif
