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
#include <fcntl.h>
#include <math.h>

#include <pulse/rtclock.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-format.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sink.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/mutex.h>

#include <sys/time.h>
#include <time.h>

#include "qal-sink.h"
#include "qal-utils.h"

/* #define SINK_DEBUG */

/* #define SINK_DUMP_ENABLED */

#define QAL_MAX_GAIN 1

#define PA_ALTERNATE_SINK_RATE 44100
#define PA_FORMAT_DEFAULT_SAMPLE_RATE_INDEX 0
#define PA_FORMAT_DEFAULT_SAMPLE_FORMAT_INDEX 0
#define PA_DEFAULT_SINK_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_SINK_RATE 48000
#define PA_DEFAULT_SINK_CHANNELS 2

typedef struct {
    qal_stream_handle_t *stream_handle;

    struct qal_device *qal_device;
    struct qal_stream_attributes *stream_attributes;
    const char *device_url;

    size_t buffer_size;
    size_t buffer_count;
    uint32_t sink_latency_us;
    uint64_t bytes_written;

    int write_fd;
    int index;
} qal_sink_data;

typedef struct {
    bool first;
    pa_sink *sink;
    pa_rtpoll *rtpoll;
    pa_thread_mq thread_mq;
    pa_thread *thread;
    pa_idxset *formats;
} pa_sink_data;

typedef struct {
    qal_sink_data *qal_sdata;
    pa_sink_data *pa_sdata;
    struct userdata *u;

    pa_fdsem *fdsem; /* common resource between pa and qal sink */
} pa_qal_sink_data;

typedef struct {
    struct pa_idxset *sinks;
} pa_qal_sink_module_data;

static pa_qal_sink_module_data *mdata = NULL;

static int restart_qal_sink(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map,
                            qal_device_id_t device_id, qal_stream_type_t type, int sink_id,
                            pa_qal_sink_data *sdata, uint32_t buffer_size, uint32_t buffer_count);
static int create_qal_sink(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map,
                           qal_device_id_t device_id, qal_stream_type_t type, int sink_id,
                           pa_qal_sink_data *sdata, uint32_t buffer_size, uint32_t buffer_count);
static int close_qal_sink(pa_qal_sink_data *sdata);
static int free_pa_sink(pa_qal_sink_data *sdata);

static const uint32_t supported_sink_rates[] =
                          {8000, 11025, 16000, 22050, 44100, 48000, 96000, 192000};

static const char *pa_qal_sink_get_name_from_type(qal_stream_type_t type) {
    const char *name = NULL;

    if (type == QAL_STREAM_LOW_LATENCY)
        name = "low_latency";
    else if (type == QAL_STREAM_DEEP_BUFFER)
        name = "deep_buffer";
    else if (type == QAL_STREAM_COMPRESSED)
        name = "offload";
    else if (type == QAL_STREAM_GENERIC)
        name = "direct_pcm";

    return name;
}

static void pa_qal_sink_set_volume_cb(pa_sink *s) {
    pa_qal_sink_data *sdata = NULL;
    float gain;
    int rc;
    pa_volume_t volume;
    qal_sink_data *qal_sdata = NULL;
    struct qal_volume_data *volume_data = NULL;
    uint32_t i,no_vol_pair;

    pa_assert(s);
    sdata = (pa_qal_sink_data *)s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->qal_sdata);
    pa_assert(sdata->qal_sdata->stream_handle);

    qal_sdata = sdata->qal_sdata;
    no_vol_pair = qal_sdata->stream_attributes->out_media_config.ch_info->channels;

    gain = ((float) pa_cvolume_max(&s->real_volume) * (float)QAL_MAX_GAIN) / (float)PA_VOLUME_NORM;
    volume = (pa_volume_t) roundf((float) gain * PA_VOLUME_NORM / QAL_MAX_GAIN);

    volume_data = (struct qal_volume_data *)malloc(sizeof(uint32_t) +
                                                 (sizeof(struct qal_channel_vol_kv) * (no_vol_pair)));

    volume_data->no_of_volpair = no_vol_pair;

    for (i = 0; i < no_vol_pair; i++) {
        volume_data->volume_pair[i].channel_mask = (uint32_t)qal_sdata->stream_attributes->out_media_config.ch_info->ch_map[i];
        volume_data->volume_pair[i].vol = gain;
    }

    rc = qal_stream_set_volume(sdata->qal_sdata->stream_handle, volume_data);
    if (rc)
        pa_log_error("qal stream : unable to set volume error %d\n", rc);
    else
        pa_cvolume_set(&s->real_volume, s->real_volume.channels, volume); /* TODO: Is this correct?  */

    pa_xfree(volume_data);
    return;
}

/* FIXME: Modify API to remove hardcoded values */
static int pa_qal_sink_fill_info(qal_sink_data *qal_sdata, pa_encoding_t encoding, pa_sample_spec *ss,
                                 pa_channel_map *map, qal_device_id_t device_id, qal_stream_type_t type,
                                 int sink_id, uint32_t buffer_size, uint32_t buffer_count) {
    uint32_t channel_count = 0;
    pa_assert(qal_sdata);

    qal_sdata->stream_attributes = pa_xnew0(struct qal_stream_attributes, 1);
    qal_sdata->stream_attributes->type = type;
    qal_sdata->stream_attributes->info.opt_stream_info.version = 1;
    qal_sdata->stream_attributes->info.opt_stream_info.duration_us = -1;
    qal_sdata->stream_attributes->info.opt_stream_info.has_video = false;
    qal_sdata->stream_attributes->info.opt_stream_info.is_streaming = false;

    qal_sdata->stream_attributes->flags = 0;
    qal_sdata->stream_attributes->direction = QAL_AUDIO_OUTPUT;
    qal_sdata->stream_attributes->out_media_config.sample_rate = ss->rate;
    qal_sdata->stream_attributes->out_media_config.bit_width = 16;
    qal_sdata->stream_attributes->out_media_config.aud_fmt_id = QAL_AUDIO_FMT_DEFAULT_PCM;

    channel_count = pa_qal_get_channel_count(map);
    qal_sdata->stream_attributes->out_media_config.ch_info = (struct qal_channel_info *) malloc(sizeof(uint16_t) + sizeof(uint8_t)*channel_count);
    if (!pa_qal_channel_map_to_qal(map, qal_sdata->stream_attributes->out_media_config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        pa_xfree(qal_sdata->stream_attributes->out_media_config.ch_info);
        return -1;
    }

    qal_sdata->qal_device = pa_xnew0(struct qal_device, 1);
    memset(qal_sdata->qal_device, 0, sizeof(struct qal_device));
    qal_sdata->qal_device->id = device_id;
    qal_sdata->qal_device->config.sample_rate = 48000;
    qal_sdata->qal_device->config.bit_width = 16;
    qal_sdata->qal_device->config.ch_info = pa_xnew0(struct qal_channel_info, 1);
    qal_sdata->qal_device->config.ch_info->channels = 2;
    qal_sdata->device_url = NULL; /* TODO: useful for BT devices */
    qal_sdata->bytes_written = 0;
    qal_sdata->index = sink_id;
    qal_sdata->buffer_size = (size_t)buffer_size;
    qal_sdata->buffer_count = (size_t)buffer_count;

    return 0;
}
/* Update after start stop issue is resolved */
static int pa_qal_sink_start(qal_sink_data *qal_sdata) {
    int rc = 0;
    pa_assert(qal_sdata);
    pa_log_debug("%s", __func__);

    rc = qal_stream_start(qal_sdata->stream_handle);
    pa_log_debug("qal_stream_start returned %d", rc);
    return rc;
}

static int pa_qal_sink_standby(qal_sink_data *qal_sdata) {
    int rc = 0;

    pa_assert(qal_sdata);
    pa_assert(qal_sdata->stream_handle);

    pa_log_debug("%s",__func__);

    rc = qal_stream_stop(qal_sdata->stream_handle);
    pa_log_debug("qal_stream_stop returned %d\n", rc);
    qal_sdata->bytes_written = 0;

    return 0;
}

static int pa_qal_sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause PA_GCC_UNUSED)
{
    pa_qal_sink_data *sdata = NULL;
    int r = 0;

    pa_assert(s);

    sdata = (pa_qal_sink_data *)(s->userdata);
    pa_assert(sdata);

    pa_log_debug("Sink new state is: %d", new_state);

    if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(s->thread_info.state))
        r = pa_qal_sink_start(sdata->qal_sdata);
    else if (new_state == PA_SINK_SUSPENDED)
        r = pa_qal_sink_standby(sdata->qal_sdata);

    return r;
}

static int pa_qal_sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    pa_qal_sink_data *sdata = (pa_qal_sink_data *)(PA_SINK(o)->userdata);

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->sink);

/* FIXME: Add callback function once qal_stream_get_param is enabled */
    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:
             *((int64_t*) data) = 0;
             return 0;

        default:
             break;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int pa_qal_sink_reconfigure_cb(pa_sink *s, pa_sample_spec *spec, pa_channel_map *map, bool passthrough) {
    pa_qal_sink_data *sdata = NULL;
    pa_sink_data *pa_sdata = NULL;
    qal_sink_data *qal_sdata = NULL;
    pa_qal_card_port_device_data *port_device_data;
    pa_channel_map new_map;

    bool supported = false;
    uint32_t i;
    int rc = 0;
    uint32_t old_rate;

    pa_assert(s);

    sdata = (pa_qal_sink_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qal_sdata);

    pa_log_debug("%s", __func__);

    pa_sdata = sdata->pa_sdata;
    qal_sdata = sdata->qal_sdata;

    for (i = 0; i < ARRAY_SIZE(supported_sink_rates) ; i++) {
        if (spec->rate == supported_sink_rates[i]) {
            supported = true;
            break;
        }
    }

    if (!supported) {
        pa_log_info("Sink does not support sample rate of %d Hz", spec->rate);
        return -1;
    }

    if (!PA_SINK_IS_OPENED(s->state)) {
        if (map)
            new_map = *map;
        else
            pa_channel_map_init_auto(&new_map, spec->channels, PA_CHANNEL_MAP_DEFAULT);

        old_rate = pa_sdata->sink->sample_spec.rate; /* take backup */
        pa_sdata->sink->sample_spec.rate = spec->rate;

        port_device_data = PA_DEVICE_PORT_DATA(pa_sdata->sink->active_port);
        rc = restart_qal_sink(PA_ENCODING_PCM, &pa_sdata->sink->sample_spec, &new_map, port_device_data->device,                                                                                                                                 qal_sdata->stream_attributes->type, qal_sdata->index, sdata, (uint32_t)qal_sdata->buffer_size, qal_sdata->buffer_count);
        if (PA_UNLIKELY(rc)) {
            pa_sdata->sink->sample_spec.rate = old_rate; /* restore old rate if failed */
            pa_log_error("Could create reopen qal sink, error %d", rc);
            return -1;
        }

        pa_sdata->sink->sample_spec = *spec;
        pa_sdata->sink->channel_map = new_map;

        pa_sink_set_fixed_latency(pa_sdata->sink, qal_sdata->sink_latency_us);
        return 0;
    }

    return rc;
}

static pa_idxset* pa_qal_sink_get_formats(pa_sink *s) {
    pa_qal_sink_data *sdata = NULL;

    pa_assert(s);

    sdata = (pa_qal_sink_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);

    return pa_idxset_copy(sdata->pa_sdata->formats, (pa_copy_func_t) pa_format_info_copy);
}

static void pa_qal_sink_thread_func(void *userdata) {
    pa_qal_sink_data *sdata;
    pa_sink_data *pa_sdata;
    qal_sink_data *qal_sdata;
    uint32_t sink_buffer_size;
    pa_memchunk chunk;
    struct qal_buffer out_buf;

    void *data;
    bool wait;
    int rc;

    pa_assert(userdata);

    sdata = (pa_qal_sink_data *)userdata;
    pa_sdata = sdata->pa_sdata;
    qal_sdata = sdata->qal_sdata;
    sink_buffer_size = (uint32_t)qal_sdata->buffer_size;

    pa_log_debug("%s:\n", __func__);

    if ((pa_sdata->sink->core->realtime_scheduling)) {
        pa_log_info("%s:: Making io thread for %s as realtime with prio %d", __func__, pa_qal_sink_get_name_from_type(qal_sdata->stream_attributes->type),
                     pa_sdata->sink->core->realtime_priority);
        pa_make_realtime(pa_sdata->sink->core->realtime_priority);
    }
    pa_thread_mq_install(&pa_sdata->thread_mq);

    memset(&out_buf, 0, sizeof(struct qal_buffer));

    while (true) {
        wait = true;

        if (pa_sdata->sink->thread_info.rewind_requested)
            pa_sink_process_rewind(pa_sdata->sink, 0);

        if ((PA_SINK_IS_OPENED(pa_sdata->sink->thread_info.state))) {

            /* Check if we need to resend previous buffer */
            if (!out_buf.buffer) {
                /* FIXME: can be more efficient by not using _full */
                pa_sink_render_full(pa_sdata->sink, qal_sdata->buffer_size, &chunk);
                pa_assert(chunk.length == qal_sdata->buffer_size);

                data = pa_memblock_acquire(chunk.memblock);
                out_buf.buffer = (char*)data + chunk.index;
                out_buf.size = chunk.length;
                sink_buffer_size = chunk.length;
            } else {
                /* Update buffer offset and size based on last write size*/
                out_buf.buffer = (char *)out_buf.buffer + sink_buffer_size - out_buf.size;
            }

            rc = qal_stream_write(qal_sdata->stream_handle, &out_buf);

            if (rc < 0) {
                pa_log_error("Could not write data: %d", rc);
            } else if ((rc >= 0) && (rc < (int)out_buf.size)) {
#ifdef SINK_DEBUG
                    pa_log_error("waiting for write done event");
#endif
                /* Store pending bytes to be written, write done event comes */
                pa_log_error("%d waiting for write done event, rc is %d, out_buf.size is %d", __LINE__, rc, (int)out_buf.size);
                out_buf.size = out_buf.size - rc;
            } else {
                qal_sdata->bytes_written += rc;
                /* Mark buffer as NULL, to indicate buffer has been consumed */
                out_buf.buffer = NULL;

                pa_memblock_release(chunk.memblock);
                pa_memblock_unref(chunk.memblock);

                wait = false;
            }
        } else if (pa_sdata->sink->thread_info.state == PA_SINK_SUSPENDED) {
            /* if sink is suspended state then reset buffer otherwise it might end up sending incorrect buffer to qal_write */
            pa_log_debug("%d sink in suspended state. sending empty buffer \n", __LINE__);
            memset(&out_buf, 0, sizeof(struct qal_buffer));
        }

        rc = pa_rtpoll_run(pa_sdata->rtpoll, wait);

        if (rc < 0) {
            pa_log_error("pa_rtpoll_run() returned an error: %d", rc);
            goto fail;
        }

        if (rc == 0)
            goto done;
    }

fail:
    pa_asyncmsgq_post(pa_sdata->thread_mq.outq, PA_MSGOBJECT(pa_sdata->sink->core), PA_CORE_MESSAGE_UNLOAD_MODULE, pa_sdata->sink->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(pa_sdata->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

done:
    pa_log_debug("Closing I/O thread");
}

static int open_qal_sink(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, qal_device_id_t device_id, qal_stream_type_t type,
                         int sink_id, pa_qal_sink_data *sdata, uint32_t buffer_size, uint32_t buffer_count) {
    int rc = 0;
    qal_sink_data *qal_sdata;
    size_t in_buffer_size;

#ifdef SINK_DUMP_ENABLED
    char *file_name;
#endif

    pa_assert(ss);
    pa_assert(map);
    pa_assert(sdata);
    pa_assert(sdata->qal_sdata);

    qal_sdata = sdata->qal_sdata;

    if (pa_qal_sink_fill_info(qal_sdata, encoding, ss, map, device_id, type, sink_id, buffer_size, buffer_count)) {
        goto exit;
    }

    pa_log_debug("opening sink with configuration type = 0x%x, encoding %d, format %d, sample_rate %d",
                 qal_sdata->stream_attributes->type, encoding, qal_sdata->stream_attributes->out_media_config.aud_fmt_id,
                 qal_sdata->stream_attributes->out_media_config.sample_rate);

    /* FIXME: Update call with callback function for events once compress offload usecase is enabled in QAL */
    rc = qal_stream_open(qal_sdata->stream_attributes, 1, qal_sdata->qal_device, 0, NULL, NULL, NULL,
                             &qal_sdata->stream_handle);

    if (rc) {
        qal_sdata->stream_handle = NULL;
        pa_log_error("Could not open output stream %d", rc);
        goto exit;
    }

    pa_log_debug("qal sink opened %p", qal_sdata->stream_handle);

    /* FIXME: Update it by calling qal_stream_get_buffer_size */
    rc = qal_stream_set_buffer_size(qal_sdata->stream_handle, &in_buffer_size, 0, &qal_sdata->buffer_size, qal_sdata->buffer_count);
    if(rc) {
        pa_log_error("qal_stream_set_buffer_size failed\n");
    }

    /* FIXME: Add DSP latency */
    qal_sdata->sink_latency_us = pa_bytes_to_usec(qal_sdata->buffer_size, ss);
    pa_log_debug("sink latency %dus", qal_sdata->sink_latency_us);

#ifdef SINK_DUMP_ENABLED
    file_name = pa_sprintf_malloc("/data/pcmdump_sink_%d", qal_sdata->index);

    qal_sdata->write_fd = open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
    if(qal_sdata->write_fd < 0)
        pa_log_error("Could not open write fd %d for sink index %d", qal_sdata->write_fd, qal_sdata->index);

    pa_xfree(file_name);
#endif

exit:
    return rc;
}

static int close_qal_sink(pa_qal_sink_data *sdata) {
    qal_sink_data *qal_sdata;
    int rc = -1;

    pa_assert(sdata);
    pa_assert(sdata->qal_sdata);

    qal_sdata = sdata->qal_sdata;

    pa_assert(qal_sdata->stream_handle);

    pa_log_debug("closing qal sink %p", qal_sdata->stream_handle);

    if (PA_UNLIKELY(qal_sdata->stream_handle == NULL)) {
        pa_log_error("Invalid sink handle %p", qal_sdata->stream_handle);
    } else {
        rc = qal_stream_close(qal_sdata->stream_handle);
        if (PA_UNLIKELY(rc))
            pa_log_error(" could not close sink sink handle %p, error  %d", qal_sdata->stream_handle, rc);

        qal_sdata->stream_handle = NULL;
    }

#ifdef SINK_DUMP_ENABLED
    close(qal_sdata->write_fd);
#endif

    return rc;
}

static int restart_qal_sink(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, qal_device_id_t device_id, qal_stream_type_t type,
                            int sink_id, pa_qal_sink_data *sdata,uint32_t buffer_size, uint32_t buffer_count) {
    int rc;

    rc = close_qal_sink(sdata);
    if (rc) {
        pa_log_error("close_qal_sink failed, error %d", rc);
        goto exit;
    }

    rc = open_qal_sink(encoding, ss, map, device_id, type, sink_id, sdata, buffer_size, buffer_count);
    if (rc) {
        pa_log_error("open_qal_sink failed during recreation, error %d", rc);
    }

exit:
    return rc;
}

static int free_qal_sink(pa_qal_sink_data *sdata) {
    int rc;

    pa_assert(sdata);
    pa_assert(sdata->qal_sdata);

    rc = close_qal_sink(sdata);
    if (rc) {
        pa_log_error("close_qal_sink failed, error %d", rc);
    }

    pa_xfree(sdata->qal_sdata->stream_attributes->out_media_config.ch_info);
    pa_xfree(sdata->qal_sdata->stream_attributes);
    pa_xfree(sdata->qal_sdata->qal_device->config.ch_info);
    pa_xfree(sdata->qal_sdata->qal_device);
    pa_xfree(sdata->qal_sdata);
    sdata->qal_sdata = NULL;

    return rc;
}

static int create_qal_sink(pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, qal_device_id_t device_id, qal_stream_type_t type, int sink_id,
                           pa_qal_sink_data *sdata, uint32_t buffer_size, uint32_t buffer_count) {
    int rc;

    sdata->qal_sdata = pa_xnew0(qal_sink_data, 1);

    rc = open_qal_sink(encoding, ss, map, device_id, type, sink_id, sdata, buffer_size, buffer_count);
    if (rc) {
        pa_log_error("open_qal_sink failed, error %d", rc);
        pa_xfree(sdata->qal_sdata);
        sdata->qal_sdata = NULL;
        return rc;
    }

    rc = qal_stream_start(sdata->qal_sdata->stream_handle);

    if (rc) {
        pa_log_error("qal stream start failed, error %d", rc);
        pa_xfree(sdata->qal_sdata);
        sdata->qal_sdata = NULL;
        return rc;
    }

    return rc;
}

static int pa_qal_sink_free_common_resources(pa_qal_sink_data *sdata) {
    pa_assert(sdata);

    if (sdata->fdsem)
        pa_fdsem_free(sdata->fdsem);

    return 0;
}

static int pa_qal_sink_alloc_common_resources(pa_qal_sink_data *sdata) {
    pa_assert(sdata);

   sdata->fdsem = pa_fdsem_new();
   if (!sdata->fdsem) {
       pa_log_error("Could not create fdsem");
       return -1;
   }

   return 0;
}

static int create_pa_sink(pa_module *m, char *sink_name, char *description, pa_idxset *formats, pa_sample_spec *ss, pa_channel_map *map, bool use_hw_volume, uint32_t alternate_sample_rate, pa_card *card, pa_hashmap *ports, const char *driver, pa_qal_sink_data *sdata) {
    pa_sink_new_data new_data;
    pa_sink_data *pa_sdata;
    pa_device_port *port;
    pa_format_info *format;
    pa_format_info *in_format;

    void *state;
    uint32_t i;

    bool port_sink_mapping = false;

    pa_assert(sdata->qal_sdata);

    pa_sdata = pa_xnew0(pa_sink_data, 1);
    pa_sink_new_data_init(&new_data);
    new_data.driver = driver;
    new_data.module = m;
    new_data.card = card;

    sdata->pa_sdata = pa_sdata;

    pa_sdata->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&pa_sdata->thread_mq, m->core->mainloop, pa_sdata->rtpoll);

    pa_sink_new_data_set_name(&new_data, sink_name);

    pa_log_info("ss->rate %d ss->channels %d", ss->rate, ss->channels);
    pa_sink_new_data_set_sample_spec(&new_data, ss);
    pa_sink_new_data_set_channel_map(&new_data, map);

    if (alternate_sample_rate == PA_ALTERNATE_SINK_RATE)
        pa_sink_new_data_set_alternate_sample_rate(&new_data, alternate_sample_rate);
    else if (alternate_sample_rate > 0)
        pa_log_error("%s: unsupported alternative sample rate %d",__func__, alternate_sample_rate);

    /* associate port with sink */
    PA_HASHMAP_FOREACH(port, ports, state) {
        pa_log_debug("adding port %s to sink %s", port->name, sink_name);
        pa_assert_se(pa_hashmap_put(new_data.ports, port->name, port) == 0);
        port_sink_mapping = true;
        pa_device_port_ref(port);
    }

    if (!port_sink_mapping) {
        pa_log_error("%s: sink_name %s creation failed as no port mapped, ",__func__, sink_name);
        goto fail;
    }

    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_STRING, pa_qal_sink_get_name_from_type(sdata->qal_sdata->stream_attributes->type));
    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_DESCRIPTION, description);

    pa_sdata->sink = pa_sink_new(m->core, &new_data, PA_SINK_HARDWARE | PA_SINK_LATENCY );
    pa_sink_new_data_done(&new_data);

    if (!pa_sdata->sink) {
        pa_log_error("Could not create pa sink");
        goto fail;
    }

    pa_log_debug("pa sink opened %p", pa_sdata->sink);

    pa_sdata->sink->userdata = (void *)sdata;
    pa_sdata->sink->parent.process_msg = pa_qal_sink_process_msg;
    pa_sdata->sink->set_state_in_io_thread = pa_qal_sink_set_state_in_io_thread_cb;
    pa_sdata->sink->set_port = NULL;
    pa_sdata->sink->reconfigure = pa_qal_sink_reconfigure_cb;

    if (pa_idxset_size(formats) > 0 ) {
        pa_sdata->sink->get_formats = pa_qal_sink_get_formats;

        pa_sdata->formats = pa_idxset_new(NULL, NULL);

        PA_IDXSET_FOREACH(in_format, formats, i) {
            format = pa_format_info_copy(in_format);
            pa_idxset_put(pa_sdata->formats, format, NULL);
        }
    }

    pa_sink_set_asyncmsgq(pa_sdata->sink, pa_sdata->thread_mq.inq);
    pa_sink_set_rtpoll(pa_sdata->sink, pa_sdata->rtpoll);
    pa_sink_set_max_request(pa_sdata->sink, sdata->qal_sdata->buffer_size);
    pa_sink_set_max_rewind(pa_sdata->sink, 0);
    pa_sink_set_fixed_latency(pa_sdata->sink, sdata->qal_sdata->sink_latency_us);

    if (use_hw_volume) {
        pa_sdata->sink->n_volume_steps = PA_VOLUME_NORM+1; /* FIXME: What should be value */
        pa_sink_set_set_volume_callback(pa_sdata->sink, pa_qal_sink_set_volume_cb);
    }

    pa_sdata->thread = pa_thread_new(sink_name, pa_qal_sink_thread_func, sdata);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

   pa_sink_put(pa_sdata->sink);

   return 0;

fail :
    if (pa_sdata)
        free_pa_sink(sdata);

    return -1;
}

static int free_pa_sink(pa_qal_sink_data *sdata) {
    pa_sink_data *pa_sdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_sdata = sdata->pa_sdata;

    pa_log_debug("closing pa sink %p", sdata->pa_sdata->sink);

    if (pa_sdata->sink)
        pa_sink_unlink(pa_sdata->sink);

    if (pa_sdata->thread) {
        pa_asyncmsgq_send(pa_sdata->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(pa_sdata->thread);
    }

    pa_thread_mq_done(&pa_sdata->thread_mq);

    if (pa_sdata->sink)
        pa_sink_unref(pa_sdata->sink);

    if (pa_sdata->formats)
        pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);

    if (pa_sdata->rtpoll)
        pa_rtpoll_free(pa_sdata->rtpoll);

    pa_xfree(pa_sdata);

    return 0;
}

bool pa_qal_sink_is_supported_sample_rate(uint32_t sample_rate) {
    bool supported = false;
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(supported_sink_rates) ; i++) {
        if (sample_rate == supported_sink_rates[i]) {
            supported = true;
            break;
        }
    }

    return supported;
}

int pa_qal_sink_create(pa_module *m, pa_card *card, const char *driver, const char *module_name, pa_qal_sink_config *sink, pa_qal_sink_handle_t **handle) {
    int rc = -1;
    pa_qal_sink_data *sdata;
    pa_device_port *card_port;
    pa_qal_card_port_config *sink_port;
    pa_hashmap *ports;
    pa_qal_card_port_device_data *port_device_data;

    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    void *state;

    pa_assert(m);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module_name);
    pa_assert(sink);
    pa_assert(sink->name);
    pa_assert(sink->description);
    pa_assert(sink->formats);
    pa_assert(sink->ports);

    if (pa_hashmap_isempty(sink->ports)) {
        pa_log_error("%s: empty port list", __func__);
        goto exit;
    }

    /*convert config port to card port */
    ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    PA_HASHMAP_FOREACH(sink_port, sink->ports, state) {
        if ((card_port = pa_hashmap_get(card->ports, sink_port->name)))
            pa_hashmap_put(ports, card_port->name, card_port);
    }

    /* first entry is default device */
    card_port = pa_hashmap_first(ports);
    port_device_data = PA_DEVICE_PORT_DATA(card_port);
    pa_assert(port_device_data);

    pa_log_info("%s: creating sink with ss %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &sink->default_spec));

    sdata = pa_xnew0(pa_qal_sink_data, 1);

    rc = pa_qal_sink_alloc_common_resources(sdata);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could pa_qal_sink_alloc_common_resources, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_qal_sink(sink->default_encoding, &sink->default_spec, &sink->default_map, port_device_data->device, sink->stream_type, sink->id, sdata, sink->buffer_size, sink->buffer_count);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could create open qal sink, error %d", rc);
        pa_qal_sink_free_common_resources(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_pa_sink(m, sink->name, sink->description, sink->formats, &sink->default_spec, &sink->default_map, sink->use_hw_volume, sink->alternate_sample_rate, card, ports, driver, sdata);
    pa_hashmap_free(ports);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa sink for sink %s, error %d", sink->name, rc);
        free_qal_sink(sdata);
        pa_qal_sink_free_common_resources(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    *handle = (pa_qal_sink_handle_t *)sdata;
    pa_idxset_put(mdata->sinks, sdata, NULL);

exit:
    return rc;
}

void pa_qal_sink_close(pa_qal_sink_handle_t *handle) {
    pa_qal_sink_data *sdata = (pa_qal_sink_data *)handle;

    pa_assert(sdata);

    free_pa_sink(sdata);
    free_qal_sink(sdata);
    pa_qal_sink_free_common_resources(sdata);

    pa_idxset_remove_by_data(mdata->sinks, sdata, NULL);

    pa_xfree(sdata);
}

void pa_qal_sink_module_deinit() {

    pa_assert(mdata);

    pa_idxset_free(mdata->sinks, NULL);

    pa_xfree(mdata);
    mdata = NULL;
}

void pa_qal_sink_module_init() {

    mdata = pa_xnew0(pa_qal_sink_module_data, sizeof(pa_qal_sink_module_data));

    mdata->sinks = pa_idxset_new(NULL, NULL);
}
