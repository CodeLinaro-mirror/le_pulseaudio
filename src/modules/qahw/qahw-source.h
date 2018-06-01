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

#ifndef fooqahwpasourcehfoo
#define fooqahwpasourcehfoo

#include <stdio.h>

#include <pulse/timeval.h>
#include <pulsecore/sink.h>
#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sink.h>
#include <pulsecore/memchunk.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-source-extn.h"

typedef size_t source_handle_t;

struct qahw_source_data {
    qahw_stream_handle_t *in_handle;
    audio_io_handle_t handle;
    qahw_module_handle_t *module_handle;

    uint32_t devices;
    audio_input_flags_t flags;
    audio_config_t config;

    const char *device_url;

    size_t source_buffer_size;
};

struct pa_source_data {
    bool first;
    pa_source *source;
    pa_rtpoll *rtpoll;
    pa_thread_mq thread_mq;
    pa_thread *thread;
};

struct source_data {
    struct qahw_source_data *qahw_sdata;
    struct pa_source_data *pa_sdata;
    source_extn_handle_t *source_extn_handle;
    struct userdata *u;
};

/*create qahw session and pa source */
int create_source(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name,
                 const char *profile_name, pa_sample_spec *ss, pa_channel_map *map, uint32_t source_devices, int32_t flags,int source_idx,
                 source_handle_t **handle);
void close_source(source_handle_t *handle);

#endif
