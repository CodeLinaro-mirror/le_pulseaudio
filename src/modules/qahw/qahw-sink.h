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

#ifndef fooqahwpasinkfoo
#define fooqahwpasinkfoo

#include <pulsecore/device-port.h>
#include <pulse/sample.h>
#include <pulsecore/card.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-card.h"
#include "qahw-sink-extn.h"

typedef struct {
    char *name;
    char *description;
    char *type;
    int id;
    audio_output_flags_t flags;
    bool use_hw_volume;
    pa_sample_spec default_spec;
    pa_encoding_t default_encoding;
    pa_channel_map default_map;
    uint32_t alternate_sample_rate;
    bool avoid_processing;

    pa_idxset *formats;
    pa_hashmap *ports;
    pa_hashmap *profiles;
    char **port_conf_string;
    pa_qahw_card_usecase_type_t usecase_type;
    int32_t buffer_duration;
} pa_qahw_sink_config;

typedef size_t pa_qahw_sink_handle_t;

bool pa_qahw_sink_is_primary(audio_output_flags_t flags);
pa_idxset* pa_qahw_sink_get_config(pa_qahw_sink_handle_t *handle);
audio_io_handle_t pa_qahw_sink_get_io_handle(uint32_t sink_id);
bool pa_qahw_sink_is_supported_sample_rate(uint32_t sample_rate);
int pa_qahw_sink_get_index(pa_qahw_sink_handle_t *handle);
char *pa_qahw_sink_get_name_from_pa_sink_id(uint32_t sink_id);
/* create qahw session and pa sink */
int pa_qahw_sink_create(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name, pa_qahw_sink_config *sink,
                        pa_qahw_sink_handle_t **handle);
void pa_qahw_sink_close(pa_qahw_sink_handle_t *handle);
void pa_qahw_sink_module_init(void);
void pa_qahw_sink_module_deinit(void);
/* function to get media config */
int pa_qahw_sink_get_media_config(pa_qahw_sink_handle_t *handle, pa_sample_spec *ss, pa_channel_map *map, pa_encoding_t *encoding);


static inline bool pa_qahw_sink_is_supported_type(char *sink_type) {
    pa_assert(sink_type);

    if (pa_streq(sink_type, "ultra-low-latency") ||  pa_streq(sink_type, "low-latency")
        || pa_streq(sink_type, "offload") || pa_streq(sink_type, "primary"))
        return true;

    return false;
}

static inline bool pa_qahw_sink_is_supported_sample_format(char *sink_type) {
    pa_assert(sink_type);

    if (pa_streq(sink_type, "s16le") ||  pa_streq(sink_type, "s24le"))
        return true;

    return false;
}

static inline bool pa_qahw_sink_is_supported_encoding(pa_encoding_t encoding) {
    bool supported = true;

    switch (encoding) {
        case PA_ENCODING_PCM:
        case PA_ENCODING_MPEG:
        case PA_ENCODING_AAC:
            break;

        default :
            supported = false;
            pa_log_error("%s: unsupported encoding %s", __func__, pa_encoding_to_string(encoding));
    }

    return supported;
}

static inline audio_output_flags_t pa_qahw_sink_get_flags_from_string(const char *flag_name) {
    audio_output_flags_t flag;

    if (pa_streq(flag_name, "AUDIO_OUTPUT_FLAG_FAST")) {
        flag = AUDIO_OUTPUT_FLAG_FAST;
    } else if (pa_streq(flag_name, "AUDIO_OUTPUT_FLAG_PRIMARY")) {
        flag = AUDIO_OUTPUT_FLAG_PRIMARY;
    } else if (pa_streq(flag_name, "AUDIO_OUTPUT_FLAG_RAW")) {
        flag = AUDIO_OUTPUT_FLAG_RAW;
    } else if (pa_streq(flag_name, "AUDIO_OUTPUT_FLAG_DEEP_BUFFER")) {
        flag = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
    } else if (pa_streq(flag_name, "AUDIO_OUTPUT_FLAG_DIRECT")) {
        flag = AUDIO_OUTPUT_FLAG_DIRECT;
    } else if (pa_streq(flag_name, "AUDIO_OUTPUT_FLAG_DIRECT_PCM")) {
        flag = AUDIO_OUTPUT_FLAG_DIRECT_PCM;
    } else if (pa_streq(flag_name, "AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD")) {
        flag = AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD;
    } else if (pa_streq(flag_name, "AUDIO_OUTPUT_FLAG_NON_BLOCKING")) {
        flag = AUDIO_OUTPUT_FLAG_NON_BLOCKING;
    } else {
        flag = AUDIO_OUTPUT_FLAG_NONE;
        pa_log_error("%s: Unsupported flag_name %s", __func__, flag_name);
    }

    return flag;
}

#endif
