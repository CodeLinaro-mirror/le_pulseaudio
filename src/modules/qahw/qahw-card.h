/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
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

#ifndef foomoduleqahwcardfoo
#define foomoduleqahwcardfoo

typedef enum {
    PA_QAHW_CARD_SINK_NONE= 0x0,
    PA_QAHW_CARD_SINK_LL_0 = 0x1,
    PA_QAHW_CARD_SINK_ULL_0 = 0x2,
    PA_QAHW_CARD_SINK_OFFLOAD_0 = 0x4,
} pa_qahw_card_sink_usecase_id_t;

typedef enum {
    PA_QAHW_CARD_SOURCE_NONE = 0x0,
    PA_QAHW_CARD_SOURCE_REGULAR_0 = 0x1,
    PA_QAHW_CARD_SOURCE_LL_0 = 0x2,
    PA_QAHW_CARD_SOURCE_REGULAR_1 = 0x4,
} pa_qahw_card_source_usecase_id_t;

typedef enum {
    PA_QAHW_CARD_AVOID_PROCESSING_FOR_NONE = 0x0,
    PA_QAHW_CARD_AVOID_PROCESSING_FOR_SAMPLE_RATE = 0x1,
    PA_QAHW_CARD_AVOID_PROCESSING_FOR_BIT_WIDTH = 0x2,
    PA_QAHW_CARD_AVOID_PROCESSING_FOR_CHANNELS = 0x4,
    PA_QAHW_CARD_AVOID_PROCESSING_FOR_ALL = (PA_QAHW_CARD_AVOID_PROCESSING_FOR_SAMPLE_RATE |
                                             PA_QAHW_CARD_AVOID_PROCESSING_FOR_BIT_WIDTH |
                                             PA_QAHW_CARD_AVOID_PROCESSING_FOR_CHANNELS),
} pa_qahw_card_avoid_processing_config_id_t;

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
} pa_qahw_card_profile_config;

typedef struct {
    char *name;
    char *description;
    pa_available_t available;
    pa_direction_t direction;
    uint32_t priority;
    audio_devices_t device;

    pa_idxset *formats;
    bool format_detection;
    char *port_type;
    char **linked_ports;
    char *primary_port_name;

    char *state_node_path;
    char *sample_format_node_path;
    char *sample_rate_node_path;
    char *sample_layout_node_path;
    char *sample_channel_node_path;
    char *sample_channel_alloc_node_path;

    char *linkon0_node_path;
    char *poweron_node_path;
    char *audio_path_node_path;
    char *arc_enable_node_path;

    char *arc_state_node_path;
    char *arc_sample_format_node_path;
    char *arc_sample_rate_node_path;
    char *bus;
} pa_qahw_card_port_config;

typedef union {
    pa_qahw_card_source_usecase_id_t source_id;
    pa_qahw_card_sink_usecase_id_t sink_id;
} pa_qahw_card_usecase_id_t;

typedef enum {
    PA_QAHW_CARD_USECASE_TYPE_STATIC = 0,
    PA_QAHW_CARD_USECASE_TYPE_DYNAMIC = 1,
} pa_qahw_card_usecase_type_t;

typedef struct {
    audio_devices_t device;
    pa_qahw_card_usecase_id_t usecase_id;
} pa_qahw_card_port_device_data;

#endif
