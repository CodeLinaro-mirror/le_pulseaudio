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

#ifndef fooqahwpasinkfoo
#define fooqahwpasinkfoo

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
#include <pulsecore/mutex.h>

#include <qahw_api.h>
#include <qahw_defs.h>

typedef size_t sink_handle_t;

struct qahw_sink_data {
    qahw_stream_handle_t *out_handle;
    audio_io_handle_t handle;
    qahw_module_handle_t *module_handle;

    audio_output_flags_t flags;
    uint32_t devices;
    audio_config_t config;

    const char *device_url;

    size_t sink_buffer_size;
    uint32_t sink_latency_us;
    pa_usec_t buffer_duration_us;
    uint64_t bytes_written;

    pa_atomic_t wait_for_write_ready;
    int write_fd;
};

struct pa_sink_data {
    bool first;
    pa_sink *sink;
    pa_rtpoll *rtpoll;
    pa_thread_mq thread_mq;
    pa_thread *thread;

    pa_rtpoll_item *rtpoll_item;
};

struct sink_data {
    struct qahw_sink_data *qahw_sdata;
    struct pa_sink_data *pa_sdata;
    struct userdata *u;

    pa_fdsem *fdsem; /* common resource between pa and qahw sink */
};

void deinit_sink(struct userdata *u);
void init_sink(struct userdata *u);

/* create qahw session and pa sink */
int create_sink(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name,
                 const char *profile_name, pa_sample_spec *ss, pa_channel_map *map, uint32_t sink_devices, int32_t flags,
                 int sink_idx, sink_handle_t **handle);
void close_sink(sink_handle_t *handle);

#endif
