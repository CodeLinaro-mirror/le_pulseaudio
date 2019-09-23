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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <pulse/rtclock.h>
#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/source.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/core-format.h>
#include <pulse/util.h>

#include "qal-source.h"
#include "qal-utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>

#define PA_ALTERNATE_SOURCE_RATE 44100
#define PA_FORMAT_DEFAULT_SAMPLE_RATE_INDEX 0
#define PA_FORMAT_DEFAULT_SAMPLE_FORMAT_INDEX 0
#define PA_DEFAULT_SOURCE_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_SOURCE_RATE 48000
#define PA_DEFAULT_SOURCE_CHANNELS 2

//#define SOURCE_DUMP_ENABLED

typedef struct {
    qal_stream_handle_t *stream_handle;

    struct qal_device *qal_device;
    struct qal_stream_attributes *stream_attributes;
    const char *device_url;

    int write_fd;

    size_t buffer_size;
    size_t buffer_count;
    int index;

    bool standby;
} qal_source_data;

typedef struct {
    bool first;
    pa_source *source;
    pa_rtpoll *rtpoll;
    pa_thread_mq thread_mq;
    pa_thread *thread;
    pa_idxset *formats;
} pa_source_data;

typedef struct {
    qal_source_data *qal_sdata;
    pa_source_data *pa_sdata;
} pa_qal_source_data;

static int restart_qal_source(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, pa_qal_card_port_device_data *port_device_data, qal_stream_type_t type,
                              int source_id, qal_source_data *qal_sdata, uint32_t buffer_size, uint32_t buffer_count);
static int create_qal_source(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, pa_qal_card_port_device_data *port_device_data, qal_stream_type_t type,
                             int source_id, pa_qal_source_data *sdata, uint32_t buffer_size, uint32_t buffer_count);
static int close_qal_source(qal_source_data *qal_sdata);

static const uint32_t supported_source_rates[] =
                          {8000, 11025, 16000, 22050, 44100, 48000, 96000, 192000};

static const char *pa_qal_source_get_name_from_type(qal_stream_type_t type) {
    const char *name = NULL;

    if (type == QAL_STREAM_RAW)
        name = "regular";
    else if (type == QAL_STREAM_LOW_LATENCY)
        name = "low-latency";
    else if (type == QAL_STREAM_COMPRESSED)
        name = "compress";

    return name;
}

    //TODO: Format Hardcoded as of now
static int pa_qal_source_fill_info(qal_source_data *qal_sdata, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, pa_qal_card_port_device_data *port_device_data,
                                    qal_stream_type_t type, int source_id, uint32_t buffer_size, uint32_t buffer_count) {
    uint32_t channel_count = 0;
    pa_assert(qal_sdata);

    qal_sdata->stream_attributes = pa_xnew0(struct qal_stream_attributes, 1);

    qal_sdata->stream_attributes->type = type;
    qal_sdata->stream_attributes->info.opt_stream_info.version = 1;
    qal_sdata->stream_attributes->info.opt_stream_info.duration_us = -1;
    qal_sdata->stream_attributes->info.opt_stream_info.has_video = false;
    qal_sdata->stream_attributes->info.opt_stream_info.is_streaming = false;

    qal_sdata->stream_attributes->flags = 0;
    qal_sdata->stream_attributes->direction = QAL_AUDIO_INPUT;

    qal_sdata->stream_attributes->in_media_config.sample_rate = ss->rate;
    qal_sdata->stream_attributes->in_media_config.bit_width = 16;
    qal_sdata->stream_attributes->in_media_config.aud_fmt_id = 0;

    channel_count = pa_qal_get_channel_count(map);
    qal_sdata->stream_attributes->in_media_config.ch_info = (struct qal_channel_info *)malloc(sizeof(uint16_t) + sizeof(uint8_t)*channel_count);
    if (!pa_qal_channel_map_to_qal(map, qal_sdata->stream_attributes->in_media_config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        pa_xfree(qal_sdata->stream_attributes->in_media_config.ch_info);
        return -1;
    }

    qal_sdata->qal_device = pa_xnew0(struct qal_device, 1);
    memset(qal_sdata->qal_device, 0, sizeof(struct qal_device));
    qal_sdata->qal_device->id = port_device_data->device;
    qal_sdata->qal_device->config.sample_rate = port_device_data->default_spec.rate;
    qal_sdata->qal_device->config.bit_width = 16;
    channel_count = pa_qal_get_channel_count(&port_device_data->default_map);
    qal_sdata->qal_device->config.ch_info = (struct qal_channel_info *) malloc(sizeof(uint16_t) + sizeof(uint8_t)*channel_count);
    if (!pa_qal_channel_map_to_qal(map, qal_sdata->qal_device->config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        pa_xfree(qal_sdata->qal_device->config.ch_info);
        return -1;
    }

    qal_sdata->device_url = NULL; /* TODO: useful for BT devices */
    qal_sdata->index = source_id;
    qal_sdata->buffer_size = (size_t)buffer_size;
    qal_sdata->buffer_count = (size_t)buffer_count;

    qal_sdata->standby = true;

    return 0;
}

static int pa_qal_source_start(qal_source_data *qal_sdata) {
    int rc = 0;
    pa_assert(qal_sdata);
    pa_log_debug("%s", __func__);

    if (qal_sdata->standby) {
        rc = qal_stream_start(qal_sdata->stream_handle);
        pa_log_debug("qal_stream_start returned %d", rc);
        qal_sdata->standby = false;
    } else {
        pa_log_debug("qal_stream already started");
    }
    return rc;
}

static int pa_qal_source_standby(qal_source_data *qal_sdata) {
    int rc = 0;

    pa_assert(qal_sdata);
    pa_assert(qal_sdata->stream_handle);

    pa_log_debug("%s",__func__);

    if (!qal_sdata->standby) {
        rc = qal_stream_stop(qal_sdata->stream_handle);
        pa_log_debug("qal_stream_stop returned %d\n", rc);
        qal_sdata->standby = true;
    } else {
        pa_log_debug("qal_stream already in standby");
    }

    return 0;
}

static int pa_qal_source_set_state_in_io_thread_cb(pa_source *s, pa_source_state_t new_state, pa_suspend_cause_t new_suspend_cause PA_GCC_UNUSED)
{
    pa_qal_source_data *source_data = NULL;
    int r = 0;

    pa_assert(s);
    pa_assert(s->userdata);

    source_data = (pa_qal_source_data *)(s->userdata);

    pa_log_debug("New state is: %d", new_state);

    if (PA_SOURCE_IS_OPENED(new_state) && !PA_SOURCE_IS_OPENED(s->thread_info.state))
        r = pa_qal_source_start(source_data->qal_sdata);
    else if (new_state == PA_SOURCE_SUSPENDED)
        r = pa_qal_source_standby(source_data->qal_sdata);

    return r;
}

static int pa_qal_source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    pa_qal_source_data *source_data = NULL;

    pa_assert(o);

    source_data = (pa_qal_source_data *)(PA_SOURCE(o)->userdata);

    pa_assert(source_data);
    pa_assert(source_data->pa_sdata->source);

    switch (code) {
        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            *((pa_usec_t*) data) = 0;
            return 0;
        }

        default:
             break;
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static int pa_qal_source_reconfigure_cb(pa_source *s, pa_sample_spec *spec, pa_channel_map *map, bool passthrough) {
    pa_qal_source_data *sdata = NULL;
    pa_source_data *pa_sdata = NULL;
    qal_source_data *qal_sdata = NULL;
    pa_qal_card_port_device_data *port_device_data = NULL;
    pa_channel_map new_map;

    bool supported = false;
    uint32_t i;
    int rc = 0;
    uint32_t old_rate;

    pa_assert(s);

    sdata = (pa_qal_source_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qal_sdata);

    pa_sdata = sdata->pa_sdata;
    qal_sdata = sdata->qal_sdata;

    for (i = 0; i < ARRAY_SIZE(supported_source_rates) ; i++) {
        if (spec->rate == supported_source_rates[i]) {
            supported = true;
            break;
        }
    }

    if (!supported) {
        pa_log_info("Source does not support sample rate of %d Hz", spec->rate);
        return -1;
    }

    if (!PA_SOURCE_IS_OPENED(s->state)) {
        if (map)
            new_map = *map;
        else
            pa_channel_map_init_auto(&new_map, spec->channels, PA_CHANNEL_MAP_DEFAULT);

        old_rate = pa_sdata->source->sample_spec.rate; /*take backup*/
        pa_sdata->source->sample_spec.rate = spec->rate;

        port_device_data = PA_DEVICE_PORT_DATA(pa_sdata->source->active_port);
        rc = restart_qal_source(PA_ENCODING_PCM, &pa_sdata->source->sample_spec, &pa_sdata->source->channel_map, port_device_data,
                                qal_sdata->stream_attributes->type, qal_sdata->index, qal_sdata, qal_sdata->buffer_size, qal_sdata->buffer_count);
        if (PA_UNLIKELY(rc)) {
            pa_sdata->source->sample_spec.rate = old_rate; /*restore old rate if failed*/
            pa_log_error("Could create reopen qal source, error %d", rc);
            return -1;
        }

        pa_sdata->source->sample_spec = *spec;
        pa_sdata->source->channel_map = new_map;

        pa_source_set_fixed_latency(pa_sdata->source, pa_bytes_to_usec(qal_sdata->buffer_size, &s->sample_spec));
        return 0;
    }

    return rc;
}

static pa_idxset* pa_qal_source_get_formats(pa_source *s) {
    pa_qal_source_data *sdata = NULL;

    pa_assert(s);

    sdata = (pa_qal_source_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);

    return pa_idxset_copy(sdata->pa_sdata->formats, (pa_copy_func_t) pa_format_info_copy);
}

static void pa_qal_source_thread_func(void *userdata) {
    pa_qal_source_data *source_data = (pa_qal_source_data *)userdata;
    pa_source_data *pa_sdata = NULL;
    qal_source_data *qal_sdata = NULL;

    pa_assert(source_data);

    pa_sdata = source_data->pa_sdata;
    qal_sdata = source_data->qal_sdata;

    pa_assert(pa_sdata);
    pa_assert(qal_sdata);

    pa_log_debug("Source IO Thread starting up");

    pa_thread_mq_install(&pa_sdata->thread_mq);

    for (;;) {
        int ret;
        bool wait = true;

        if (PA_SOURCE_IS_OPENED(pa_sdata->source->thread_info.state)) {
            pa_memchunk chunk;
            void *data;
            struct qal_buffer in_buf;

            memset(&in_buf, 0, sizeof(struct qal_buffer));

            chunk.memblock = pa_memblock_new(pa_sdata->source->core->mempool, qal_sdata->buffer_size);
            data = pa_memblock_acquire(chunk.memblock);
            chunk.length = pa_memblock_get_length(chunk.memblock);
            chunk.index = 0;

            in_buf.buffer = data;
            in_buf.size = chunk.length;

            if ((ret = qal_stream_read(qal_sdata->stream_handle, &in_buf)) <= 0) {
                pa_log_error("qal_stream_read failed, ret = %d", ret);
                pa_msleep(pa_bytes_to_usec(in_buf.size, &pa_sdata->source->sample_spec)/1000);
                ret = in_buf.size;
            }

            chunk.length = ret;
#ifdef SOURCE_DUMP_ENABLED
            pa_log_error(" chunk length %d chunk index %d in_buf.size %d ",chunk.length, chunk.index, ret);
            if ((ret = write(qal_sdata->write_fd, in_buf.buffer, ret)) < 0)
                    pa_log_error("write to fd failed %d", ret);
#endif
            /* FIXME: don't post if read fails */
            pa_memblock_release(chunk.memblock);
            pa_source_post(pa_sdata->source, &chunk);
            pa_memblock_unref(chunk.memblock);

            wait = false;
        }

        /* nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(pa_sdata->rtpoll, wait)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(pa_sdata->thread_mq.outq, PA_MSGOBJECT(pa_sdata->source->core), PA_CORE_MESSAGE_UNLOAD_MODULE, pa_sdata->source->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(pa_sdata->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Source IO Thread shutting down");
}

static int open_qal_source(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, pa_qal_card_port_device_data *port_device_data, qal_stream_type_t type,
                           int source_id, qal_source_data *qal_sdata, uint32_t buffer_size, uint32_t buffer_count) {
    int rc;
#ifdef SOURCE_DUMP_ENABLED
    char *file_name;
#endif

    size_t out_buffer_size;

    pa_assert(ss);
    pa_assert(map);
    pa_assert(qal_sdata);

    pa_qal_source_fill_info(qal_sdata, encoding, ss, map, port_device_data, type, source_id, buffer_size, buffer_count);

#ifdef SOURCE_DUMP_ENABLED
    file_name = pa_sprintf_malloc("/data/pcmdump_source_%d", qal_sdata->index);

    qal_sdata->write_fd = open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
    if(qal_sdata->write_fd < 0)
        pa_log_error("Could not open write fd %d for source index %d", qal_sdata->write_fd, qal_sdata->index);

    pa_xfree(file_name);
#endif

    pa_log_debug("opening source with configuration flag = 0x%x, encoding %d,format %d, sample_rate %d",
                 qal_sdata->stream_attributes->type, encoding, qal_sdata->stream_attributes->in_media_config.aud_fmt_id,
                 qal_sdata->stream_attributes->in_media_config.sample_rate);

    rc = qal_stream_open(qal_sdata->stream_attributes, 1, qal_sdata->qal_device, 0, NULL, NULL, NULL,
                             &qal_sdata->stream_handle);
    if (rc) {
        qal_sdata->stream_handle = NULL;
        pa_log_error("Could not open input stream %d", rc);
        goto fail;
    }

    pa_log_debug("qal source opened %p", qal_sdata->stream_handle);

    /* FIXME: Update it by calling qal_stream_get_buffer_size */
    pa_log_debug("buffer size is %zu, buffer count is %zu\n", qal_sdata->buffer_size, qal_sdata->buffer_count);
    rc = qal_stream_set_buffer_size(qal_sdata->stream_handle, &qal_sdata->buffer_size, qal_sdata->buffer_count, &out_buffer_size, 0);
    if(rc) {
        pa_log_error("qal_stream_set_buffer_size failed\n");
    }

fail:
    return rc;
}

static int close_qal_source(qal_source_data *qal_sdata) {
    int rc = -1;

    pa_assert(qal_sdata);
    pa_assert(qal_sdata->stream_handle);

    pa_log_debug("closing qal source %p", qal_sdata->stream_handle);

    if (PA_UNLIKELY(qal_sdata->stream_handle == NULL)) {
        pa_log_error("Invalid source handle %p", qal_sdata->stream_handle);
    } else {
        rc = qal_stream_close(qal_sdata->stream_handle);
        if (PA_UNLIKELY(rc)) {
            pa_log_error(" could not close source handle %p, error  %d", qal_sdata->stream_handle, rc);
        }

        qal_sdata->stream_handle = NULL;
    }
#ifdef SOURCE_DUMP_ENABLED
    close(qal_sdata->write_fd);
#endif

    return rc;
}

static int restart_qal_source(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, pa_qal_card_port_device_data *port_device_data, qal_stream_type_t type,
                              int source_id, qal_source_data *qal_sdata, uint32_t buffer_size, uint32_t buffer_count) {
    int rc;

    rc = close_qal_source(qal_sdata);
    if (rc) {
        pa_log_error("close_qal_source failed, error %d", rc);
        goto exit;
    }

    rc = open_qal_source(encoding, ss, map, port_device_data, type, source_id, qal_sdata, buffer_size, buffer_count);
    if (rc) {
        pa_log_error("open_qal_source failed during recreation, error %d", rc);
    }

exit:
    return rc;
}

static int free_qal_source(qal_source_data *qal_sdata) {
    int rc;

    rc = close_qal_source(qal_sdata);
    if (rc) {
        pa_log_error("close_qal_source failed, error %d", rc);
    }

    pa_xfree(qal_sdata->stream_attributes->in_media_config.ch_info);
    pa_xfree(qal_sdata->stream_attributes);
    pa_xfree(qal_sdata->qal_device->config.ch_info);
    pa_xfree(qal_sdata->qal_device);
    pa_xfree(qal_sdata);
    qal_sdata = NULL;

    return rc;
}

static int create_qal_source(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, pa_qal_card_port_device_data *port_device_data, qal_stream_type_t type,
                             int source_id, pa_qal_source_data *sdata, uint32_t buffer_size, uint32_t buffer_count) {
    int rc;

    sdata->qal_sdata = pa_xnew0(qal_source_data, 1);

    rc = open_qal_source(encoding, ss, map, port_device_data, type, source_id, sdata->qal_sdata, buffer_size, buffer_count);
    if (rc) {
        pa_log_error("open_qal_source failed, error %d", rc);
        pa_xfree(sdata->qal_sdata);
        sdata->qal_sdata = NULL;
    }

    rc = pa_qal_source_start(sdata->qal_sdata);
    if (rc) {
        pa_log_error("qal stream start failed, error %d", rc);
        pa_xfree(sdata->qal_sdata);
        sdata->qal_sdata = NULL;
        return rc;
    }

    return rc;
}

static int create_pa_source(pa_module *m, char *source_name, char *description, pa_idxset *formats, pa_sample_spec *ss, pa_channel_map *map, uint32_t alternate_sample_rate, pa_card *card,
                            pa_hashmap *ports, const char *driver, pa_qal_source_data *source_data) {
    pa_source_new_data new_data;
    pa_source_data *pa_sdata = NULL;
    qal_source_data *qal_sdata = NULL;

    pa_device_port *port;
    pa_format_info *format;
    pa_format_info *in_format;
    void *state;
    uint32_t i;

    bool port_source_mapping = false;

    pa_assert(source_data->qal_sdata);

    qal_sdata = source_data->qal_sdata;
    pa_sdata = pa_xnew0(pa_source_data, 1);
    pa_source_new_data_init(&new_data);
    new_data.driver = driver;
    new_data.module = m;
    new_data.card = card;

    source_data->pa_sdata = pa_sdata;

    pa_sdata->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&pa_sdata->thread_mq, m->core->mainloop, pa_sdata->rtpoll);

    pa_source_new_data_set_name(&new_data, source_name);

    pa_log_info("ss->rate %d ss->channels %d", ss->rate, ss->channels);
    pa_source_new_data_set_sample_spec(&new_data, ss);
    pa_source_new_data_set_channel_map(&new_data, map);
    if (alternate_sample_rate == PA_ALTERNATE_SOURCE_RATE)
        pa_source_new_data_set_alternate_sample_rate(&new_data, PA_ALTERNATE_SOURCE_RATE);
    else if (alternate_sample_rate > 0)
        pa_log_error("%s: unsupported alternate sample rate %d",__func__, alternate_sample_rate);

    /* associate port with source */
    PA_HASHMAP_FOREACH(port, ports, state) {
        pa_log_debug("adding port %s to source %s", port->name, source_name);
        pa_assert_se(pa_hashmap_put(new_data.ports, port->name, port) == 0);
        port_source_mapping = true;
        pa_device_port_ref(port);
    }

   if (!port_source_mapping) {
        pa_log_error("%s: source %s creation failed as no port mapped, ",__func__, source_name);
        goto fail;
    }

    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_STRING, pa_qal_source_get_name_from_type(qal_sdata->stream_attributes->type));
    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_DESCRIPTION, description);

    pa_sdata->source = pa_source_new(m->core, &new_data, PA_SOURCE_HARDWARE);
    if (!pa_sdata->source) {
        pa_log_error("Could not create source");
        goto fail;
    }

    pa_log_info("pa source opened %p", pa_sdata->source);
    pa_source_new_data_done(&new_data);

    pa_sdata->source->userdata = (void *)source_data;
    pa_sdata->source->parent.process_msg = pa_qal_source_process_msg;
    pa_sdata->source->set_state_in_io_thread = pa_qal_source_set_state_in_io_thread_cb;
    pa_sdata->source->set_port = NULL; //Need to update with set port callback function

    /* FIXME: check reconfigure needed for non pcm */
    pa_sdata->source->reconfigure = pa_qal_source_reconfigure_cb;

    if (pa_idxset_size(formats) > 0 ) {
        pa_sdata->source->get_formats = pa_qal_source_get_formats;

        pa_sdata->formats = pa_idxset_new(NULL, NULL);

        PA_IDXSET_FOREACH(in_format, formats, i) {
            format = pa_format_info_copy(in_format);
            pa_idxset_put(pa_sdata->formats, format, NULL);
        }
    }

    pa_source_set_asyncmsgq(pa_sdata->source, pa_sdata->thread_mq.inq);
    pa_source_set_rtpoll(pa_sdata->source, pa_sdata->rtpoll);
    pa_source_set_max_rewind(pa_sdata->source, 0);
    pa_source_set_fixed_latency(pa_sdata->source, pa_bytes_to_usec(qal_sdata->buffer_size, ss));

    pa_sdata->thread = pa_thread_new(source_name, pa_qal_source_thread_func, source_data);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

    pa_source_put(pa_sdata->source);

    return 0;

fail :
    if (pa_sdata->rtpoll)
        pa_rtpoll_free(pa_sdata->rtpoll);

    if (pa_sdata->source) {
        pa_source_new_data_done(&new_data);
        pa_source_unlink(pa_sdata->source);
        pa_source_unref(pa_sdata->source);
        pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);
    }

    pa_xfree(pa_sdata);
    source_data->pa_sdata = NULL;

    return -1;
}

static int free_pa_source(pa_source_data *pa_sdata) {
    pa_assert(pa_sdata);
    pa_assert(pa_sdata->source);
    pa_assert(pa_sdata->thread);
    pa_assert(pa_sdata->rtpoll);

    pa_log_debug("closing pa source %p", pa_sdata->source);

    pa_source_unlink(pa_sdata->source);

    pa_asyncmsgq_send(pa_sdata->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
    pa_thread_free(pa_sdata->thread);

    pa_source_unref(pa_sdata->source);

    if (pa_sdata->formats)
        pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);

    pa_thread_mq_done(&pa_sdata->thread_mq);

    pa_rtpoll_free(pa_sdata->rtpoll);

    pa_xfree(pa_sdata);

    return 0;
}

bool pa_qal_source_is_supported_sample_rate(uint32_t sample_rate) {
    bool supported = false;
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(supported_source_rates) ; i++) {
        if (sample_rate == supported_source_rates[i]) {
            supported = true;
            break;
        }
    }

    return supported;
}

int pa_qal_source_create(pa_module *m, pa_card *card, const char *driver, const char *module_name, pa_qal_source_config *source,
                          pa_qal_source_handle_t **handle) {
    int rc = -1;
    pa_qal_source_data *sdata;
    pa_device_port *card_port;
    pa_qal_card_port_config *source_port;
    pa_hashmap *ports;
    pa_qal_card_port_device_data *port_device_data;

    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    void *state;

    pa_assert(m);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module_name);
    pa_assert(source);
    pa_assert(source->name);
    pa_assert(source->description);
    pa_assert(source->formats);
    pa_assert(source->ports);

    if (pa_hashmap_isempty(source->ports)) {
        pa_log_error("%s: empty port list", __func__);
        goto exit;
    }

    /*convert config port to card port */
    ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    PA_HASHMAP_FOREACH(source_port, source->ports, state) {
        if ((card_port = pa_hashmap_get(card->ports, source_port->name)))
            pa_hashmap_put(ports, card_port->name, card_port);
    }

    /* first entry is default device */
    card_port = pa_hashmap_first(ports);
    port_device_data = PA_DEVICE_PORT_DATA(card_port);
    pa_assert(port_device_data);

    sdata = pa_xnew0(pa_qal_source_data, 1);

    pa_log_info("%s: creating source with ss %s buffer size %d buffer count %d", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &source->default_spec), source->buffer_size, source->buffer_count);

    rc = create_qal_source(source->default_encoding, &source->default_spec, &source->default_map, port_device_data, source->stream_type, source->id, sdata, source->buffer_size, source->buffer_count);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could not open qal source, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_pa_source(m, source->name, source->description, source->formats, &source->default_spec, &source->default_map, source->alternate_sample_rate, card, ports, driver, sdata);
    pa_hashmap_free(ports);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa source for source %s, error %d", source->name, rc);
        free_qal_source(sdata->qal_sdata);
        pa_xfree(sdata);
        sdata = NULL;
    }

    *handle = (pa_qal_source_handle_t *)sdata;

exit:
    return rc;
}

void pa_qal_source_close(pa_qal_source_handle_t *handle) {
    pa_qal_source_data *sdata = (pa_qal_source_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->qal_sdata);
    pa_assert(sdata->pa_sdata);

    free_pa_source(sdata->pa_sdata);
    free_qal_source(sdata->qal_sdata);
    pa_xfree(sdata);
}
