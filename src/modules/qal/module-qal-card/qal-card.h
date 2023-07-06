/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
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

#ifndef foomodulepalcardfoo
#define foomodulepalcardfoo

typedef enum {
    PA_PAL_CARD_SINK_NONE= 0x0,
    PA_PAL_CARD_SINK_LL_0 = 0x1,
    PA_PAL_CARD_SINK_OFFLOAD_0 = 0x3,
} pa_pal_card_sink_usecase_id_t;

typedef enum {
    PA_PAL_CARD_SOURCE_NONE = 0x0,
    PA_PAL_CARD_SOURCE_LL_0 = 0x7,
} pa_pal_card_source_usecase_id_t;

typedef struct {
    char *name;
    char *description;

    uint32_t priority;
    pa_available_t available;

    pa_hashmap *ports;
    char **port_conf_string;

    uint32_t n_sinks;
    uint32_t n_sources;

    uint32_t max_sink_channels;
    uint32_t max_source_channels;
} pa_pal_card_profile_config;

typedef struct {
    char *name;
    char *description;
    pa_available_t available;
    pa_direction_t direction;
    pa_sample_spec default_spec;
    pa_channel_map default_map;
    uint32_t priority;
    pal_device_id_t device;

    pa_idxset *formats;
} pa_pal_card_port_config;

typedef union {
    pa_pal_card_source_usecase_id_t source_id;
    pa_pal_card_sink_usecase_id_t sink_id;
} pa_pal_card_usecase_id_t;

typedef enum {
    PA_PAL_CARD_USECASE_TYPE_STATIC = 0,
    PA_PAL_CARD_USECASE_TYPE_DYNAMIC = 1,
} pa_pal_card_usecase_type_t;

typedef struct {
    pal_device_id_t device;
    pa_pal_card_usecase_id_t usecase_id;
    pa_sample_spec default_spec;
    pa_channel_map default_map;
    bool is_connected;
} pa_pal_card_port_device_data;

#endif
