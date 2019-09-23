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

#ifndef fooqahwpasourcehfoo
#define fooqahwpasourcehfoo

#include <pulse/sample.h>
#include <pulsecore/card.h>
#include <pulsecore/core.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-card.h"

typedef size_t pa_qahw_source_handle_t;

typedef struct {
    char *name;
    char *description;
    char *type;
    int id;
    audio_input_flags_t flags;
    pa_sample_spec default_spec;
    pa_encoding_t default_encoding;
    pa_channel_map default_map;
    uint32_t alternate_sample_rate;
    pa_qahw_card_avoid_processing_config_id_t avoid_config_processing;
    pa_proplist *proplist;

    pa_idxset *formats;
    pa_hashmap *ports;
    pa_hashmap *profiles;
    char **port_conf_string;
    audio_source_t source_type;
    pa_qahw_card_usecase_type_t usecase_type;
    int32_t buffer_duration;
    int32_t preemph_status;
    uint32_t dsd_rate;
} pa_qahw_source_config;

/*create qahw session and pa source */
int pa_qahw_source_create(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name,
                          pa_qahw_source_config *source, pa_qahw_source_handle_t **handle);
pa_idxset* pa_qahw_source_get_config(pa_qahw_source_handle_t *handle);
void pa_qahw_source_close(pa_qahw_source_handle_t *handle);
bool pa_qahw_source_is_supported_sample_rate(uint32_t sample_rate);
/* function to get media config */
int pa_qahw_source_get_media_config(pa_qahw_source_handle_t *handle, pa_sample_spec *ss, pa_channel_map *map, pa_encoding_t *encoding);

static inline bool pa_qahw_source_is_supported_type(char *source_type) {
    pa_assert(source_type);

    if (pa_streq(source_type, "low-latency") || pa_streq(source_type, "regular") || pa_streq(source_type, "compress") || pa_streq(source_type, "passthrough"))
        return true;

    return false;
}

static inline bool pa_qahw_source_is_supported_sample_format(char *source_type) {
    pa_assert(source_type);

    if (pa_streq(source_type, "s16le") ||  pa_streq(source_type, "s24le"))
        return true;

    return false;
}

static inline bool pa_qahw_source_is_supported_encoding(pa_encoding_t encoding) {
    bool supported = true;

    switch (encoding) {
        case PA_ENCODING_PCM:
        case PA_ENCODING_UNKNOWN_IEC61937:
        case PA_ENCODING_UNKNOWN_4X_IEC61937:
        case PA_ENCODING_UNKNOWN_HBR_IEC61937:
        case PA_ENCODING_DSD:
            break;

        default :
            supported = false;
            pa_log_error("%s: unsupported encoding %s", __func__, pa_encoding_to_string(encoding));
    }

    return supported;
}

static inline audio_input_flags_t pa_qahw_source_get_flags_from_string(const char *flag_name) {
    audio_input_flags_t flag;

    if (pa_streq(flag_name, "AUDIO_INPUT_FLAG_NONE")) {
        flag = AUDIO_INPUT_FLAG_NONE;
    } else if (pa_streq(flag_name,"AUDIO_INPUT_FLAG_FAST")) {
        flag = AUDIO_INPUT_FLAG_FAST;
    } else if (pa_streq(flag_name, "QAHW_INPUT_FLAG_TIMESTAMP")) {
        flag = QAHW_INPUT_FLAG_TIMESTAMP;
    } else if (pa_streq(flag_name, "QAHW_INPUT_FLAG_COMPRESS")) {
        flag = QAHW_INPUT_FLAG_COMPRESS;
    } else if (pa_streq(flag_name, "QAHW_INPUT_FLAG_PASSTHROUGH")) {
        flag = QAHW_INPUT_FLAG_PASSTHROUGH;
    } else {
        flag = AUDIO_INPUT_FLAG_NONE;
        pa_log_error("%s: Unsupported flag name %s", __func__, flag_name);
    }

    return flag;
}

void pa_qahw_source_suspend(pa_qahw_source_handle_t *handle, bool suspend);
audio_source_t pa_qahw_source_name_to_enum(const char *source_name);
int pa_qahw_source_set_param(pa_qahw_source_handle_t *handle, const char *param);

#endif
