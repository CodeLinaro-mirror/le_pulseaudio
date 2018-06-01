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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>

#include <pulse/rtclock.h>
#include <pulsecore/core-rtclock.h>

#include "qahw-sink.h"
#include "qahw-utils.h"

#include <sys/time.h>
#include <time.h>

/* #define SINK_DEBUG */

/* #define SINK_DUMP_ENABLED */

#define QAHW_MAX_GAIN 1

#define PA_ALTERNATE_SINK_RATE 44100 /* FIXME: Pass it from card */

static int restart_qahw_sink(qahw_module_handle_t *module_handle, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                              audio_output_flags_t flags, int sink_iohandle, struct sink_data *sdata);
static int create_qahw_sink(qahw_module_handle_t *module_handle,pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                            audio_output_flags_t flags, int sink_iohandle, struct sink_data *sdata);
static int close_qahw_sink(struct sink_data *sdata);
static int free_pa_sink(struct sink_data *sdata);

static const uint32_t supported_sink_rates[] =
                          {8000, 11025, 16000, 22050, 44100, 48000, 96000, 192000};

static const char *get_sink_name(audio_output_flags_t flags) {
    const char *name = NULL;

    if (flags == AUDIO_OUTPUT_FLAG_FAST)
        name = "low_latency";
    else if (flags == AUDIO_OUTPUT_FLAG_DEEP_BUFFER)
        name ="deep_buffer";
    else if (flags == AUDIO_OUTPUT_FLAG_DIRECT_PCM)
        name = "direct_pcm";
    else if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        name = "pcm_offload";
    else if (flags == AUDIO_OUTPUT_FLAG_RAW)
        name = "ultra_low_latency";

    return name;
}

static int qahw_out_write_cb(qahw_stream_callback_event_t event, void *param, void *userdata) {
    struct sink_data *sdata = (struct sink_data *)userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->fdsem);

    switch (event) {
        case QAHW_STREAM_CBK_EVENT_WRITE_READY:

            pa_atomic_store(&sdata->qahw_sdata->wait_for_write_ready, 0);

#ifdef SINK_DEBUG
            pa_log_debug("Received event QAHW_STREAM_CBK_EVENT_WRITE_READY for handle %p", sdata->qahw_sdata->out_handle);
#endif

            /*Wake up sink thread */
            pa_fdsem_post(sdata->fdsem);

            break;

        default:
            pa_log_error(" Unsupported event %d  handle %p", event, sdata->qahw_sdata->out_handle);
    }

    return 0;
}

static void qahw_fill_sink_info(struct qahw_sink_data *qahw_sdata, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                                audio_output_flags_t flags, int sink_iohandle) {

    qahw_sdata->config.format = get_qahw_audio_format(ss->format);
    qahw_sdata->config.sample_rate = ss->rate;
    qahw_sdata->config.channel_mask = audio_channel_out_mask_from_count(ss->channels); /* TODO: le get channel mask for pa map */

    /* DIRECT PCM uses offload structure */
    if (flags == AUDIO_OUTPUT_FLAG_DIRECT_PCM || AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        qahw_sdata->config.offload_info = AUDIO_INFO_INITIALIZER;
        qahw_sdata->config.offload_info.format = qahw_sdata->config.format;
        qahw_sdata->config.offload_info.sample_rate = qahw_sdata->config.sample_rate;
        qahw_sdata->config.offload_info.channel_mask = qahw_sdata->config.channel_mask;
    }

    qahw_sdata->devices = devices;
    qahw_sdata->flags = flags;
    qahw_sdata->handle = sink_iohandle; /* check if its correct */
    qahw_sdata->device_url = NULL; /* TODO: useful for BT devices */
    qahw_sdata->bytes_written = 0;
}

static uint64_t qahw_sink_get_latency(struct sink_data *sdata) {
    int rc, delta, bytes_rendered;
    int64_t latency = 0;
    uint64_t frames;
    struct qahw_sink_data *qahw_sdata;
    struct pa_sink_data *pa_sdata;

    struct timespec timestamp;
    pa_usec_t qahw_time, now;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qahw_sdata);

    qahw_sdata = sdata->qahw_sdata;
    pa_sdata = sdata->pa_sdata;

    pa_assert(pa_sdata->sink);
    pa_assert(qahw_sdata->out_handle);

    rc = qahw_out_get_presentation_position(qahw_sdata->out_handle, &frames, &timestamp);
    if (!rc) {
        qahw_time = pa_timespec_load(&timestamp);
        bytes_rendered =  frames * pa_frame_size(&pa_sdata->sink->sample_spec);

        /* calculate bytes pending to be rendered */
        delta = qahw_sdata->bytes_written - bytes_rendered;

        /* bytes written should never be less than bytes rendered */
        if (delta <= 0) {
#ifdef SINK_DEBUG
            pa_log_debug("latency is 0");
#endif
            return 0;
        }

        now = pa_rtclock_now();
        /* latency = bytes pending to be rendered + time elapesed after qahw_out_get_presentation_position */
        latency = (int64_t)(pa_bytes_to_usec(delta, &pa_sdata->sink->sample_spec) - (now - qahw_time));

#ifdef SINK_DEBUG
        pa_log_debug("%s:: now %" PRId64 "us, qahw_time %" PRId64 "us, delta %" PRId64 ", latency %" PRId64 "us", __func__, (int64_t)now, (int64_t)qahw_time,
                    (int64_t)pa_bytes_to_usec(delta, &pa_sdata->sink->sample_spec), latency);
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

static int qahw_sink_start(struct qahw_sink_data *qahw_sdata) {
    return 0;
}

static int qahw_sink_standby(struct qahw_sink_data *qahw_sdata) {

    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->out_handle);

    pa_log_info("%s",__func__);

    qahw_out_standby(qahw_sdata->out_handle);
    qahw_sdata->bytes_written = 0;
    pa_atomic_store(&qahw_sdata->wait_for_write_ready, 0);

    return 0;
}

static void qahw_sink_set_volume_cb(pa_sink *s) {
    struct sink_data *sdata = (struct sink_data *)s->userdata;
    float gain;
    int rc;
    pa_volume_t volume;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->qahw_sdata->out_handle);

    gain = ((float) pa_cvolume_max(&s->real_volume) * (float)QAHW_MAX_GAIN) / (float)PA_VOLUME_NORM;
    volume = (pa_volume_t) roundf((float) gain * PA_VOLUME_NORM / QAHW_MAX_GAIN);

    pa_log_debug ("qahw stream %p: gain %f\n", sdata->qahw_sdata->out_handle, gain);

    rc = qahw_out_set_volume(sdata->qahw_sdata->out_handle, gain, gain);
    if (rc)
        pa_log_error("qahw stream %p: unable to set volume error %d\n", sdata->qahw_sdata->out_handle, rc);
    else
        pa_cvolume_set(&s->real_volume, s->real_volume.channels, volume); /* TODO: Is this correct? */

    return;
}

static int qahw_sink_set_port_cb(pa_sink *s, pa_device_port *p) {
    audio_devices_t *audio_device;
    char *kvpair;
    struct sink_data *sdata = (struct sink_data *)s->userdata;
    int rc;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->qahw_sdata->out_handle);

    audio_device = PA_DEVICE_PORT_DATA(p);
    pa_assert(audio_device);

    kvpair = pa_sprintf_malloc("%s=%d", QAHW_PARAMETER_STREAM_ROUTING, *audio_device);

    rc = qahw_out_set_parameters(sdata->qahw_sdata->out_handle, kvpair);
    if (rc)
        pa_log_error("qahw routing failed %d",rc);

    pa_log_debug("port name: %s kvpair %s device %d",p->name, kvpair, *audio_device);

    pa_xfree(kvpair);

    return rc;
}

static int qahw_sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    struct sink_data *sdata = (struct sink_data *)(PA_SINK(o)->userdata);

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->sink);

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:
             *((int64_t*) data) = qahw_sink_get_latency(sdata);
             return 0;

        case PA_SINK_MESSAGE_SET_STATE: {
            pa_sink_state_t new_state = PA_PTR_TO_UINT(data);
            int r = 0;

            pa_log_debug("Sink new state is: %d", new_state);

            if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(sdata->pa_sdata->sink->thread_info.state))
                r = qahw_sink_start(sdata->qahw_sdata);
             else if (new_state == PA_SINK_SUSPENDED)
                r = qahw_sink_standby(sdata->qahw_sdata);

            /* Error */
            if (r < 0)
                return r;

            break;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int qahw_sink_update_cb(pa_sink *s, uint32_t rate) {//pa_sample_spec *spec, bool passthrough) {
    struct sink_data *sdata = (struct sink_data *) s->userdata;
    struct pa_sink_data *pa_sdata = NULL;
    struct qahw_sink_data *qahw_sdata = NULL;
    bool supported = false;
    uint32_t i, rc;
    uint32_t old_rate;

    pa_log_debug("qahw_sink_update_cb");

    pa_assert(s);
    pa_assert(s->userdata);
    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qahw_sdata);

    pa_sdata = sdata->pa_sdata;
    qahw_sdata = sdata->qahw_sdata;

    for (i = 0; i < ARRAY_SIZE(supported_sink_rates) ; i++) {
        if (/*spec->*/rate == supported_sink_rates[i]) {
            supported = true;
            break;
        }
    }

    if (!supported) {
        pa_log_info("Sink does not support sample rate of %d Hz", rate);
        return -1;
    }

    if (!PA_SINK_IS_OPENED(s->state)) {

        old_rate = pa_sdata->sink->sample_spec.rate; /* take backup */
        pa_sdata->sink->sample_spec.rate = rate;

        qahw_sdata->devices = *((audio_devices_t *)PA_DEVICE_PORT_DATA(pa_sdata->sink->active_port));

        pa_log_info("Updating rate for device %d, new rate is %d", qahw_sdata->devices, rate);

        rc = restart_qahw_sink(qahw_sdata->module_handle, &pa_sdata->sink->sample_spec, &pa_sdata->sink->channel_map, qahw_sdata->devices,
                              qahw_sdata->flags, qahw_sdata->handle, sdata);
        if (PA_UNLIKELY(rc)) {
            pa_sdata->sink->sample_spec.rate = old_rate; /* restore old rate if failed */
            pa_log_error("Could create reopen qahw sink, error %d", rc);
            return -1;
        }

        pa_qahw_sink_extn_sink_handle_update(sdata->sink_extn_handle, qahw_sdata->out_handle);

        pa_sink_set_fixed_latency(pa_sdata->sink, qahw_sdata->sink_latency_us);
        return 0;
    }

    pa_log_info("Sink could not set sample rate of %d Hz", rate);
    return -1;
}

static void qahw_sink_thread_func(void *userdata) {
    struct sink_data *sdata = (struct sink_data *)userdata;
    struct pa_sink_data *pa_sdata = sdata->pa_sdata;
    struct qahw_sink_data *qahw_sdata = sdata->qahw_sdata;

    pa_memchunk chunk;
    qahw_out_buffer_t out_buf;

    void *data;
    bool wait;
    int rc;

    pa_thread_mq_install(&pa_sdata->thread_mq);

    memset(&out_buf, 0, sizeof(qahw_out_buffer_t));

    while (true) {
        wait = true;

        if (pa_sdata->sink->thread_info.rewind_requested)
            pa_sink_process_rewind(pa_sdata->sink, 0);

        if ((PA_SINK_IS_OPENED(pa_sdata->sink->thread_info.state)) && !pa_atomic_load(&qahw_sdata->wait_for_write_ready)) {
            /* Check if we need to resend previous buffer */
            if (!out_buf.buffer) {
                /* FIXME: can be more efficient by not using _full */
                pa_sink_render_full(pa_sdata->sink, qahw_sdata->sink_buffer_size, &chunk);
                pa_assert(chunk.length == qahw_sdata->sink_buffer_size);

                data = pa_memblock_acquire(chunk.memblock);
                out_buf.buffer = (char*)data + chunk.index;
                out_buf.bytes = chunk.length;
            } else {
                /* Update buffer offset and size based on last write size*/
                out_buf.buffer = (char *)out_buf.buffer + qahw_sdata->sink_buffer_size - out_buf.bytes;
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
            }
        } else if (pa_sdata->sink->thread_info.state == PA_SINK_SUSPENDED) {
            /* if sink is suspended state then reset buffer otherwise it might end up sending incorrect buffer to qahw_write */
            memset(&out_buf, 0, sizeof(qahw_out_buffer_t));
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

static int open_qahw_sink(qahw_module_handle_t *module_handle, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                            audio_output_flags_t flags, int sink_iohandle, struct sink_data *sdata) {
    int rc;
    struct qahw_sink_data *qahw_sdata;
#ifdef SINK_DUMP_ENABLED
    char *file_name;
#endif

    pa_assert(ss);
    pa_assert(map);
    pa_assert(module_handle);
    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);

    qahw_sdata = sdata->qahw_sdata;

    qahw_fill_sink_info(qahw_sdata, ss, map, devices, flags, sink_iohandle);

    pa_log_debug("opening sink with configuration flag = 0x%x, format %d, sample_rate %d, channel_mask 0x%x device %d",
                 qahw_sdata->flags, qahw_sdata->config.format, qahw_sdata->config.sample_rate, qahw_sdata->config.channel_mask, qahw_sdata->devices);

    rc = qahw_open_output_stream(module_handle, qahw_sdata->handle, qahw_sdata->devices, qahw_sdata->flags, &qahw_sdata->config,
            &qahw_sdata->out_handle, qahw_sdata->device_url);
    if (rc) {
        qahw_sdata->out_handle = NULL;
        pa_log_error("Could not open output stream %d", rc);
        goto exit;
    }

    qahw_sdata->module_handle = module_handle;

    pa_log_debug("qahw sink opened %p", qahw_sdata->out_handle);

    qahw_sdata->sink_buffer_size = qahw_out_get_buffer_size(qahw_sdata->out_handle);
    if (qahw_sdata->sink_buffer_size <= 0) {
        qahw_close_output_stream(qahw_sdata->out_handle);
        pa_log_error("Invalid buffer size %zu", qahw_sdata->sink_buffer_size);
        rc = -1;
        goto exit;
    }

    if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        qahw_out_set_callback(qahw_sdata->out_handle, qahw_out_write_cb, sdata);

    /*FIXME: Add DSP latency */
    qahw_sdata->sink_latency_us = pa_bytes_to_usec(qahw_sdata->sink_buffer_size, ss);
    pa_log_debug("sink latency %dus", qahw_sdata->sink_latency_us);

    pa_atomic_store(&qahw_sdata->wait_for_write_ready, 0);

#ifdef SINK_DUMP_ENABLED
    file_name = pa_sprintf_malloc("/data/pcmdump_sink_%d", qahw_sdata->handle);

    qahw_sdata->write_fd = open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
    if(qahw_sdata->write_fd < 0)
        pa_log_error("Could not open write fd %d for sink index %d", qahw_sdata->write_fd, qahw_sdata->handle);

    pa_xfree(file_name);
#endif

exit:
    return rc;
}

static int close_qahw_sink(struct sink_data *sdata) {
    struct qahw_sink_data *qahw_sdata;
    int rc = -1;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);

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

    return rc;
}

static int restart_qahw_sink(qahw_module_handle_t *module_handle, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                              audio_output_flags_t flags, int sink_iohandle, struct sink_data *sdata) {
    int rc;

    rc = close_qahw_sink(sdata);
    if (rc) {
        pa_log_error("close_qahw_sink failed, error %d", rc);
        goto exit;
    }

    rc = open_qahw_sink(module_handle, ss, map, devices, flags, sink_iohandle, sdata);
    if (rc) {
        pa_log_error("open_qahw_sink failed during recreation, error %d", rc);
    }

exit:
    return rc;
}

static int free_qahw_sink(struct sink_data *sdata) {
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

static int create_qahw_sink(qahw_module_handle_t *module_handle, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                            audio_output_flags_t flags, int sink_iohandle, struct sink_data *sdata) {
   int rc;

   sdata->qahw_sdata = pa_xnew0(struct qahw_sink_data, 1);

   rc = open_qahw_sink(module_handle, ss, map, devices, flags, sink_iohandle, sdata);
   if (rc) {
       pa_log_error("open_qahw_sink failed, error %d", rc);
       pa_xfree(sdata->qahw_sdata);
       sdata->qahw_sdata = NULL;
   }

    return rc;
}

static int free_common_sink_resources(struct sink_data *sdata) {
    if (sdata->fdsem)
        pa_fdsem_free(sdata->fdsem);

    return 0;
}

static int alloc_common_sink_resources(struct sink_data *sdata) {
   sdata->fdsem = pa_fdsem_new();
   if (!sdata->fdsem) {
       pa_log_error("Could not create fdsem");
       return -1;
   }
   
   return 0;
}

static int create_pa_sink(pa_module *m, pa_sample_spec *ss, pa_channel_map *map, char *sink_name, pa_card *card,
                          const char *profile_name, const char *driver, struct sink_data *sdata) {
    pa_sink_new_data new_data;
    struct pa_sink_data *pa_sdata;
    pa_device_port *port;
    pa_card_profile *profile;
    void *state, *state2;

    pa_assert(sdata->qahw_sdata);

    pa_sdata = pa_xnew0(struct pa_sink_data, 1);
    pa_sink_new_data_init(&new_data);
    new_data.driver = driver;
    new_data.module = m;
    new_data.card = card;

    pa_sdata->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&pa_sdata->thread_mq, m->core->mainloop, pa_sdata->rtpoll);

    pa_sink_new_data_set_name(&new_data, sink_name);

    pa_log_info("ss->rate %d ss->channels %d", ss->rate, ss->channels);
    pa_sink_new_data_set_sample_spec(&new_data, ss);
    pa_sink_new_data_set_channel_map(&new_data, map);
    pa_sink_new_data_set_alternate_sample_rate(&new_data, PA_ALTERNATE_SINK_RATE);

    /* associate port with sink, first get port in a card then for each profile in that port check if matches with output profile */
    PA_HASHMAP_FOREACH(port, card->ports, state) {
        if (!(port->direction & PA_DIRECTION_OUTPUT))
            continue;

        PA_HASHMAP_FOREACH(profile, port->profiles, state2) {
            profile = pa_hashmap_get(port->profiles, profile_name);

            if (profile && pa_streq(profile->name, profile_name)) {
                pa_log_debug("adding port %s to sink %s", port->name, sink_name);
                pa_assert_se(pa_hashmap_put(new_data.ports, port->name, port) == 0);
                pa_device_port_ref(port);
            }
        }
    }

    pa_sdata->sink = pa_sink_new(m->core, &new_data, PA_SINK_HARDWARE | PA_SINK_LATENCY);
    pa_sink_new_data_done(&new_data);

    if (!pa_sdata->sink) {
        pa_log_error("Could not create pa sink");
        goto fail;
    }

    pa_log_debug("pa sink opened %p", pa_sdata->sink);
    sdata->pa_sdata = pa_sdata;

    pa_sdata->sink->userdata = (void *)sdata;
    pa_sdata->sink->parent.process_msg = qahw_sink_process_msg;
    pa_sdata->sink->set_port = qahw_sink_set_port_cb;
    pa_sdata->sink->update_rate = qahw_sink_update_cb;

    pa_sink_set_asyncmsgq(pa_sdata->sink, pa_sdata->thread_mq.inq);
    pa_sink_set_rtpoll(pa_sdata->sink, pa_sdata->rtpoll);

    pa_sink_set_max_request(pa_sdata->sink, sdata->qahw_sdata->sink_buffer_size);
    pa_sink_set_max_rewind(pa_sdata->sink, 0);
    pa_sink_set_fixed_latency(pa_sdata->sink, sdata->qahw_sdata->sink_latency_us);

    pa_sink_set_set_volume_callback(pa_sdata->sink, qahw_sink_set_volume_cb);
    pa_sdata->sink->n_volume_steps = 15; /* TODO: What should be value */

    pa_sdata->thread = pa_thread_new(sink_name, qahw_sink_thread_func, sdata);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

   pa_sdata->rtpoll_item = pa_rtpoll_item_new_fdsem(pa_sdata->rtpoll, PA_RTPOLL_NORMAL, sdata->fdsem);
   if (!pa_sdata->rtpoll_item) {
       pa_log_error("Could not create rpoll item");
       goto fail;
   }

   /* keep pa sink and qahw port in sync, qahw is opened with some default port, update qahw with active port decided by pa sink */
   qahw_sink_set_port_cb(pa_sdata->sink, pa_sdata->sink->active_port);

   pa_sink_put(pa_sdata->sink);

   return 0;

fail :
    if (pa_sdata)
        free_pa_sink(sdata);

    return -1;
}

static int free_pa_sink(struct sink_data *sdata) {
    struct pa_sink_data *pa_sdata;

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

    if (pa_sdata->sink)
        pa_sink_unref(pa_sdata->sink);

    pa_thread_mq_done(&pa_sdata->thread_mq);

    if (pa_sdata->rtpoll_item)
        pa_rtpoll_item_free(pa_sdata->rtpoll_item);


    if (pa_sdata->rtpoll)
        pa_rtpoll_free(pa_sdata->rtpoll);

    pa_xfree(pa_sdata);

    return 0;
}

int create_sink(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name, const char *profile_name,
                pa_sample_spec *ss, pa_channel_map *map, uint32_t sink_devices, int32_t flags, int sink_iohandle, sink_handle_t **handle) {
    int rc;
    char *name;
    struct sink_data *sdata;

    pa_assert(m);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module_handle);
    pa_assert(module_name);
    pa_assert(profile_name);
    pa_assert(ss);
    pa_assert(map);

    sdata = pa_xnew0(struct sink_data, sizeof(struct sink_data));

    rc = alloc_common_sink_resources(sdata);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could alloc_common_sink_resources, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_qahw_sink(module_handle, ss, map, sink_devices, flags, sink_iohandle, sdata);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could create open qahw sink, error %d", rc);
        free_common_sink_resources(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    name = pa_sprintf_malloc("qahw_sink.%s_%s_%d", module_name, get_sink_name(flags), sink_iohandle);
    pa_log_debug("Opening sink for profile %s with name %s", profile_name, name);

    rc = create_pa_sink(m, ss, map, name, card, profile_name, driver, sdata);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa sink for sink %s, error %d", name, rc);
        free_qahw_sink(sdata);
        free_common_sink_resources(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = pa_qahw_sink_extn_create(sdata->pa_sdata->sink->core, sdata->qahw_sdata->out_handle, sdata->pa_sdata->sink->index, &sdata->sink_extn_handle);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create qahw sink extn %s, error %d", name, rc);
        free_qahw_sink(sdata);
        free_pa_sink(sdata);
        pa_xfree(sdata);
        sdata = NULL;
    }

    pa_xfree(name);

    *handle = (sink_handle_t *)sdata;

exit:
    return rc;
}

void close_sink(sink_handle_t *handle) {
    struct sink_data *sdata = (struct sink_data *)handle;

    pa_assert(sdata);

    pa_qahw_sink_extn_free(sdata->sink_extn_handle);
    free_pa_sink(sdata);
    free_qahw_sink(sdata);
    free_common_sink_resources(sdata);

    pa_xfree(sdata);
}
