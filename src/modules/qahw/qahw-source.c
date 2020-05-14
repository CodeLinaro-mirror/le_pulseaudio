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
#include <errno.h>
#include <unistd.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/source.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/core-format.h>
#include <pulsecore/trace_log.h>
#include <pulse/util.h>

#include "qahw-source.h"
#include "qahw-utils.h"
#include "qahw-source-extn.h"
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
#define AUDIO_IN_VALID_CH_COUNT_FOR_CH_MASK 8

#define PA_DEFAULT_STARTUP_LATENCY_MS 100

//#define SOURCE_DUMP_ENABLED

typedef enum {
    PA_QAHW_SOURCE_READ_EVENT_DONE = PA_SOURCE_MESSAGE_MAX + 1,
} qahw_read_event_t;

typedef struct {
    qahw_stream_handle_t *in_handle;
    audio_io_handle_t handle;
    qahw_module_handle_t *module_handle;

    uint32_t devices;
    audio_input_flags_t flags;
    audio_config_t config;

    const char *device_url;
    int write_fd;

    size_t source_buffer_size;
    audio_source_t source_type;
    uint32_t source_latency_us;
    pa_thread *qahw_read_thread;
    pa_atomic_t stopped;

    int32_t buffer_duration;
    int32_t preemph_status;
    uint32_t dsd_rate;
    pa_atomic_t first_read;
    pa_qahw_card_qahw_processing_id_t qahw_processing_id;

    trace_log ts_log;
} qahw_source_data;

typedef struct {
    bool first;
    pa_source *source;
    pa_rtpoll *rtpoll;
    pa_thread_mq thread_mq;
    pa_thread *thread;
    pa_idxset *formats;

    pa_qahw_card_avoid_processing_config_id_t avoid_config_processing;
} pa_source_data;

typedef struct {
    qahw_source_data *qahw_sdata;
    pa_source_data *pa_sdata;
    pa_qahw_source_extn_handle_t *source_extn_handle;
} pa_qahw_source_data;

typedef struct {
    audio_source_t qahw_source_type;
    char *qahw_source_name;
} pa_qahw_source_name_to_enum_mapping;

pa_qahw_source_name_to_enum_mapping source_name_to_enum[] = {
    { AUDIO_SOURCE_MIC,                 (char*)"AUDIO_SOURCE_MIC" },
    { AUDIO_SOURCE_UNPROCESSED,         (char*)"AUDIO_SOURCE_UNPROCESSED" },
};

static int restart_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                              audio_input_flags_t flags, int source_id, qahw_source_data *qahw_sdata);
static int create_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                                                    audio_input_flags_t flags, int source_id, pa_qahw_source_data *sdata, audio_source_t source_type,
                            int32_t buffer_duration, int32_t preemph_status, pa_qahw_card_qahw_processing_id_t qahw_processing_id, uint32_t dsd_rate);
static int open_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                           audio_input_flags_t flags, int source_id, qahw_source_data *qahw_sdata, audio_source_t source_type,
                           int32_t buffer_duration, pa_qahw_card_qahw_processing_id_t qahw_processing_id);

static int close_qahw_source(qahw_source_data *qahw_sdata);
static int stop_qahw_source(qahw_source_data *qahw_sdata);
static void pa_qahw_source_read_thread_func(void *userdata);

static const uint32_t supported_source_rates[] =
                          {8000, 11025, 16000, 22050, 32000, 44100, 48000, 96000, 192000};

static pa_sample_format_t pa_qahw_source_find_nearest_supported_pa_format(pa_sample_format_t format) {
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
            break;
    }

    return format1;
}

static uint32_t pa_qahw_source_find_nearest_supported_sample_rate(uint32_t sample_rate) {
    uint32_t i;
    uint32_t nearest_rate = PA_DEFAULT_SOURCE_RATE;

    for (i = 0; i < ARRAY_SIZE(supported_source_rates) ; i++) {
        if (sample_rate == supported_source_rates[i]) {
            nearest_rate = sample_rate;
            break;
        } else if (sample_rate > supported_source_rates[i]) {
            nearest_rate = supported_source_rates[i];
        }
    }

    return nearest_rate;
}

audio_source_t pa_qahw_source_name_to_enum(const char *source_name) {
    uint32_t count;
    audio_source_t source_type = AUDIO_SOURCE_DEFAULT;

    pa_assert(source_name);

    for (count = 0; count < ARRAY_SIZE(source_name_to_enum); count++) {
        if (pa_streq(source_name, source_name_to_enum[count].qahw_source_name)) {
            source_type = source_name_to_enum[count].qahw_source_type;
            break;
        }
    }

    pa_log_debug("%s: source_name %s qahw source %u", __func__, source_name, source_type);

    return source_type;
}

static const char *pa_qahw_source_get_name_from_flags(audio_input_flags_t flags) {
    const char *name = NULL;

    if (flags == AUDIO_INPUT_FLAG_NONE)
        name = "regular";
    else if (flags == AUDIO_INPUT_FLAG_FAST)
        name = "low-latency";
    else if (flags == (QAHW_INPUT_FLAG_TIMESTAMP | QAHW_INPUT_FLAG_COMPRESS))
        name = "compress-with-timestamp";
    else if (flags == (QAHW_INPUT_FLAG_PASSTHROUGH | QAHW_INPUT_FLAG_COMPRESS))
        name = "passthrough";
    else if (flags == QAHW_INPUT_FLAG_COMPRESS)
        name = "compress";
    else if (flags == (QAHW_INPUT_FLAG_TIMESTAMP | QAHW_INPUT_FLAG_COMPRESS | AUDIO_INPUT_FLAG_FAST))
        name = "compress-with-timestamp-fast-flag";
    else if (flags == (QAHW_INPUT_FLAG_PASSTHROUGH | QAHW_INPUT_FLAG_COMPRESS))
        name = "compress-with-pasthrough";
    else if (flags == (QAHW_INPUT_FLAG_COMPRESS | AUDIO_INPUT_FLAG_FAST))
        name = "compress-with-fast-flag";
    else if (flags == (QAHW_INPUT_FLAG_TIMESTAMP | QAHW_INPUT_FLAG_COMPRESS | QAHW_INPUT_FLAG_PASSTHROUGH))
        name = "compress-passthrough-with-timestamp";
    else if (flags == (QAHW_INPUT_FLAG_PASSTHROUGH | QAHW_INPUT_FLAG_COMPRESS | AUDIO_INPUT_FLAG_FAST))
        name = "compress-pasthrough-with-fast-flag";
    else if (flags == (QAHW_INPUT_FLAG_TIMESTAMP | QAHW_INPUT_FLAG_PASSTHROUGH | QAHW_INPUT_FLAG_COMPRESS | AUDIO_INPUT_FLAG_FAST))
        name = "compress-pasthrough-with-timestamp-fast-flag";

    return name;
}

static void pa_qahw_source_fill_info(qahw_source_data *qahw_sdata, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                                            audio_input_flags_t flags, int source_iohandle, audio_source_t source_type, int32_t buffer_duration,
                                            pa_qahw_card_qahw_processing_id_t qahw_processing_id) {
    qahw_sdata->source_type = source_type;
    qahw_sdata->buffer_duration = buffer_duration;

    if (encoding == PA_ENCODING_PCM)
        qahw_sdata->config.format = pa_qahw_util_get_qahw_format_from_pa_sample(ss->format);
    else
        qahw_sdata->config.format = pa_qahw_util_get_qahw_format_from_pa_encoding(encoding);

    /* convert to media rate to transmission rate, as pa expects same for PA_ENCODING_UNKNOWN_4X_IEC61937
       transmission rate  = media_rate * 4
    */
    if (encoding == PA_ENCODING_UNKNOWN_4X_IEC61937)
        ss->rate = ss->rate * 4;

    qahw_sdata->config.sample_rate = ss->rate;

    /* default input channel mask is defined only till channel count 8 in audio.h. If it exceeds 8,
       then INVALID mask is returned. Add a pa_qahw_util function to derive channel mask if count exceeds 8.
     */
    if (ss->channels > AUDIO_IN_VALID_CH_COUNT_FOR_CH_MASK) {
        qahw_sdata->config.channel_mask = pa_qahw_util_in_mask_from_count(ss->channels);
    } else {
        qahw_sdata->config.channel_mask = audio_channel_in_mask_from_count(ss->channels);
    }

    /* DIRECT PCM uses offload structure */
    if (flags & QAHW_INPUT_FLAG_COMPRESS) {
        qahw_sdata->config.offload_info = AUDIO_INFO_INITIALIZER;
        qahw_sdata->config.offload_info.format = qahw_sdata->config.format;
        qahw_sdata->config.offload_info.sample_rate = qahw_sdata->config.sample_rate;
        qahw_sdata->config.offload_info.channel_mask = qahw_sdata->config.channel_mask;
    }

    qahw_sdata->devices = devices;
    qahw_sdata->flags = flags;
    qahw_sdata->handle = source_iohandle;
    qahw_sdata->qahw_processing_id = qahw_processing_id;
}

static int pa_qahw_source_start(pa_qahw_source_data *sdata) {

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);

    pa_atomic_store(&sdata->qahw_sdata->stopped, 0);
    pa_atomic_store(&sdata->qahw_sdata->first_read, 0);
    if (!(sdata->qahw_sdata->qahw_read_thread = pa_thread_new(sdata->pa_sdata->source->name, pa_qahw_source_read_thread_func, sdata))) {
        pa_log_error("%s: qahw_read_thread creation failed", __func__);
    }

    trace_newstream(&sdata->qahw_sdata->ts_log, sdata->pa_sdata->source->name);

    return 0;
}

static int pa_qahw_source_standby(pa_qahw_source_data *sdata) {
    qahw_source_data *qahw_sdata;

    pa_assert(sdata);

    qahw_sdata = sdata->qahw_sdata;

    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->in_handle);

    pa_log_info("%s", __func__);

    stop_qahw_source(sdata->qahw_sdata);
    qahw_in_standby(qahw_sdata->in_handle);

    trace_close(&sdata->qahw_sdata->ts_log);

    return 0;
}

static int pa_qahw_source_set_port_cb(pa_source *s, pa_device_port *p) {

    pa_qahw_card_port_device_data *port_device_data;
    char *kvpair = NULL;
    pa_qahw_source_data *source_data = (pa_qahw_source_data *)s->userdata;
    int rc;

    pa_assert(source_data);
    pa_assert(source_data->qahw_sdata);
    pa_assert(source_data->qahw_sdata->in_handle);

    port_device_data = PA_DEVICE_PORT_DATA(p);
    pa_assert(port_device_data);

    kvpair = pa_sprintf_malloc("%s=%u", QAHW_PARAMETER_STREAM_ROUTING, port_device_data->device);
    pa_log_info("port name: %s kvpair %s device 0x%x", p->name, kvpair, port_device_data->device);

    rc = qahw_in_set_parameters(source_data->qahw_sdata->in_handle, kvpair);
    if (rc)
        pa_log_error("qahw in routing failed %d",rc);

    pa_xfree(kvpair);

    return rc;
}

static int pa_qahw_source_set_state_in_io_thread_cb(pa_source *s, pa_source_state_t new_state, pa_suspend_cause_t new_suspend_cause PA_GCC_UNUSED)
{
    pa_qahw_source_data *source_data = (pa_qahw_source_data *)(s->userdata);
    int r = 0;

    pa_log_debug("New state is: %d", new_state);

    if (PA_SOURCE_IS_OPENED(new_state) && !PA_SOURCE_IS_OPENED(s->thread_info.state))
        r = pa_qahw_source_start(source_data);
    else if (new_state == PA_SOURCE_SUSPENDED)
        r = pa_qahw_source_standby(source_data);

    return r;
}

static int pa_qahw_source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    pa_qahw_source_data *source_data = (pa_qahw_source_data *)(PA_SOURCE(o)->userdata);

    pa_assert(source_data);
    pa_assert(source_data->pa_sdata->source);

    switch (code) {
        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            *((pa_usec_t*) data) = 0;
            return 0;
        }
        case PA_QAHW_SOURCE_READ_EVENT_DONE: {
#ifdef SOURCE_DUMP_ENABLED
             pa_log_debug("%s: chunk length %d chunk index %d ", __func__, chunk->length, chunk->index);
#endif

             pa_source_post(source_data->pa_sdata->source, chunk);
             pa_memblock_unref(chunk->memblock);
             return 0;
        }

        default:
             break;
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static int pa_qahw_source_reconfigure_cb(pa_source *s, pa_sample_spec *spec, pa_channel_map *map, bool passthrough) {
    pa_qahw_source_data *sdata = (pa_qahw_source_data *) s->userdata;
    pa_encoding_t encoding = PA_ENCODING_PCM;
    pa_source_data *pa_sdata = NULL;
    qahw_source_data *qahw_sdata = NULL;
    pa_format_info *format = NULL;
    uint32_t i;

    char channel_map_buf[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_channel_map new_map;
    pa_sample_spec tmp_spec;

    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    int rc = -1;

    pa_assert(s);
    pa_assert(s->userdata);
    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(spec);

    pa_log_info("%s", __func__);

    pa_sdata = sdata->pa_sdata;
    qahw_sdata = sdata->qahw_sdata;
    tmp_spec = *spec;

    if (!PA_SOURCE_IS_OPENED(s->state)) {
        pa_log_info("%s: old sample spec %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &pa_sdata->source->sample_spec));
        pa_log_info("%s: requested sample spec %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &tmp_spec));

        if (sdata->pa_sdata->formats) {
            PA_IDXSET_FOREACH(format, sdata->pa_sdata->formats, i) {
                if (format->encoding != PA_ENCODING_PCM)
                    goto exit;
            }
        }

        if (map) {
            pa_log_info("%s:old channel map %s", __func__, pa_channel_map_snprint(channel_map_buf, sizeof(channel_map_buf), &pa_sdata->source->channel_map));
            pa_log_info("%s:requested channel map %s", __func__, pa_channel_map_snprint(channel_map_buf, sizeof(channel_map_buf), map));

            if (pa_sdata->avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_CHANNELS)
                new_map = *map;
            else {
                new_map = pa_sdata->source->channel_map;
                tmp_spec.channels = pa_sdata->source->sample_spec.channels;
            }
        } else {
            if (pa_sdata->avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_CHANNELS)
                pa_channel_map_init_auto(&new_map, tmp_spec.channels, PA_CHANNEL_MAP_DEFAULT);
            else {
                new_map = pa_sdata->source->channel_map;
                tmp_spec.channels = pa_sdata->source->sample_spec.channels;
            }
        }

        qahw_sdata->devices = *((audio_devices_t *)PA_DEVICE_PORT_DATA(pa_sdata->source->active_port));
        /* find nearest suitable format */
        if (pa_sdata->avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_BIT_WIDTH)
            tmp_spec.format = pa_qahw_source_find_nearest_supported_pa_format(spec->format);
        else
            tmp_spec.format = pa_sdata->source->sample_spec.format;

        /* find nearest suitable rate */
        if (pa_sdata->avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_SAMPLE_RATE)
            tmp_spec.rate = pa_qahw_source_find_nearest_supported_sample_rate(spec->rate);
        else
            tmp_spec.rate = pa_sdata->source->sample_spec.rate;

        pa_log_info("%s: trying to reconfigure qahw with sample spec %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &tmp_spec));

        rc = restart_qahw_source(qahw_sdata->module_handle, encoding, &tmp_spec, &new_map, qahw_sdata->devices, qahw_sdata->flags,
                qahw_sdata->handle, qahw_sdata);
        if (rc) {
            pa_log_error("%s: could not create qahw source with requested conf, error %d, restoring old conf", __func__, rc);
            rc = open_qahw_source(qahw_sdata->module_handle, encoding, &pa_sdata->source->sample_spec, &pa_sdata->source->channel_map, qahw_sdata->devices,
                                  qahw_sdata->flags, qahw_sdata->handle, qahw_sdata, qahw_sdata->source_type,
                                  qahw_sdata->buffer_duration, qahw_sdata->qahw_processing_id);
            if (rc)
                pa_log_info("%s: restoring of qahw source with old config failed, error %d", __func__, rc);

            goto exit;
        }

        pa_sdata->source->sample_spec = tmp_spec;
        pa_sdata->source->channel_map = new_map;

        pa_qahw_source_extn_source_handle_update(sdata->source_extn_handle, qahw_sdata->in_handle);

        pa_source_set_fixed_latency(pa_sdata->source, qahw_sdata->source_latency_us);
        return 0;
    }

exit:
    return rc;
}

int pa_qahw_source_set_param(pa_qahw_source_handle_t *handle, const char *param) {
    int ret = -1;
    pa_qahw_source_data *sdata = NULL;

    pa_assert(handle);

    sdata = (pa_qahw_source_data *)handle;

    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->qahw_sdata->module_handle);

    ret = qahw_set_parameters(sdata->qahw_sdata->module_handle, param);
    pa_log_info("%s: param %s set to hal with return value %d", __func__, param, ret);

    return ret;
}

int pa_qahw_source_get_media_config(pa_qahw_source_handle_t *handle, pa_sample_spec *ss, pa_channel_map *map, pa_encoding_t *encoding) {
    pa_qahw_source_data *sdata = (pa_qahw_source_data *)handle;
    pa_format_info *f;

    uint32_t i;
    int ret = -1;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->source);

    *ss = sdata->pa_sdata->source->sample_spec;
    *map = sdata->pa_sdata->source->channel_map;

    PA_IDXSET_FOREACH(f, sdata->pa_sdata->formats, i) {
        /* currently a source supports single format */
        *encoding = f->encoding;
        ret = 0;
        break;
    }

    return ret;
}

static pa_idxset* pa_qahw_source_get_formats(pa_source *s) {
    pa_qahw_source_data *sdata = (pa_qahw_source_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);

    return pa_idxset_copy(sdata->pa_sdata->formats, (pa_copy_func_t) pa_format_info_copy);
}

static void pa_qahw_source_read_thread_func(void *userdata) {
    pa_qahw_source_data *source_data = (pa_qahw_source_data *)userdata;
    pa_source_data *pa_sdata = source_data->pa_sdata;
    qahw_source_data *qahw_sdata = source_data->qahw_sdata;
#ifdef SOURCE_DUMP_ENABLED
    pa_usec_t cur_qtimer, ticks = 0;
#endif

    if ((pa_sdata->source->core->realtime_scheduling)) {
        pa_log_info("%s:: Making read thread for %s as realtime with prio %d", __func__,
                    pa_qahw_source_get_name_from_flags(qahw_sdata->flags),
                    pa_sdata->source->core->realtime_priority);
        pa_make_realtime(pa_sdata->source->core->realtime_priority);
    }

    pa_log_debug("Source Qahw Read Thread starting up");

    for (;;) {
        pa_memchunk chunk;
        int ret = 0;
        qahw_in_buffer_t in_buf;
        void *data;


        memset(&in_buf, 0, sizeof(qahw_in_buffer_t));

        pa_memchunk_reset(&chunk);
        chunk.memblock = pa_memblock_new(pa_sdata->source->core->mempool, (size_t) qahw_sdata->source_buffer_size);
        data = pa_memblock_acquire(chunk.memblock);
        chunk.length = pa_memblock_get_length(chunk.memblock);

        in_buf.buffer = data;
        in_buf.bytes = chunk.length;
        if (qahw_sdata->flags & QAHW_INPUT_FLAG_TIMESTAMP)
            in_buf.timestamp = (int64_t *)&chunk.timestamp;

        if (!pa_atomic_load(&qahw_sdata->stopped)) {
            if ((ret = qahw_in_read(qahw_sdata->in_handle, &in_buf)) <= 0) {
                pa_log_error("qahw_in_read failed, ret = %d, qahw handle %p, sleeping for %lldms",
                        ret, qahw_sdata->in_handle, pa_bytes_to_usec(in_buf.bytes, &pa_sdata->source->sample_spec)/1000);
                pa_msleep(pa_bytes_to_usec(in_buf.bytes, &pa_sdata->source->sample_spec)/1000);
                ret = in_buf.bytes;
            }

            if (qahw_sdata->flags & QAHW_INPUT_FLAG_TIMESTAMP) {
                chunk.timestamp = chunk.timestamp * PA_NSEC_PER_USEC;
#ifdef SOURCE_DUMP_ENABLED
#if defined __aarch64__
                asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
#else
                asm volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r"(ticks));
#endif
                cur_qtimer = ticks * 10/192;
                pa_log_debug("read_timestamp %" PRId64 "nsec read_cur_timestamp %" PRId64 "usec", chunk.timestamp, cur_qtimer);
#endif
                trace_ts(&qahw_sdata->ts_log, pa_sdata->source->name, chunk.timestamp, chunk.duration, chunk.length);
            }
            pa_atomic_store(&qahw_sdata->first_read, 1);
        } else {
            pa_memblock_release(chunk.memblock);
            pa_memblock_unref(chunk.memblock);
            goto finish;
        }

        chunk.length = ret;
#ifdef SOURCE_DUMP_ENABLED
        if ((ret = write(qahw_sdata->write_fd, in_buf.buffer, ret)) < 0)
            pa_log_error("write to fd failed %d", ret);
#endif
        pa_memblock_release(chunk.memblock);
        pa_asyncmsgq_post(pa_sdata->thread_mq.inq, PA_MSGOBJECT(pa_sdata->source), PA_QAHW_SOURCE_READ_EVENT_DONE, NULL, 0, &chunk,NULL);
    }


finish:
    pa_log_debug("Source QAHW Read Thread shutting down");
}

static void pa_qahw_source_thread_func(void *userdata) {
    pa_qahw_source_data *source_data = (pa_qahw_source_data *)userdata;
    pa_source_data *pa_sdata = source_data->pa_sdata;
    qahw_source_data *qahw_sdata = source_data->qahw_sdata;
    pa_usec_t timeout = qahw_sdata->source_latency_us * 2;
    bool timer_enabled = false;

    if ((pa_sdata->source->core->realtime_scheduling)) {
        pa_log_info("%s:: Making io thread for %s as realtime with prio %d", __func__,
                    pa_qahw_source_get_name_from_flags(qahw_sdata->flags),
                    pa_sdata->source->core->realtime_priority);
        pa_make_realtime(pa_sdata->source->core->realtime_priority);
    }

    pa_log_debug("Source IO Thread starting up");

    pa_thread_mq_install(&pa_sdata->thread_mq);

    for (;;) {
        int ret = 0;

        /* Start timer */
        if (PA_SOURCE_IS_OPENED(pa_sdata->source->thread_info.state)) {
            if (!pa_atomic_load(&qahw_sdata->first_read))
                timeout += (PA_DEFAULT_STARTUP_LATENCY_MS * 1000);

            pa_rtpoll_set_timer_relative(pa_sdata->rtpoll, timeout);
            timer_enabled = true;
        }

        /* nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(pa_sdata->rtpoll, true)) < 0)
            goto fail;

        /* Check whether timer has elapsed *
         * This case occur when read() gets blocked in alsa */
        if (timer_enabled) {
            if (pa_rtpoll_timer_elapsed(pa_sdata->rtpoll)) {
                if (source_data->qahw_sdata) {
                    pa_log_info("%s: timer exceeded. unblock read() by calling stop()", __func__);
                    qahw_in_stop(qahw_sdata->in_handle);
                }
            }

            pa_rtpoll_set_timer_disabled(pa_sdata->rtpoll);
            timer_enabled = false;
        }

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

static int open_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                  audio_input_flags_t flags, int source_id, qahw_source_data *qahw_sdata, audio_source_t source_type, int32_t buffer_duration,
                  pa_qahw_card_qahw_processing_id_t qahw_processing_id) {
    int rc;
    int ret = -1;
    const char *bt_sco_on = "BT_SCO=on";
    const char *dsd_format = NULL;

#ifdef SOURCE_DUMP_ENABLED
    char *file_name;
#endif

    pa_assert(ss);
    pa_assert(map);
    pa_assert(module_handle);
    pa_assert(qahw_sdata);

    pa_qahw_source_fill_info(qahw_sdata, encoding, ss, map, devices, flags, source_id, source_type, buffer_duration, qahw_processing_id);

#ifdef SOURCE_DUMP_ENABLED
    file_name = pa_sprintf_malloc("/data/pcmdump_source_%d", qahw_sdata->handle);

    qahw_sdata->write_fd = open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
    if(qahw_sdata->write_fd < 0)
        pa_log_error("Could not open write fd %d for source index %d", qahw_sdata->write_fd, qahw_sdata->handle);

    pa_xfree(file_name);
#endif

    pa_log_debug("opening source with configuration flag = 0x%x, encoding %d,format %d, sample_rate %d, channel_mask 0x%x device 0x%x",
                 qahw_sdata->flags, encoding, qahw_sdata->config.format, qahw_sdata->config.sample_rate, qahw_sdata->config.channel_mask, qahw_sdata->devices);

    /* Turn BT_SCO on if bt_sco recording */
    if(audio_is_bluetooth_sco_device(qahw_sdata->devices)) {
        ret = qahw_set_parameters(module_handle, bt_sco_on);
        pa_log_info("%s: param %s set to hal with return value %d", __func__, bt_sco_on, ret);
    }

    if (buffer_duration > 0)
        qahw_sdata->config.offload_info.duration_us = buffer_duration * 1000;

    rc = qahw_open_input_stream(module_handle, qahw_sdata->handle, qahw_sdata->devices, &qahw_sdata->config, &qahw_sdata->in_handle, qahw_sdata->flags,
                                qahw_sdata->device_url, qahw_sdata->source_type);
    if (rc) {
        qahw_sdata->in_handle = NULL;
        pa_log_error("Could not open input stream %d", rc);
        goto fail;
    }
    qahw_sdata->module_handle = module_handle;

    /* set profile for the recording session */
    if (qahw_sdata->preemph_status)
        qahw_in_set_parameters(qahw_sdata->in_handle, "audio_stream_profile=record_deemph");
    else if (qahw_sdata->source_type == AUDIO_SOURCE_UNPROCESSED)
        qahw_in_set_parameters(qahw_sdata->in_handle, "audio_stream_profile=record_unprocessed");
    else if (qahw_sdata->qahw_processing_id & PA_QAHW_CARD_QAHW_PROCESSING_FLUENCE)
        qahw_in_set_parameters(qahw_sdata->in_handle, "audio_stream_profile=record_fluence");
    else if (qahw_sdata->qahw_processing_id & PA_QAHW_CARD_QAHW_PROCESSING_FFECNS)
        qahw_in_set_parameters(qahw_sdata->in_handle, "audio_stream_profile=record_ffecns");

    if (encoding == PA_ENCODING_DSD) {
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

        qahw_in_set_parameters(qahw_sdata->in_handle, dsd_format);
    }

    pa_log_debug("qahw source opened %p", qahw_sdata->in_handle);

    qahw_sdata->source_buffer_size = qahw_in_get_buffer_size(qahw_sdata->in_handle);
    if (qahw_sdata->source_buffer_size <= 0) {
        qahw_close_input_stream(qahw_sdata->in_handle);
        rc = -1;
        goto fail;
    }

    /*FIXME: Add DSP latency */
    qahw_sdata->source_latency_us = pa_bytes_to_usec(qahw_sdata->source_buffer_size, ss);
    pa_log_debug("source latency %dus", qahw_sdata->source_latency_us);

fail:
    return rc;
}

static int close_qahw_source(qahw_source_data *qahw_sdata) {
    int rc = -1;
    int ret = -1;
    const char *bt_sco_off = "BT_SCO=off";

    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->in_handle);
    pa_assert(qahw_sdata->module_handle);

    pa_log_debug("closing qahw source %p", qahw_sdata->in_handle);

    if (PA_UNLIKELY(qahw_sdata->in_handle == NULL)) {
        pa_log_error("Invalid source handle %p", qahw_sdata->in_handle);
    } else {
        rc = qahw_close_input_stream(qahw_sdata->in_handle);
        if (PA_UNLIKELY(rc)) {
            pa_log_error(" could not close source handle %p, error  %d", qahw_sdata->in_handle, rc);
        }

        qahw_sdata->in_handle = NULL;
    }
#ifdef SOURCE_DUMP_ENABLED
    close(qahw_sdata->write_fd);
#endif

    /* Turn BT_SCO off if bt_sco recording */
    if(audio_is_bluetooth_sco_device(qahw_sdata->devices)) {
        ret = qahw_set_parameters(qahw_sdata->module_handle, bt_sco_off);
        pa_log_info("%s: param %s set to hal with return value %d", __func__, bt_sco_off, ret);
    }

    return rc;
}

static int restart_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                              audio_input_flags_t flags, int source_id, qahw_source_data *qahw_sdata) {
    int rc;

    rc = close_qahw_source(qahw_sdata);
    if (rc) {
        pa_log_error("close_qahw_source failed, error %d", rc);
        goto exit;
    }

    rc = open_qahw_source(module_handle, encoding, ss, map, devices, flags, source_id, qahw_sdata, qahw_sdata->source_type, qahw_sdata->buffer_duration, qahw_sdata->qahw_processing_id);
    if (rc) {
        pa_log_error("open_qahw_source failed during recreation, error %d", rc);
    }

exit:
    return rc;
}

static int free_qahw_source(qahw_source_data *qahw_sdata) {
    int rc;

    pa_assert(qahw_sdata);

    rc = close_qahw_source(qahw_sdata);
    if (rc) {
        pa_log_error("close_qahw_source failed, error %d", rc);
    }

    pa_xfree(qahw_sdata);
    qahw_sdata = NULL;

    return rc;
}

static int create_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                                                    audio_input_flags_t flags, int source_id, pa_qahw_source_data *sdata, audio_source_t source_type,
                            int32_t buffer_duration, int32_t preemph_status, pa_qahw_card_qahw_processing_id_t qahw_processing_id, uint32_t dsd_rate) {
   int rc;

   sdata->qahw_sdata = pa_xnew0(qahw_source_data, 1);
   sdata->qahw_sdata->preemph_status = preemph_status;
   sdata->qahw_sdata->dsd_rate = dsd_rate;
   sdata->qahw_sdata->ts_log = TRACE_LOG_STATIC_INIT;

   rc = open_qahw_source(module_handle, encoding, ss, map, devices, flags, source_id, sdata->qahw_sdata, source_type, buffer_duration, qahw_processing_id);
   if (rc) {
       pa_log_error("open_qahw_source failed, error %d", rc);
       pa_xfree(sdata->qahw_sdata);
       sdata->qahw_sdata = NULL;
   }

    return rc;
}

static int create_pa_source(pa_module *m, char *source_name, char *description, pa_idxset *formats, pa_sample_spec *ss, pa_channel_map *map,
                            uint32_t alternate_sample_rate, pa_qahw_card_avoid_processing_config_id_t avoid_config_processing, pa_card *card,
                            pa_hashmap *ports, const char *driver, pa_qahw_source_data *source_data, pa_proplist *proplist, uint32_t priority) {
    pa_source_new_data new_data;
    pa_source_data *pa_sdata = NULL;

    pa_device_port *port;
    pa_format_info *format;
    pa_format_info *in_format;
    void *state;
    uint32_t i;

    bool port_source_mapping = false;

    pa_assert(source_data->qahw_sdata);

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

    if (source_data->qahw_sdata->config.format == AUDIO_FORMAT_DSD) {
        ss->channels = 1;
        ss->format = PA_SAMPLE_U8;
        pa_channel_map_init_auto(map, ss->channels, PA_CHANNEL_MAP_DEFAULT);
    }

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

    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_STRING, pa_qahw_source_get_name_from_flags(source_data->qahw_sdata->flags));
    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_DESCRIPTION, description);
    pa_proplist_setf(new_data.proplist, "buffer-size", "%d", source_data->qahw_sdata->source_buffer_size);

    if (avoid_config_processing & PA_QAHW_CARD_AVOID_PROCESSING_FOR_ALL)
        new_data.avoid_processing = true;
    else
        new_data.avoid_processing = false;

    if (proplist)
        pa_proplist_update(new_data.proplist, PA_UPDATE_REPLACE, proplist);

    pa_sdata->source = pa_source_new(m->core, &new_data, PA_SOURCE_HARDWARE);
    if (!pa_sdata->source) {
        pa_log_error("Could not create source");
        goto fail;
    }

    pa_log_info("pa source opened %p", pa_sdata->source);
    pa_source_new_data_done(&new_data);

    pa_sdata->source->userdata = (void *)source_data;
    pa_sdata->source->parent.process_msg = pa_qahw_source_process_msg;
    pa_sdata->source->set_state_in_io_thread = pa_qahw_source_set_state_in_io_thread_cb;
    pa_sdata->source->set_port = pa_qahw_source_set_port_cb;
    pa_sdata->source->priority = priority;

    /* FIXME: check reconfigure needed for non pcm */
    pa_sdata->source->reconfigure = pa_qahw_source_reconfigure_cb;

    pa_sdata->avoid_config_processing = avoid_config_processing;

    if (pa_idxset_size(formats) > 0 ) {
        pa_sdata->source->get_formats = pa_qahw_source_get_formats;

        pa_sdata->formats = pa_idxset_new(NULL, NULL);

        PA_IDXSET_FOREACH(in_format, formats, i) {
            format = pa_format_info_copy(in_format);
            pa_idxset_put(pa_sdata->formats, format, NULL);
        }
    }

    pa_source_set_asyncmsgq(pa_sdata->source, pa_sdata->thread_mq.inq);
    pa_source_set_rtpoll(pa_sdata->source, pa_sdata->rtpoll);
    pa_source_set_max_rewind(pa_sdata->source, 0);
    pa_source_set_fixed_latency(pa_sdata->source, source_data->qahw_sdata->source_latency_us);

    pa_sdata->thread = pa_thread_new(source_name, pa_qahw_source_thread_func, source_data);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

    /* keep pa source and qahw port in sync, qahw is opened with some default port, update qahw with active port decided by pa source */
    pa_qahw_source_set_port_cb(pa_sdata->source, pa_sdata->source->active_port);

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

    pa_log_debug("closing pa source %p state %d", pa_sdata->source, pa_sdata->source->state);

    if (pa_sdata->source)
        pa_source_unlink(pa_sdata->source);

    if (pa_sdata->thread) {
        pa_asyncmsgq_send(pa_sdata->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(pa_sdata->thread);
    }

    pa_thread_mq_done(&pa_sdata->thread_mq);

    if (pa_sdata->source)
        pa_source_unref(pa_sdata->source);

    if (pa_sdata->formats)
        pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);

    if (pa_sdata->rtpoll)
        pa_rtpoll_free(pa_sdata->rtpoll);

    pa_xfree(pa_sdata);

    return 0;
}

bool pa_qahw_source_is_supported_sample_rate(uint32_t sample_rate) {
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

pa_idxset* pa_qahw_source_get_config(pa_qahw_source_handle_t *handle) {
    pa_qahw_source_data *sdata = (pa_qahw_source_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->source);

    return pa_qahw_source_get_formats(sdata->pa_sdata->source);
}

int pa_qahw_source_create(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name, pa_qahw_source_config *source,
                        pa_qahw_source_handle_t **handle) {
    int rc = -1;
    pa_qahw_source_data *sdata;
    pa_device_port *card_port;
    pa_qahw_card_port_config *source_port;
    pa_hashmap *ports;
    pa_qahw_card_port_device_data *port_device_data;

    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    void *state;

    pa_assert(m);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module_handle);
    pa_assert(module_name);
    pa_assert(source);
    pa_assert(source->name);
    pa_assert(source->description);
    pa_assert(source->formats);
    pa_assert(source->type);
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

    sdata = pa_xnew0(pa_qahw_source_data, 1);

    pa_log_info("%s: creating source with ss %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &source->default_spec));

    rc = create_qahw_source(module_handle, source->default_encoding, &source->default_spec, &source->default_map,  port_device_data->device, source->flags,
                     source->id, sdata, source->source_type, source->buffer_duration, source->preemph_status, source->qahw_processing_id, source->dsd_rate);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could not open qahw source, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_pa_source(m, source->name, source->description, source->formats, &source->default_spec, &source->default_map, source->alternate_sample_rate,
                          source->avoid_config_processing, card, ports, driver, sdata, source->proplist, source->priority);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa source for source %s, error %d", source->name, rc);
        free_qahw_source(sdata->qahw_sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = pa_qahw_source_extn_create(sdata->pa_sdata->source->core, sdata->qahw_sdata->in_handle, sdata->pa_sdata->source->index, &sdata->source_extn_handle);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create qahw source extn %s, error %d", source->name, rc);
        free_qahw_source(sdata->qahw_sdata);
        free_pa_source(sdata->pa_sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    *handle = (pa_qahw_source_handle_t *)sdata;

exit:
    if (ports)
        pa_hashmap_free(ports);
    return rc;
}

static int stop_qahw_source(qahw_source_data *qahw_sdata) {
    int rc;

    pa_assert(qahw_sdata);

    pa_atomic_store(&qahw_sdata->stopped, 1);
    rc = qahw_in_stop(qahw_sdata->in_handle);

    if (qahw_sdata->qahw_read_thread) {
        pa_thread_free(qahw_sdata->qahw_read_thread);
        qahw_sdata->qahw_read_thread = NULL;
    }

    return rc;
}

void pa_qahw_source_close(pa_qahw_source_handle_t *handle) {
    pa_qahw_source_data *sdata = (pa_qahw_source_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->pa_sdata);

    stop_qahw_source(sdata->qahw_sdata);

    pa_qahw_source_extn_free(sdata->source_extn_handle);
    free_pa_source(sdata->pa_sdata);
    free_qahw_source(sdata->qahw_sdata);
    pa_xfree(sdata);
}

void pa_qahw_source_suspend(pa_qahw_source_handle_t *handle, bool suspend) {
    pa_qahw_source_data *sdata;

    pa_assert(handle);
    sdata = (pa_qahw_source_data *)handle;

    pa_source_suspend(sdata->pa_sdata->source, suspend, PA_SUSPEND_UNAVAILABLE);
}
