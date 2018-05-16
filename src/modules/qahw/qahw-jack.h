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

#ifndef fooqahwjackhfoo
#define fooqahwjackhfoo

#include <pulsecore/module.h>

typedef enum {
    PA_QAHW_JACK_TYPE_INVALID = -1,
    PA_QAHW_JACK_TYPE_WIRED_HEADSET = 0x1,
    PA_QAHW_JACK_TYPE_WIRED_HEADPHONE = 0x2,
    PA_QAHW_JACK_TYPE_LINEOUT = 0x4,
    PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS = 0x8,
    PA_QAHW_JACK_TYPE_MAX = 0x0F,
} pa_qahw_jack_type_t;

typedef enum {
    PA_QAHW_JACK_ERROR,
    PA_QAHW_JACK_AVAILABLE,
    PA_QAHW_JACK_UNAVAILABLE,
} pa_qahw_jack_event_t;

typedef struct pa_qahw_jack_event_data {
    pa_qahw_jack_type_t jack_type;
    void *pa_qahw_jack_info; /* can be used to send any info related to a jack */
} pa_qahw_jack_event_data_t;

typedef size_t pa_qahw_jack_handle_t;

typedef void (*pa_qahw_jack_callback_t)(pa_qahw_jack_event_t event, pa_qahw_jack_event_data_t *event_data, void *priv_data);
int pa_qahw_jack_enable(pa_module *m, pa_qahw_jack_type_t jtype, pa_qahw_jack_callback_t callback, pa_qahw_jack_handle_t **handle, void *prv_data);
void pa_qahw_jack_disable(pa_qahw_jack_handle_t *handle);

#endif
