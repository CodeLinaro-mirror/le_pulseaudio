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

#ifndef fooqahwpaloopbackhfoo
#define fooqahwpaloopbackhfoo

#include <pulsecore/core.h>
#include <pulsecore/card.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-effect.h"

typedef struct {
    char *name;
    char *description;

    pa_hashmap *in_ports;
    char **in_port_conf_string;

    pa_hashmap *out_ports;
    char **out_port_conf_string;
} pa_qahw_loopback_config;

typedef enum {
    PA_QAHW_LOOPBACK_EVENT_INVALID,
    PA_QAHW_LOOPBACK_EVENT_STARTED,
    PA_QAHW_LOOPBACK_EVENT_STOPPED,
    PA_QAHW_LOOPBACK_EVENT_FORMAT_UPDATE,
    PA_QAHW_LOOPBACK_EVENT_RECREATED,
} pa_qahw_loopback_event_t;

typedef void (* pa_qahw_loopback_callback_t)(const char *port_name, pa_qahw_loopback_event_t event, void *prv_data);
char *pa_qahw_loopback_get_name_from_handle(audio_patch_handle_t handle);

void pa_qahw_loopback_init(qahw_module_handle_t *module_handle, pa_core *core, pa_card *card,
                           pa_hashmap *loopbacks, pa_qahw_loopback_callback_t callback,
                           void *prv_data, pa_qahw_effect_handle_t effect_handle, pa_module *m);

void pa_qahw_loopback_deinit(void);

#endif
