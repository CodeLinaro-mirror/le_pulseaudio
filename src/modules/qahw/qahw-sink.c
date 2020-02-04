/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
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
#include <pulse/timeval.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-format.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sink.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/mutex.h>
#include <pulse/util.h>
#include <pulsecore/trace_log.h>

#include <sys/time.h>
#include <time.h>

#include <qahw_api.h>

#include "qahw-sink.h"
#include "qahw-sink.h"
#include "qahw-utils.h"

/* #define SINK_DEBUG */

/* #define SINK_DUMP_ENABLED */

#define QAHW_MAX_GAIN 1

#define PA_ALTERNATE_SINK_RATE 44100
#define PA_FORMAT_DEFAULT_SAMPLE_RATE_INDEX 0
#define PA_FORMAT_DEFAULT_SAMPLE_FORMAT_INDEX 0
#define PA_DEFAULT_SINK_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_SINK_RATE 48000
#define PA_DEFAULT_SINK_CHANNELS 2

typedef enum {
    PA_QAHW_SINK_MESSAGE_DRAIN_READY = PA_SINK_MESSAGE_MAX + 1,
} pa_qahw_sink_event_t;

typedef enum {
    STATE_IDLE,
    STATE_PLAYING,
    STATE_PAUSED,
    STATE_DRAIN_READY,
} pa_qahw_sink_state_t;

typedef struct {
    qahw_stream_handle_t *out_handle;
    audio_io_handle_t handle;
    qahw_module_handle_t *module_handle;

    audio_output_flags_t flags;
    uint32_t devices;
    audio_config_t config;
    struct qahw_out_channel_map_param qahw_map;
    const char *device_url;

    size_t sink_buffer_size;
    uint32_t sink_latency_us;
    uint64_t bytes_written;

    pa_atomic_t wait_for_write_ready;
    pa_atomic_t wait_for_drain_ready;
    pa_atomic_t restart_in_progress;
    int write_fd;

    pa_encoding_t encoding;
    bool compressed;
    pa_qahw_sink_state_t state;
    pa_usec_t timestamp;

    int32_t buffer_duration;
    double max_gain;
    pa_atomic_t set_rt_prio_for_out_cb;
    int32_t dsd_rate;

    trace_log ts_log;
} qahw_sink_data;

typedef struct {
    bool first;
    pa_sink *sink;
    pa_rtpoll *rtpoll;
    pa_thread_mq thread_mq;
    pa_thread *thread;
    pa_rtpoll_item *rtpoll_item;
    pa_idxset *formats;

    pa_qahw_card_avoid_processing_config_id_t avoid_config_processing;
} pa_sink_data;

typedef struct {
    qahw_sink_data *qahw_sdata;
    pa_sink_data *pa_sdata;
    pa_qahw_sink_extn_handle_t *sink_extn_handle;
    struct userdata *u;

    pa_fdsem *fdsem; /* common resource between pa and qahw sink */
} pa_qahw_sink_data;

typedef struct {
    struct pa_idxset *sinks;
} pa_qahw_sink_module_data;

static pa_qahw_sink_module_data *mdata = NULL;

static int restart_qahw_sink(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                             audio_output_flags_t flags, int sink_id, pa_qahw_sink_data *sdata);
static int create_qahw_sink(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                            audio_output_flags_t flags, int sink_id, pa_qahw_sink_data *sdata, int32_t buffer_duration, double max_gain);
static int open_qahw_sink(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                          audio_output_flags_t flags, int sink_id, pa_qahw_sink_data *sdata, int32_t buffer_duration, double max_gain);
static int close_qahw_sink(pa_qahw_sink_data *sdata);
static int free_pa_sink(pa_qahw_sink_data *sdata);
static int pa_qahw_sink_pause(pa_qahw_sink_data *sdata, bool pause);

static const uint32_t supported_sink_rates[] =
                          {8000, 11025, 16000, 22050, 44100, 48000, 96000, 192000};

#ifdef CLOCK_MONOTONIC
static int32_t get_clock_id() {
    return CLOCK_MONOTONIC;
}
#else
static int32_t get_clock_id() {
    return CLOCK_REALTIME;
}
#endif

static pa_sample_format_t pa_qahw_sink_find_nearest_supported_pa_format(pa_sample_format_t format) {
    pa_sample_format_t format1;

    switch(format) {
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_U8:
        case PA_SAMPLE_ALAW:
        case PA_SAMPLE_S16BE:
            format1 = PA_SAMPLE_S16LE;
            break;
        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
            format1 = PA_SAMPLE_S24LE;
            break;
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_S32BE:
            format1 = PA_SAMPLE_S32LE;
            break;
        default:
            format1 = PA_SAMPLE_S16LE;
            pa_log_error(" unsupport format %d hence defaulting to %d",format, format1);
    }
    return format1;

}

static uint32_t pa_qahw_sink_find_nearest_supported_sample_rate(uint32_t sample_rate) {
    uint32_t i;
    uint32_t nearest_rate = PA_DEFAULT_SINK_RATE;

    for (i = 0; i < ARRAY_SIZE(supported_sink_rates) ; i++) {
        if (sample_rate == supported_sink_rates[i]) {
            nearest_rate = sample_rate;
            break;
        } else if (sample_rate > supported_sink_rates[i]) {
            nearest_rate = supported_sink_rates[i];
        }
    }

    return nearest_rate;
}

static const char *pa_qahw_sink_get_name_from_flags(audio_output_flags_t flags) {
    const char *name = NULL;

    if (flags == AUDIO_OUTPUT_FLAG_PRIMARY)
        name = "primary";
    else if (flags == AUDIO_OUTPUT_FLAG_FAST)
        name = "low_latency";
    else if (flags == AUDIO_OUTPUT_FLAG_DEEP_BUFFER)
        name ="deep_buffer";
    else if (flags & AUDIO_OUTPUT_FLAG_DIRECT_PCM)
        name = "direct_pcm";
    else if (flags & AUDIO_OUTPUT_FLAG_DIRECT)
        name = "direct_pcm";
    else if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        name = "offload";
    else if (flags == (AUDIO_OUTPUT_FLAG_RAW | AUDIO_OUTPUT_FLAG_FAST))
        name = "ultra_low_latency";

    return name;
}

static int pa_qahw_out_cb(qahw_stream_callback_event_t event, void *param, void *userdata) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->fdsem);

    if (pa_atomic_load(&sdata->qahw_sdata->set_rt_prio_for_out_cb) && sdata->pa_sdata->sink->core->realtime_scheduling) {
        pa_make_realtime(sdata->pa_sdata->sink->core->realtime_priority);
        pa_atomic_store(&sdata->qahw_sdata->set_rt_prio_for_out_cb, 0);
    }

    switch (event) {
        case QAHW_STREAM_CBK_EVENT_WRITE_READY:

            pa_atomic_store(&sdata->qahw_sdata->wait_for_write_ready, 0);

#ifdef SINK_DEBUG
            pa_log_debug("Received event QAHW_STREAM_CBK_EVENT_WRITE_READY for handle %p", sdata->qahw_sdata->out_handle);
#endif

            /*Wake up sink thread */
            pa_fdsem_post(sdata->fdsem);
            break;

        case QAHW_STREAM_CBK_EVENT_DRAIN_READY:
            pa_atomic_store(&sdata->qahw_sdata->wait_for_drain_ready, 0);
            pa_log_debug("%s: Received event QAHW_STREAM_CBK_EVENT_DRAIN_READY for handle %p",
                          __func__, sdata->qahw_sdata->out_handle);
            sdata->qahw_sdata->state = STATE_DRAIN_READY;
            /* post drain complete to i/o thread */
            pa_asyncmsgq_post(sdata->pa_sdata->thread_mq.inq,
                              PA_MSGOBJECT(sdata->pa_sdata->sink),
                              PA_QAHW_SINK_MESSAGE_DRAIN_READY, NULL, 0, NULL, NULL);
            break;

        default:
            pa_log_error(" Unsupported event %d  handle %p", event, sdata->qahw_sdata->out_handle);
            break;
    }

    return 0;
}

static int pa_qahw_sink_fill_info(qahw_sink_data *qahw_sdata, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                                audio_output_flags_t flags, int sink_id, int32_t buffer_duration, double max_gain) {
    qahw_sdata->buffer_duration = buffer_duration;

    if (encoding == PA_ENCODING_PCM)
        qahw_sdata->config.format = pa_qahw_util_get_qahw_format_from_pa_sample(ss->format);
    else
        qahw_sdata->config.format = pa_qahw_util_get_qahw_format_from_pa_encoding(encoding);

    qahw_sdata->config.sample_rate = ss->rate;
    qahw_sdata->config.channel_mask = audio_channel_out_mask_from_count(map->channels);

    if (!pa_qahw_channel_map_to_qahw(map, &qahw_sdata->qahw_map)) {
        pa_log_error("%s: unsupported channel map", __func__);
        return -1;
    }

    /* DIRECT PCM uses offload structure */
    if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD || flags & AUDIO_OUTPUT_FLAG_DIRECT_PCM || flags & AUDIO_OUTPUT_FLAG_DIRECT)  {
        qahw_sdata->config.offload_info = AUDIO_INFO_INITIALIZER;
        qahw_sdata->config.offload_info.format = qahw_sdata->config.format;
        qahw_sdata->config.offload_info.sample_rate = qahw_sdata->config.sample_rate;
        qahw_sdata->config.offload_info.channel_mask = qahw_sdata->config.channel_mask;

        if (qahw_sdata->config.format == AUDIO_FORMAT_DSD)
            qahw_sdata->config.offload_info.bit_width = 32;
    }

    qahw_sdata->devices = devices;
    qahw_sdata->flags = flags;
    qahw_sdata->handle = sink_id; /* check if its correct */
    qahw_sdata->device_url = NULL; /* TODO: useful for BT devices */
    qahw_sdata->bytes_written = 0;
    qahw_sdata->encoding = encoding;
    qahw_sdata->timestamp = 0;
    qahw_sdata->state = STATE_IDLE;
    qahw_sdata->max_gain = max_gain;

    return 0;
}

static uint64_t pa_qahw_sink_get_latency(pa_qahw_sink_data *sdata) {
    int rc, delta, bytes_rendered;
    int64_t latency = 0;
    qahw_sink_data *qahw_sdata;
    pa_sink_data *pa_sdata;

    pa_usec_t qahw_time, now, delta_usec;
    struct qahw_out_presentation_position_param pos_param;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qahw_sdata);

    qahw_sdata = sdata->qahw_sdata;
    pa_sdata = sdata->pa_sdata;

    pa_assert(pa_sdata->sink);
    pa_assert(qahw_sdata->out_handle);
    memset(&pos_param, 0, sizeof(struct qahw_out_presentation_position_param));

    pos_param.clock_id = get_clock_id();

    if(qahw_sdata->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        rc = qahw_out_get_param_data(qahw_sdata->out_handle, QAHW_PARAM_OUT_PRESENTATION_POSITION,
                                 (qahw_param_payload *)&pos_param);
    } else {
        rc = qahw_out_get_presentation_position(qahw_sdata->out_handle, &pos_param.frames,
                                 &pos_param.timestamp);
    }

    if (!rc) {
        qahw_time = pa_timespec_load(&pos_param.timestamp);
        bytes_rendered =  pos_param.frames * pa_frame_size(&pa_sdata->sink->sample_spec);

        /* calculate bytes pending to be rendered */
        if (qahw_sdata->compressed) {
            delta_usec = qahw_sdata->timestamp -
               pa_bytes_to_usec(bytes_rendered, &pa_sdata->sink->sample_spec);
        } else {
            delta = qahw_sdata->bytes_written - bytes_rendered;
            delta_usec = pa_bytes_to_usec(delta, &pa_sdata->sink->sample_spec);
        }

#ifdef SINK_DEBUG
        pa_log_debug("%s:: timestamp %" PRId64 ", frames %" PRId64 " clock id %d", __func__,
                     pos_param.timestamp.tv_sec*1000000000LL + pos_param.timestamp.tv_nsec, pos_param.frames, pos_param.clock_id);
#endif
        /* bytes written should never be less than bytes rendered */
        if (delta_usec <= 0) {
#ifdef SINK_DEBUG
            pa_log_debug("latency is 0");
#endif
            return 0;
        }

        now = pa_rtclock_now();
        /* latency = time to render pending frames - time elapesed after qahw_out_get_presentation_position */
        latency = (int64_t)(delta_usec - (now - qahw_time));

#ifdef SINK_DEBUG
        pa_log_debug("%s:: now %" PRId64 "us, qahw_time %" PRId64 "us, delta %" PRId64 ", latency %" PRId64 "us", __func__, (int64_t)now, (int64_t)qahw_time,
                    (int64_t)delta_usec, latency);
#endif
    } else  {
        latency = (int64_t)(pa_bytes_to_usec(qahw_sdata->bytes_written, &pa_sdata->sink->sample_spec));
#ifdef SINK_DEBUG
        pa_log_debug("qahw_out_get_presentation_position failed, using latency based on written bytes latency = %" PRId64 "", latency);
#endif
    }

    if (latency < 0) {
        pa_log_error("latency is invalid (-ve), resetting it zero");
        latency = 0;
    }

    return (uint64_t)latency;
}

static int pa_qahw_sink_start(pa_qahw_sink_data *sdata, pa_sink_state_t new_state) {
    int r = 0;

    pa_assert(sdata);
    /* Required to resume paused compressed streams in case new state is RUNNING */
    if (new_state == PA_SINK_RUNNING)
        r = pa_qahw_sink_pause(sdata, false);

    trace_open(&sdata->qahw_sdata->ts_log);
    return r;
}

static int pa_qahw_sink_standby(qahw_sink_data *qahw_sdata) {

    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->out_handle);

    pa_log_info("%s",__func__);

    if (qahw_sdata->compressed && (qahw_sdata->state == STATE_PAUSED)) {
        /* Resume is expected if state is paused for compressed streams */
        pa_log_debug("%s: Avoid standby for compress playback in pause state", __func__);
        return 0;
    }

    qahw_out_standby(qahw_sdata->out_handle);
    pa_atomic_store(&qahw_sdata->wait_for_write_ready, 0);
    qahw_sdata->state = STATE_IDLE;

    /* Reset bytes written only for offload playback since AHAL does not reset in standby */
    if (qahw_sdata->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        qahw_sdata->bytes_written = 0;

    if (qahw_sdata->flags & AUDIO_OUTPUT_FLAG_FAST)
        pa_atomic_store(&qahw_sdata->set_rt_prio_for_out_cb, 1);

    return 0;
}

static int pa_qahw_sink_pause(pa_qahw_sink_data *sdata, bool pause) {

    qahw_sink_data *qahw_sdata;
    int rc = 0;

    pa_assert(sdata);

    qahw_sdata = sdata->qahw_sdata;

    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->out_handle);

    if (!qahw_sdata->compressed) {
        pa_log_debug("%s: Unsupported for non-compress playback", __func__);
        return 0;
    }
    pa_log_info("%s: %s compress playback", __func__, pause ? "pause" : "resume");

    if (pause && (qahw_sdata->state == STATE_PLAYING)) {
        rc = qahw_out_pause(qahw_sdata->out_handle);
        if (!rc)
            qahw_sdata->state = STATE_PAUSED;
    } else if (!pause && (qahw_sdata->state == STATE_PAUSED)) {
        rc = qahw_out_resume(qahw_sdata->out_handle);
        if (!rc)
            qahw_sdata->state = STATE_PLAYING;
    }

    return rc;
}

static void pa_qahw_sink_set_volume_cb(pa_sink *s) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)s->userdata;
    float gain;
    int rc;
    pa_volume_t volume;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->qahw_sdata->out_handle);

    if (sdata->qahw_sdata->max_gain > 0 && sdata->qahw_sdata->max_gain <= QAHW_MAX_GAIN) {
        pa_log_debug ("qahw stream %p: gain %f\n", sdata->qahw_sdata->out_handle, (float)sdata->qahw_sdata->max_gain);
        gain = ((float) pa_cvolume_max(&s->real_volume) * (float)sdata->qahw_sdata->max_gain) / (float)PA_VOLUME_NORM;
        volume = (pa_volume_t) roundf((float) gain * PA_VOLUME_NORM / sdata->qahw_sdata->max_gain);
    } else {
        gain = ((float) pa_cvolume_max(&s->real_volume) * (float)QAHW_MAX_GAIN) / (float)PA_VOLUME_NORM;
        volume = (pa_volume_t) roundf((float) gain * PA_VOLUME_NORM / QAHW_MAX_GAIN);
    }

    pa_log_debug ("qahw stream %p: gain %f\n", sdata->qahw_sdata->out_handle, gain);

    rc = qahw_out_set_volume(sdata->qahw_sdata->out_handle, gain, gain);
    if (rc)
        pa_log_error("qahw stream %p: unable to set volume error %d\n", sdata->qahw_sdata->out_handle, rc);
    else
        pa_cvolume_set(&s->real_volume, s->real_volume.channels, volume); /* TODO: Is this correct? */

    return;
}

static int pa_qahw_sink_set_port_cb(pa_sink *s, pa_device_port *p) {
    pa_qahw_card_port_device_data *port_device_data;
    pa_qahw_card_port_device_data *active_port_device_data;
    char *kvpair;
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)s->userdata;
    int rc;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->qahw_sdata->out_handle);

    port_device_data = PA_DEVICE_PORT_DATA(p);
    pa_assert(port_device_data);

    active_port_device_data = PA_DEVICE_PORT_DATA(s->active_port);
    pa_assert(active_port_device_data);

    if (active_port_device_data->device & AUDIO_DEVICE_OUT_HDMI) {
        kvpair = pa_sprintf_malloc("%s=%d", QAHW_PARAMETER_DEVICE_DISCONNECT, active_port_device_data->device);

        rc = qahw_set_parameters(sdata->qahw_sdata->module_handle, kvpair);
        if (rc)
            pa_log_error("qahw set parameters failed %d",rc);

        pa_log_info("%s: port name: %s kvpair %s device %x", __func__, p->name, kvpair, active_port_device_data->device);

        pa_xfree(kvpair);
    }

    if ((port_device_data->device & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP) || (port_device_data->device & AUDIO_DEVICE_OUT_HDMI)) {
        kvpair = pa_sprintf_malloc("%s=%d", QAHW_PARAMETER_DEVICE_CONNECT, port_device_data->device);

        rc = qahw_set_parameters(sdata->qahw_sdata->module_handle, kvpair);
        if (rc)
            pa_log_error("qahw set parameters failed %d",rc);

        pa_log_info("%s: port name: %s kvpair %s device %x", __func__, p->name, kvpair, port_device_data->device);

        pa_xfree(kvpair);
    }

    kvpair = pa_sprintf_malloc("%s=%d", QAHW_PARAMETER_STREAM_ROUTING, port_device_data->device);
    pa_log_info("%s: port name: %s kvpair %s device %x", __func__, p->name, kvpair, port_device_data->device);

    rc = qahw_out_set_parameters(sdata->qahw_sdata->out_handle, kvpair);
    if (rc)
        pa_log_error("qahw routing failed %d",rc);
    else
        sdata->qahw_sdata->devices = port_device_data->device;

    pa_xfree(kvpair);

    return rc;
}

static int pa_qahw_sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause PA_GCC_UNUSED)
{
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)(s->userdata);
    int r = 0;

    pa_log_debug("Sink current state is: %d, new state is: %d", s->thread_info.state, new_state);

    if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(s->thread_info.state))
        r = pa_qahw_sink_start(sdata, new_state);
    else if (new_state == PA_SINK_SUSPENDED)
        r = pa_qahw_sink_standby(sdata->qahw_sdata);
    else if (PA_SINK_IS_RUNNING(new_state) && (s->thread_info.state == PA_SINK_IDLE)) {
        r = pa_qahw_sink_pause(sdata, false);
        trace_open(&sdata->qahw_sdata->ts_log);
        trace_newstream(&sdata->qahw_sdata->ts_log, s->name);
    } else if (PA_SINK_IS_RUNNING(s->thread_info.state) && (new_state == PA_SINK_IDLE))
        r = pa_qahw_sink_pause(sdata, true);

    return r;
}

static bool pa_qahw_sink_set_format_cb(pa_sink *s, const pa_format_info *format) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)s->userdata;
    qahw_sink_data *qahw_sdata;
    pa_sink_data *pa_sdata;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_encoding_t encoding;
    char ch_map_buf[PA_CHANNEL_MAP_SNPRINT_MAX];
    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];
    char fmt[PA_FORMAT_INFO_SNPRINT_MAX];
    int rc;
    bool ret = false;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);

    qahw_sdata = sdata->qahw_sdata;
    pa_sdata = sdata->pa_sdata;

    pa_assert(qahw_sdata);
    pa_assert(pa_sdata);
    pa_assert(pa_sdata->sink);
    pa_assert(qahw_sdata->out_handle);

    if (format == NULL) {
        if (qahw_sdata->encoding == PA_ENCODING_PCM) {
            pa_log_error("%s: Already set to default encoding", __func__);
            goto exit;
        }

        pa_log_debug("%s: Exit compress playback and restore previous config", __func__);
        ss = pa_sdata->sink->sample_spec;
        map = pa_sdata->sink->channel_map;
        encoding = PA_ENCODING_PCM;
    } else {
        if (pa_format_info_is_compressed(format) == 0) {
            pa_log_error("%s: Format info structure is not compressed", __func__);
            goto exit;
        }

        pa_log_debug("%s: Start compress playback", __func__);
        rc =  pa_format_info_to_sample_spec2(format, &ss, &map,
                    &pa_sdata->sink->sample_spec, &pa_sdata->sink->channel_map);
        if (rc) {
            pa_log_error("%s: Failed to obtain sample spec from format", __func__);
            goto exit;
        }

        pa_log_debug("Negotiated format: %s", pa_format_info_snprint(fmt, sizeof(fmt), format));
        rc = pa_format_info_get_rate(format, &ss.rate);
        rc = pa_format_info_get_channels(format, &ss.channels);
        pa_qahw_util_channel_map_init(&map, ss.channels);

        if (format->encoding == PA_ENCODING_DSD) {
            pa_format_info_get_prop_int(format, "dsd-type", &(qahw_sdata->dsd_rate));
            ss.format = PA_SAMPLE_S32LE;
        }

        rc = pa_qahw_util_set_qahw_metadata_from_pa_format(format);
        if (rc) {
            pa_log_error("%s: Failed to set metadata from format", __func__);
            goto exit;
        }

        encoding = format->encoding;

        pa_log_info("%s: sample spec %s", __func__,
              pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &ss));
        pa_log_info("%s: channel map %s", __func__,
              pa_channel_map_snprint(ch_map_buf, sizeof(ch_map_buf), &map));
    }

    rc = restart_qahw_sink(qahw_sdata->module_handle, encoding,
                           &ss, &map, qahw_sdata->devices, qahw_sdata->flags,
                           qahw_sdata->handle, sdata);
    if (rc) {
        pa_log_error("%s: Failed to restart qahw_sink with %s encoding",
            __func__, format == NULL ? "default" : "requested");
    } else {
        pa_log_info("%s: Started qahw_sink with %s encoding",
            __func__, format == NULL ? "default" : "requested");
        qahw_sdata->compressed = (encoding != PA_ENCODING_PCM ? true : false);
        ret = true;
    }

exit:
    return ret;
}

static int pa_qahw_sink_drain_cb(pa_sink *s) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)s->userdata;
    qahw_sink_data *qahw_sdata;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);

    qahw_sdata = sdata->qahw_sdata;

    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->out_handle);

    pa_atomic_store(&qahw_sdata->wait_for_drain_ready, 1);
    return qahw_out_drain(qahw_sdata->out_handle, QAHW_DRAIN_ALL);
}

static int pa_qahw_sink_flush_cb(pa_sink *s) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)s->userdata;
    qahw_sink_data *qahw_sdata;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);

    qahw_sdata = sdata->qahw_sdata;

    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->out_handle);

    /* stream should be in paused state during flush */
    qahw_out_pause(qahw_sdata->out_handle);

    return qahw_out_flush(qahw_sdata->out_handle);
}

static int pa_qahw_sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)(PA_SINK(o)->userdata);

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->sink);

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:
            *((int64_t*) data) = pa_qahw_sink_get_latency(sdata);
            return 0;

        case PA_QAHW_SINK_MESSAGE_DRAIN_READY:
            pa_sink_drain_complete(sdata->pa_sdata->sink);
            return 0;

        default:
            break;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int pa_qahw_sink_reconfigure_cb(pa_sink *s, pa_sample_spec *spec, pa_channel_map *map, bool passthrough) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *) s->userdata;
    pa_encoding_t encoding = PA_ENCODING_PCM;
    pa_sink_data *pa_sdata = NULL;
    qahw_sink_data *qahw_sdata = NULL;

    char channel_map_buf[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_channel_map new_map;
    pa_sample_spec tmp_spec;

    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    int rc = -1;

    pa_assert(s);
    pa_assert(s->userdata);
    pa_assert(spec);
    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qahw_sdata);

    pa_log_info("%s", __func__);

    pa_sdata = sdata->pa_sdata;
    qahw_sdata = sdata->qahw_sdata;
    tmp_spec = *spec;

    if (!PA_SOURCE_IS_OPENED(s->state)) {
        pa_log_info("%s: old sample spec %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &pa_sdata->sink->sample_spec));
        pa_log_info("%s: requested sample spec %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &tmp_spec));

        if (map) {
            pa_log_info("%s:old channel map %s", __func__, pa_channel_map_snprint(channel_map_buf, sizeof(channel_map_buf), &pa_sdata->sink->channel_map));
            pa_log_info("%s:requested channel map %s", __func__, pa_channel_map_snprint(channel_map_buf, sizeof(channel_map_buf), map));

            if (pa_sdata->avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_CHANNELS)
                new_map = *map;
            else {
                new_map = pa_sdata->sink->channel_map;
                tmp_spec.channels = pa_sdata->sink->sample_spec.channels;
            }
        } else {
            if (pa_sdata->avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_CHANNELS)
                pa_channel_map_init_auto(&new_map, tmp_spec.channels, PA_CHANNEL_MAP_DEFAULT);
            else {
                new_map = pa_sdata->sink->channel_map;
                tmp_spec.channels = pa_sdata->sink->sample_spec.channels;
            }
        }

        qahw_sdata->devices = *((audio_devices_t *)PA_DEVICE_PORT_DATA(pa_sdata->sink->active_port));

        /* find nearest suitable format */
        if (pa_sdata->avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_BIT_WIDTH)
            tmp_spec.format = pa_qahw_sink_find_nearest_supported_pa_format(spec->format);
        else
            tmp_spec.format = pa_sdata->sink->sample_spec.format;

        /* find nearest suitable rate */
        if (pa_sdata->avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_SAMPLE_RATE)
            tmp_spec.rate =  pa_qahw_sink_find_nearest_supported_sample_rate(spec->rate);
        else
            tmp_spec.rate = pa_sdata->sink->sample_spec.rate;

        pa_log_info("%s: trying to reconfigure qahw with sample spec %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &tmp_spec));

        /* first try to open with new conf if it fails then restore session with old conf */
        rc = restart_qahw_sink(qahw_sdata->module_handle, encoding, &tmp_spec, &new_map, qahw_sdata->devices, qahw_sdata->flags, qahw_sdata->handle, sdata);
        if (rc) {
            pa_log_error("%s: could note create qahw sink with requested conf, error %d, restoring old conf", __func__, rc);
            rc = open_qahw_sink(qahw_sdata->module_handle, encoding, &pa_sdata->sink->sample_spec, &pa_sdata->sink->channel_map, qahw_sdata->devices,
                                   qahw_sdata->flags, qahw_sdata->handle, sdata, qahw_sdata->buffer_duration, qahw_sdata->max_gain);
            if (rc)
                pa_log_info("%s: restoring of qahw sink with old config failed, error %d", __func__, rc);

            goto exit;
        }

        pa_sdata->sink->sample_spec = tmp_spec;
        pa_sdata->sink->channel_map = new_map;

        pa_qahw_sink_extn_sink_handle_update(sdata->sink_extn_handle, qahw_sdata->out_handle);
        pa_sink_set_max_request(pa_sdata->sink, sdata->qahw_sdata->sink_buffer_size);
        pa_sink_set_max_rewind(pa_sdata->sink, 0);
        pa_sink_set_fixed_latency(pa_sdata->sink, qahw_sdata->sink_latency_us);
        return 0;
    }

exit:
    return rc;
}

int pa_qahw_sink_set_param(pa_qahw_sink_handle_t *handle, const char *param) {
    int ret = -1;
    pa_qahw_sink_data *sdata = NULL;

    pa_assert(handle);

    sdata = (pa_qahw_sink_data *)handle;

    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->qahw_sdata->module_handle);

    ret = qahw_set_parameters(sdata->qahw_sdata->module_handle, param);
    pa_log_info("%s: param %s set to hal with return value %d", __func__, param, ret);

    return ret;
}

int pa_qahw_sink_get_media_config(pa_qahw_sink_handle_t *handle, pa_sample_spec *ss, pa_channel_map *map, pa_encoding_t *encoding) {
        pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)handle;
        pa_format_info *f;

        uint32_t i;
        int ret = -1;

        pa_assert(sdata);
        pa_assert(sdata->pa_sdata);
        pa_assert(sdata->pa_sdata->sink);

        *ss = sdata->pa_sdata->sink->sample_spec;
        *map = sdata->pa_sdata->sink->channel_map;

        PA_IDXSET_FOREACH(f, sdata->pa_sdata->formats, i) {
            /* currently a sink supports single format */
            *encoding = f->encoding;
            ret = 0;
            break;
        }

        return ret;
}


static pa_idxset* pa_qahw_sink_get_formats(pa_sink *s) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);

    return pa_idxset_copy(sdata->pa_sdata->formats, (pa_copy_func_t) pa_format_info_copy);
}

static void pa_qahw_sink_thread_func(void *userdata) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)userdata;
    pa_sink_data *pa_sdata = sdata->pa_sdata;
    qahw_sink_data *qahw_sdata = sdata->qahw_sdata;
    pa_usec_t timestamp;

    pa_memchunk chunk;
    qahw_out_buffer_t out_buf;

    void *data;
    bool wait;
    int rc, ret;
    bool running, ts_enable = false;
    size_t sink_buffer_size = qahw_sdata->sink_buffer_size;
#ifdef SINK_DEBUG
    pa_usec_t cur_qtimer, ticks = 0;
#endif

    if ((pa_sdata->sink->core->realtime_scheduling)) {
        pa_log_info("%s:: Making io thread for %s as realtime with prio %d", __func__, pa_qahw_sink_get_name_from_flags(qahw_sdata->flags), pa_sdata->sink->core->realtime_priority);
        pa_make_realtime(pa_sdata->sink->core->realtime_priority);
    }

    pa_thread_mq_install(&pa_sdata->thread_mq);

    memset(&out_buf, 0, sizeof(qahw_out_buffer_t));
    ts_enable = qahw_sdata->flags & QAHW_OUTPUT_FLAG_TIMESTAMP;

    while (true) {
        wait = true;

        if (pa_sdata->sink->thread_info.rewind_requested)
            pa_sink_process_rewind(pa_sdata->sink, 0);

        /* A compressed sink only renders in RUNNING, not in IDLE */
        running = (!qahw_sdata->compressed && PA_SINK_IS_OPENED(pa_sdata->sink->thread_info.state) && !ts_enable) ||
                   PA_SINK_IS_RUNNING(pa_sdata->sink->thread_info.state);

        if (ts_enable)
            pa_rtpoll_set_timer_disabled(pa_sdata->rtpoll);

        if (running && !pa_atomic_load(&qahw_sdata->wait_for_write_ready) && !pa_atomic_load(&qahw_sdata->restart_in_progress)) {
            /* Check if we need to resend previous buffer */
            if (!out_buf.buffer) {
                if (qahw_sdata->compressed) {
                    pa_sink_render(pa_sdata->sink, qahw_sdata->sink_buffer_size, &chunk);
                    /* Handle no data scenario for compressed streams */
                    if (pa_memblock_is_silence(chunk.memblock)) {
                        pa_memblock_unref(chunk.memblock);
                        pa_log_debug("Got silence, avoid writing the block");
                        goto poll;
                    }
                } else {
                    if (ts_enable) {
                        ret = pa_sink_render_one(pa_sdata->sink, &chunk);
                        if (!ret) {
#ifdef SINK_DEBUG
                            pa_log_debug("render one returned empty chunk");
#endif
                            /* wait for 1msec */
                            wait = true;
                            pa_rtpoll_set_timer_relative(pa_sdata->rtpoll, 1000);
                            goto poll;
                        }
                        if (chunk.timestamp == PA_NSEC_INVALID) {
                            pa_log_error("invalid timestamp");
                            pa_memblock_unref(chunk.memblock);
                            goto poll;
                        }
                    } else {
                        pa_sink_render_full(pa_sdata->sink, qahw_sdata->sink_buffer_size, &chunk);
                        pa_assert(chunk.length == qahw_sdata->sink_buffer_size);
                    }
                }

                data = pa_memblock_acquire(chunk.memblock);
                out_buf.buffer = (char*)data + chunk.index;
                out_buf.bytes = chunk.length;
                if (ts_enable) {
                    timestamp =  chunk.timestamp / PA_NSEC_PER_USEC;
                    out_buf.timestamp = (int64_t *)&timestamp;
#ifdef SINK_DEBUG
#if defined __aarch64__
                    asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
#else
                    asm volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r"(ticks));
#endif
                    cur_qtimer = ticks * 10/192;
                    pa_log_error("write_timestamp %" PRId64 "usec write_cur_qtimer %" PRId64 "usec", timestamp, cur_qtimer);
#endif
                    trace_ts(&qahw_sdata->ts_log, pa_sdata->sink->name, chunk.timestamp, chunk.duration);
                }
                sink_buffer_size = chunk.length;
                if (qahw_sdata->compressed) {
                    if (chunk.duration == PA_NSEC_INVALID)
                        pa_log_warn("%s: Did not get duration in compressed mode", __func__);
                    else
                        qahw_sdata->timestamp += chunk.duration / PA_NSEC_PER_USEC;
                }
            } else {
                /* Update buffer offset and size based on last write size*/
                out_buf.buffer = (char *)out_buf.buffer + sink_buffer_size - out_buf.bytes;
            }

            if (qahw_sdata->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
                pa_atomic_store(&qahw_sdata->wait_for_write_ready, 1);

            if ((rc = qahw_out_write(qahw_sdata->out_handle, &out_buf)) < 0) {
                pa_log_error("Could not write data: %d", rc);
            } else if ((qahw_sdata->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) && (rc >= 0) && (rc < (int)out_buf.bytes)) {
#ifdef SINK_DEBUG
                pa_log_error("waiting for write done event");
#endif
                /* Store pending bytes to be written, write done event comes */
                out_buf.bytes = out_buf.bytes - rc;
                if (qahw_sdata->state != STATE_PLAYING)
                    qahw_sdata->state = STATE_PLAYING;
            } else {
                /* reset flag if write was successfull as it will not generate any write callback */
                if (qahw_sdata->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
                    pa_atomic_store(&qahw_sdata->wait_for_write_ready, 0);

                qahw_sdata->bytes_written += rc;
#ifdef SINK_DEBUG
                pa_log_error("write data: size %d", rc);
#endif

#ifdef SINK_DUMP_ENABLED
                if ((rc = write(qahw_sdata->write_fd, out_buf.buffer, out_buf.bytes)) < 0)
                    pa_log_error("write to fd failed %d", rc);
#endif
                /* Mark buffer as NULL, to indicate buffer has been consumed */
                out_buf.buffer = NULL;

                pa_memblock_release(chunk.memblock);
                pa_memblock_unref(chunk.memblock);

                wait = false;

                if (qahw_sdata->state != STATE_PLAYING)
                    qahw_sdata->state = STATE_PLAYING;
            }
        } else if (pa_sdata->sink->thread_info.state == PA_SINK_SUSPENDED) {
            /* if sink is suspended state then reset buffer otherwise it might end up sending incorrect buffer to qahw_write */
            memset(&out_buf, 0, sizeof(qahw_out_buffer_t));
        }

poll:
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

static int open_qahw_sink(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                          audio_output_flags_t flags, int sink_id, pa_qahw_sink_data *sdata, int32_t buffer_duration, double max_gain) {
    int rc = 0;
    qahw_sink_data *qahw_sdata;
    qahw_param_payload payload;
    int ret = -1;
    const char *bt_sco_on = "BT_SCO=on";
    const char *dsd_format = NULL;

#ifdef SINK_DUMP_ENABLED
    char *file_name;
#endif

    pa_assert(ss);
    pa_assert(map);
    pa_assert(module_handle);
    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);

    qahw_sdata = sdata->qahw_sdata;

    if (flags == AUDIO_OUTPUT_FLAG_NONE) {
        pa_log_error("Invalid flag AUDIO_OUTPUT_FLAG_NONE");
        goto exit;
    }

    if (pa_qahw_sink_fill_info(qahw_sdata, encoding, ss, map, devices, flags, sink_id, buffer_duration, max_gain)) {
        rc = -1;
        goto exit;
    }

    pa_log_debug("opening sink with configuration flag = 0x%x, encoding %d, format %d, sample_rate %d, channel_mask 0x%x device %d",
                 qahw_sdata->flags, encoding, qahw_sdata->config.format, qahw_sdata->config.sample_rate, qahw_sdata->config.channel_mask, qahw_sdata->devices);

    /* Turn BT_SCO on if bt_sco recording */
    if(audio_is_bluetooth_sco_device(qahw_sdata->devices)) {
        ret = qahw_set_parameters(module_handle, bt_sco_on);
        pa_log_info("%s: param %s set to hal with return value %d", __func__, bt_sco_on, ret);
    }

    if (buffer_duration > 0)
        qahw_sdata->config.offload_info.duration_us = buffer_duration * 1000;

    rc = qahw_open_output_stream(module_handle, qahw_sdata->handle, qahw_sdata->devices, qahw_sdata->flags, &qahw_sdata->config,
            &qahw_sdata->out_handle, qahw_sdata->device_url);
    if (rc) {
        qahw_sdata->out_handle = NULL;
        pa_log_error("Could not open output stream %d", rc);
        goto exit;
    }

    qahw_sdata->module_handle = module_handle;

    if (qahw_sdata->config.format == AUDIO_FORMAT_DSD) {
        if (qahw_sdata->dsd_rate == 64)
            dsd_format = "dsd_format=0";
        else if (qahw_sdata->dsd_rate == 128)
            dsd_format = "dsd_format=1";
        else if (qahw_sdata->dsd_rate == 256)
            dsd_format = "dsd_format=2";
        else if (qahw_sdata->dsd_rate == 512)
            dsd_format = "dsd_format=3";
        else
            dsd_format = "dsd_format=0";

        qahw_out_set_parameters(qahw_sdata->out_handle, dsd_format);
    }

    pa_log_debug("qahw sink opened %p", qahw_sdata->out_handle);

    qahw_sdata->sink_buffer_size = qahw_out_get_buffer_size(qahw_sdata->out_handle);
    if (qahw_sdata->sink_buffer_size <= 0) {
        qahw_close_output_stream(qahw_sdata->out_handle);
        pa_log_error("Invalid buffer size %zu", qahw_sdata->sink_buffer_size);
        rc = -1;
        goto exit;
    }

    if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        qahw_out_set_callback(qahw_sdata->out_handle, pa_qahw_out_cb, sdata);

    if (flags & AUDIO_OUTPUT_FLAG_FAST)
        pa_atomic_store(&qahw_sdata->set_rt_prio_for_out_cb, 1);

    /*FIXME: Add DSP latency */
    qahw_sdata->sink_latency_us = pa_bytes_to_usec(qahw_sdata->sink_buffer_size, ss);
    pa_log_debug("sink latency %dus", qahw_sdata->sink_latency_us);

    pa_atomic_store(&qahw_sdata->wait_for_write_ready, 0);
    pa_atomic_store(&qahw_sdata->wait_for_drain_ready, 0);

    payload.channel_map_params = qahw_sdata->qahw_map;
    rc = qahw_out_set_param_data(qahw_sdata->out_handle,
                                QAHW_PARAM_OUT_CHANNEL_MAP,
                                &payload);
    if (rc) {
        qahw_close_output_stream(qahw_sdata->out_handle);
        pa_log_error("Invalid buffer size %zu", qahw_sdata->sink_buffer_size);
        rc = -1;
        goto exit;
    }

#ifdef SINK_DUMP_ENABLED
    file_name = pa_sprintf_malloc("/data/pcmdump_sink_%d", qahw_sdata->handle);

    qahw_sdata->write_fd = open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
    if(qahw_sdata->write_fd < 0)
        pa_log_error("Could not open write fd %d for sink index %d", qahw_sdata->write_fd, qahw_sdata->handle);

    pa_xfree(file_name);
#endif

    qahw_sdata->ts_log = TRACE_LOG_STATIC_INIT;

exit:
    return rc;
}

static int close_qahw_sink(pa_qahw_sink_data *sdata) {
    qahw_sink_data *qahw_sdata;
    int rc = -1;
    int ret = -1;
    const char *bt_sco_off = "BT_SCO=off";
    char *kvpair = NULL;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->qahw_sdata->module_handle);

    qahw_sdata = sdata->qahw_sdata;

    pa_assert(qahw_sdata->out_handle);

    pa_log_debug("closing qahw sink %p", qahw_sdata->out_handle);

    if (PA_UNLIKELY(qahw_sdata->out_handle == NULL)) {
        pa_log_error("Invalid sink handle %p", qahw_sdata->out_handle);
    } else {
        rc = qahw_close_output_stream(qahw_sdata->out_handle);
        if (PA_UNLIKELY(rc))
            pa_log_error(" could not close sink sink handle %p, error  %d", qahw_sdata->out_handle, rc);

        qahw_sdata->out_handle = NULL;
    }

#ifdef SINK_DUMP_ENABLED
    close(qahw_sdata->write_fd);
#endif

    trace_close(&qahw_sdata->ts_log);

    /* Turn BT_SCO off if bt_sco recording */
    if(audio_is_bluetooth_sco_device(qahw_sdata->devices)) {
        ret = qahw_set_parameters(qahw_sdata->module_handle, bt_sco_off);
        pa_log_info("%s: param %s set to hal with return value %d", __func__, bt_sco_off, ret);
    }

    if (qahw_sdata->devices & AUDIO_DEVICE_OUT_HDMI) {
        kvpair = pa_sprintf_malloc("%s=%d", QAHW_PARAMETER_DEVICE_DISCONNECT, qahw_sdata->devices);

        rc = qahw_set_parameters(qahw_sdata->module_handle, kvpair);
        if (rc)
            pa_log_error("qahw_set_parameters failed %d",rc);

        pa_xfree(kvpair);
    }

    return rc;
}

static int restart_qahw_sink(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                              audio_output_flags_t flags, int sink_id, pa_qahw_sink_data *sdata) {
    int rc;
    qahw_sink_data *qahw_sdata;

    pa_assert(sdata->qahw_sdata);

    pa_log_info("%s", __func__);
    qahw_sdata = sdata->qahw_sdata;
    pa_atomic_store(&qahw_sdata->restart_in_progress, 1);

    rc = close_qahw_sink(sdata);
    if (rc) {
        pa_log_error("close_qahw_sink failed, error %d", rc);
        goto exit;
    }

    rc = open_qahw_sink(module_handle, encoding, ss, map, devices, flags, sink_id, sdata, sdata->qahw_sdata->buffer_duration, sdata->qahw_sdata->max_gain);
    if (rc) {
        pa_log_error("open_qahw_sink failed during recreation, error %d", rc);
    }

exit:
    pa_atomic_store(&qahw_sdata->restart_in_progress, 0);
    return rc;
}

static int free_qahw_sink(pa_qahw_sink_data *sdata) {
    int rc;

    pa_assert(sdata);

    rc = close_qahw_sink(sdata);
    if (rc) {
        pa_log_error("close_qahw_sink failed, error %d", rc);
    }

    pa_xfree(sdata->qahw_sdata);
    sdata->qahw_sdata = NULL;

    return rc;
}

static int create_qahw_sink(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                            audio_output_flags_t flags, int sink_id, pa_qahw_sink_data *sdata, int32_t buffer_duration, double max_gain) {
   int rc;

   sdata->qahw_sdata = pa_xnew0(qahw_sink_data, 1);

   rc = open_qahw_sink(module_handle, encoding, ss, map, devices, flags, sink_id, sdata, buffer_duration, max_gain);
   if (rc) {
       pa_log_error("open_qahw_sink failed, error %d", rc);
       pa_xfree(sdata->qahw_sdata);
       sdata->qahw_sdata = NULL;
   }

    return rc;
}

static int pa_qahw_sink_free_common_resources(pa_qahw_sink_data *sdata) {
    if (sdata->fdsem)
        pa_fdsem_free(sdata->fdsem);

    return 0;
}

static int pa_qahw_sink_alloc_common_resources(pa_qahw_sink_data *sdata) {
   sdata->fdsem = pa_fdsem_new();
   if (!sdata->fdsem) {
       pa_log_error("Could not create fdsem");
       return -1;
   }

   return 0;
}

static int create_pa_sink(pa_module *m, char *sink_name, char *description, pa_idxset *formats, pa_sample_spec *ss, pa_channel_map *map, bool use_hw_volume,
                          uint32_t alternate_sample_rate, pa_qahw_card_avoid_processing_config_id_t avoid_config_processing, pa_card *card, pa_hashmap *ports,
                          const char *driver, pa_qahw_sink_data *sdata, pa_proplist *proplist) {
    pa_sink_new_data new_data;
    pa_sink_data *pa_sdata;
    pa_device_port *port;
    pa_format_info *format;
    pa_format_info *in_format;

    void *state;
    uint32_t i;

    bool port_sink_mapping = false;

    pa_assert(sdata->qahw_sdata);

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

    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_STRING, pa_qahw_sink_get_name_from_flags(sdata->qahw_sdata->flags));
    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_DESCRIPTION, description);

    if (avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_ALL)
        new_data.avoid_processing = true;
    else
        new_data.avoid_processing = false;

    if (proplist)
        pa_proplist_update(new_data.proplist, PA_UPDATE_REPLACE, proplist);

    pa_sdata->sink = pa_sink_new(m->core, &new_data, PA_SINK_HARDWARE | PA_SINK_LATENCY );
    pa_sink_new_data_done(&new_data);

    if (!pa_sdata->sink) {
        pa_log_error("Could not create pa sink");
        goto fail;
    }

    pa_log_debug("pa sink opened %p", pa_sdata->sink);

    pa_sdata->sink->userdata = (void *)sdata;
    pa_sdata->sink->parent.process_msg = pa_qahw_sink_process_msg;
    pa_sdata->sink->set_state_in_io_thread = pa_qahw_sink_set_state_in_io_thread_cb;
    pa_sdata->sink->set_port = pa_qahw_sink_set_port_cb;
    pa_sdata->sink->reconfigure = pa_qahw_sink_reconfigure_cb;
    pa_sdata->sink->set_format = pa_qahw_sink_set_format_cb;
    pa_sdata->sink->drain = pa_qahw_sink_drain_cb;
    pa_sdata->sink->flush = pa_qahw_sink_flush_cb;

    pa_sdata->avoid_config_processing = avoid_config_processing;

    if (pa_idxset_size(formats) > 0 ) {
        pa_sdata->sink->get_formats = pa_qahw_sink_get_formats;

        pa_sdata->formats = pa_idxset_new(NULL, NULL);

        PA_IDXSET_FOREACH(in_format, formats, i) {
            format = pa_format_info_copy(in_format);
            pa_idxset_put(pa_sdata->formats, format, NULL);
        }
    }

    pa_sink_set_asyncmsgq(pa_sdata->sink, pa_sdata->thread_mq.inq);
    pa_sink_set_rtpoll(pa_sdata->sink, pa_sdata->rtpoll);

    pa_sink_set_max_request(pa_sdata->sink, sdata->qahw_sdata->sink_buffer_size);
    pa_sink_set_max_rewind(pa_sdata->sink, 0);
    pa_sink_set_fixed_latency(pa_sdata->sink, sdata->qahw_sdata->sink_latency_us);

    if (use_hw_volume) {
        pa_sdata->sink->n_volume_steps = 15; /* FIXME: What should be value */
        pa_sink_set_set_volume_callback(pa_sdata->sink, pa_qahw_sink_set_volume_cb);
    }

   pa_sdata->rtpoll_item = pa_rtpoll_item_new_fdsem(pa_sdata->rtpoll, PA_RTPOLL_NORMAL, sdata->fdsem);
   if (!pa_sdata->rtpoll_item) {
       pa_log_error("Could not create rpoll item");
       goto fail;
   }

    pa_sdata->thread = pa_thread_new(sink_name, pa_qahw_sink_thread_func, sdata);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

   /* keep pa sink and qahw port in sync, qahw is opened with some default port, update qahw with active port decided by pa sink */
   pa_qahw_sink_set_port_cb(pa_sdata->sink, pa_sdata->sink->active_port);

   pa_sink_put(pa_sdata->sink);

   return 0;

fail :
    if (pa_sdata)
        free_pa_sink(sdata);

    return -1;
}

static int free_pa_sink(pa_qahw_sink_data *sdata) {
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

    if (pa_sdata->rtpoll_item)
        pa_rtpoll_item_free(pa_sdata->rtpoll_item);

    if (pa_sdata->formats)
        pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);

    if (pa_sdata->rtpoll)
        pa_rtpoll_free(pa_sdata->rtpoll);

    pa_xfree(pa_sdata);

    return 0;
}

bool pa_qahw_sink_is_primary(audio_output_flags_t flags) {
    if (flags == AUDIO_OUTPUT_FLAG_PRIMARY)
        return true;
    else
        return false;
}

pa_idxset* pa_qahw_sink_get_config(pa_qahw_sink_handle_t *handle) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->sink);

    return pa_qahw_sink_get_formats(sdata->pa_sdata->sink);
}

bool pa_qahw_sink_is_supported_sample_rate(uint32_t sample_rate) {
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

int pa_qahw_sink_get_index(pa_qahw_sink_handle_t *handle) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)handle;
    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    return sdata->pa_sdata->sink->index;
}

char *pa_qahw_sink_get_name_from_pa_sink_id(uint32_t sink_id) {
    pa_qahw_sink_data *sdata;
    uint32_t idx;

    pa_assert(mdata);

    PA_IDXSET_FOREACH(sdata, mdata->sinks, idx) {
        if ((sdata->pa_sdata) && (sdata->pa_sdata->sink->index == sink_id))
            return sdata->pa_sdata->sink->name;
    }

    pa_log_error("%s: No sink with pa sink id %d", __func__, sink_id);
    return NULL;
}

audio_io_handle_t pa_qahw_sink_get_io_handle(uint32_t sink_id) {
    pa_qahw_sink_data *sdata;
    uint32_t idx;

    pa_assert(mdata);

    PA_IDXSET_FOREACH(sdata, mdata->sinks, idx) {
        if ((sdata->pa_sdata) && (sdata->pa_sdata->sink->index == sink_id))
            return sdata->qahw_sdata->handle;
    }

    pa_log_error("%s: No sink with pa sink id %d", __func__, sink_id);
    return -1;
}


int pa_qahw_sink_create(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name, pa_qahw_sink_config *sink,
                        pa_qahw_sink_handle_t **handle) {
    int rc = -1;
    pa_qahw_sink_data *sdata;
    pa_device_port *card_port;
    pa_qahw_card_port_config *sink_port;
    pa_hashmap *ports;
    pa_qahw_card_port_device_data *port_device_data;

    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    void *state;

    pa_assert(m);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module_handle);
    pa_assert(module_name);
    pa_assert(sink);
    pa_assert(sink->name);
    pa_assert(sink->description);
    pa_assert(sink->formats);
    pa_assert(sink->type);
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

    sdata = pa_xnew0(pa_qahw_sink_data, 1);

    rc = pa_qahw_sink_alloc_common_resources(sdata);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could pa_qahw_sink_alloc_common_resources, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_qahw_sink(module_handle, sink->default_encoding, &sink->default_spec, &sink->default_map, port_device_data->device, sink->flags, sink->id, sdata, sink->buffer_duration,
                          sink->max_gain);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could create open qahw sink, error %d", rc);
        pa_qahw_sink_free_common_resources(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_pa_sink(m, sink->name, sink->description, sink->formats, &sink->default_spec, &sink->default_map, sink->use_hw_volume,
                        sink->alternate_sample_rate, sink->avoid_config_processing, card, ports, driver, sdata, sink->proplist);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa sink for sink %s, error %d", sink->name, rc);
        free_qahw_sink(sdata);
        pa_qahw_sink_free_common_resources(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = pa_qahw_sink_extn_create(sdata->pa_sdata->sink->core, sdata->qahw_sdata->out_handle, sdata->pa_sdata->sink->index, &sdata->sink_extn_handle);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create qahw sink extn %s, error %d", sink->name, rc);
        free_qahw_sink(sdata);
        pa_qahw_sink_free_common_resources(sdata);
        free_pa_sink(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    *handle = (pa_qahw_sink_handle_t *)sdata;
    pa_idxset_put(mdata->sinks, sdata, NULL);

exit:
    if (ports)
        pa_hashmap_free(ports);
    return rc;
}

void pa_qahw_sink_close(pa_qahw_sink_handle_t *handle) {
    pa_qahw_sink_data *sdata = (pa_qahw_sink_data *)handle;

    pa_assert(sdata);

    pa_qahw_sink_extn_free(sdata->sink_extn_handle);
    free_pa_sink(sdata);
    free_qahw_sink(sdata);
    pa_qahw_sink_free_common_resources(sdata);

    pa_idxset_remove_by_data(mdata->sinks, sdata, NULL);

    pa_xfree(sdata);
}

void pa_qahw_sink_module_deinit() {

    pa_assert(mdata);

    pa_idxset_free(mdata->sinks, NULL);

    pa_xfree(mdata);
    mdata = NULL;
}

void pa_qahw_sink_module_init() {

    mdata = pa_xnew0(pa_qahw_sink_module_data, sizeof(pa_qahw_sink_module_data));

    mdata->sinks = pa_idxset_new(NULL, NULL);
}
