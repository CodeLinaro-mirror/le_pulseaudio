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

#ifndef fooqalpasinkfoo
#define fooqalpasinkfoo

#include <pulsecore/device-port.h>
#include <pulse/sample.h>
#include <pulsecore/card.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>

#include <QalApi.h>
#include <QalDefs.h>

#include "qal-card.h"

typedef struct {
    char *name;
    char *description;
    int id;
    qal_stream_type_t stream_type;
    bool use_hw_volume;
    pa_sample_spec default_spec;
    pa_encoding_t default_encoding;
    pa_channel_map default_map;
    uint32_t alternate_sample_rate;
    pa_idxset *formats;
    pa_hashmap *ports;
    pa_hashmap *profiles;
    char **port_conf_string;
    pa_qal_card_usecase_type_t usecase_type;
} pa_qal_sink_config;

typedef size_t pa_qal_sink_handle_t;

bool pa_qal_sink_is_supported_sample_rate(uint32_t sample_rate);
/* create qal session and pa sink */
int pa_qal_sink_create(pa_module *m, pa_card *card, const char *driver, const char *module_name, pa_qal_sink_config *sink, pa_qal_sink_handle_t **handle);
void pa_qal_sink_close(pa_qal_sink_handle_t *handle);
void pa_qal_sink_module_init(void);
void pa_qal_sink_module_deinit(void);

static inline bool pa_qal_sink_is_supported_encoding(pa_encoding_t encoding) {
    bool supported = true;

    switch (encoding) {
        case PA_ENCODING_PCM:
            break;

        default :
            supported = false;
            pa_log_error("%s: unsupported encoding %s", __func__, pa_encoding_to_string(encoding));
    }

    return supported;
}

static inline qal_stream_type_t pa_qal_sink_get_type_from_string(const char *stream_type) {
    qal_stream_type_t type;

    if (pa_streq(stream_type, "QAL_STREAM_LOW_LATENCY")) {
        type = QAL_STREAM_LOW_LATENCY;
    } else if (pa_streq(stream_type,"QAL_STREAM_DEEP_BUFFER")) {
        type = QAL_STREAM_DEEP_BUFFER;
    } else if (pa_streq(stream_type, "QAL_STREAM_COMPRESSED")) {
        type = QAL_STREAM_COMPRESSED;
    } else {
        type = QAL_STREAM_GENERIC; //No QAL_STREAM_NONE. Hence using generic one.
        pa_log_error("%s: Unsupported stream_type %s", __func__, stream_type);
    }

    return type;
}

#endif
