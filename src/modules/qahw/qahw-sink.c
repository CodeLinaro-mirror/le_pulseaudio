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

#include "qahw-sink.h"
#include "qahw-utils.h"

#define QAHW_MAX_GAIN 1

static int create_qahw_sink(qahw_module_handle_t *module_handle,pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                            audio_output_flags_t flags, int sink_iohandle, struct sink_data *sdata);

static const char *get_sink_name(audio_output_flags_t flags) {
    const char *name = NULL;

    if (flags == AUDIO_OUTPUT_FLAG_FAST)
        name = "low_latency";
    else if (flags == AUDIO_OUTPUT_FLAG_DEEP_BUFFER)
        name ="deep_buffer";
    else if (flags == AUDIO_OUTPUT_FLAG_DIRECT_PCM)
        name = "direct_pcm";
    else if (flags == AUDIO_OUTPUT_FLAG_RAW)
        name = "ultra_low_latency";

    return name;
}

static void qahw_fill_sink_info(struct qahw_sink_data *qahw_sdata, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                                audio_output_flags_t flags, int sink_iohandle) {

    qahw_sdata->config.format = get_qahw_audio_format(ss->format);
    qahw_sdata->config.sample_rate = ss->rate;
    qahw_sdata->config.channel_mask = audio_channel_out_mask_from_count(ss->channels); /* TODO: le get channel mask for pa map */

    /* DIRECT PCM uses offload structure */
    if (flags == AUDIO_OUTPUT_FLAG_DIRECT_PCM) {
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

static int qahw_sink_start(struct qahw_sink_data *qahw_sdata) {
    return 0;
}

static int qahw_sink_standby(struct qahw_sink_data *qahw_sdata) {
    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->out_handle);

    qahw_out_standby(qahw_sdata->out_handle);
    qahw_sdata->bytes_written = 0;

    return 0;
}

static void qahw_sink_set_volume_cb(pa_sink *s) {
    struct sink_data *sink_data = (struct sink_data *)s->userdata;
    float gain;
    int rc;
    pa_volume_t volume;

    pa_assert(sink_data);
    pa_assert(sink_data->qahw_sdata);
    pa_assert(sink_data->qahw_sdata->out_handle);

    gain = ((float) pa_cvolume_max(&s->real_volume) * (float)QAHW_MAX_GAIN) / (float)PA_VOLUME_NORM;
    volume = (pa_volume_t) roundf((float) gain * PA_VOLUME_NORM / QAHW_MAX_GAIN);

    pa_log_debug ("qahw stream %p: gain %f\n", sink_data->qahw_sdata->out_handle, gain);

    rc = qahw_out_set_volume(sink_data->qahw_sdata->out_handle, gain, gain);
    if (rc)
        pa_log_error("qahw stream %p: unable to set volume error %d\n", sink_data->qahw_sdata->out_handle, rc);
    else
        pa_cvolume_set(&s->real_volume, s->real_volume.channels, volume); /* TODO: Is this correct? */

    return;
}

static int qahw_sink_set_port_cb(pa_sink *s, pa_device_port *p) {
    audio_devices_t *audio_device;
    char kvpair[KV_PAIR_MAX_LENGTH] = { 0 };
    struct sink_data *sink_data = (struct sink_data *)s->userdata;
    int rc;

    pa_assert(sink_data);
    pa_assert(sink_data->qahw_sdata);
    pa_assert(sink_data->qahw_sdata->out_handle);

    audio_device = PA_DEVICE_PORT_DATA(p);
    pa_assert(audio_device);

    /* FIXME: use pa_sprintf_malloc() */
    snprintf(kvpair, KV_PAIR_MAX_LENGTH, "%s=%d", QAHW_PARAMETER_STREAM_ROUTING, *audio_device);

    rc = qahw_out_set_parameters(sink_data->qahw_sdata->out_handle, kvpair);
    if (rc)
        pa_log_error("qahw routing failed %d",rc);

    pa_log_debug("port name: %s kvpair %s device %d",p->name, kvpair, *audio_device);

    return rc;
}

static int qahw_sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    struct sink_data *sink_data = (struct sink_data *)(PA_SINK(o)->userdata);

    pa_assert(sink_data);
    pa_assert(sink_data->pa_sdata);
    pa_assert(sink_data->pa_sdata->sink);

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:
             *((int64_t*) data) = 0;
             break;

        case PA_SINK_MESSAGE_SET_STATE: {
            pa_sink_state_t new_state = PA_PTR_TO_UINT(data);
            int r = 0;

            pa_log_debug("Sink new state is: %d", new_state);

            if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(sink_data->pa_sdata->sink->thread_info.state)) {
                r = qahw_sink_start(sink_data->qahw_sdata);
            } else if (new_state == PA_SINK_SUSPENDED) {
                r = qahw_sink_standby(sink_data->qahw_sdata);
            }

            /* Error */
            if (r < 0)
                return r;

            break;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void qahw_sink_thread_func(void *userdata) {
    struct sink_data *sink_data = (struct sink_data *)userdata;
    struct pa_sink_data *pa_sdata = sink_data->pa_sdata;
    struct qahw_sink_data *qahw_sdata = sink_data->qahw_sdata;

    pa_thread_mq_install(&pa_sdata->thread_mq);

    while (true) {
       int rc;

        if (pa_sdata->sink->thread_info.rewind_requested)
            pa_sink_process_rewind(pa_sdata->sink, 0);

        if (PA_SINK_IS_OPENED(pa_sdata->sink->thread_info.state)) {
            pa_memchunk chunk;
            void *data;
            qahw_out_buffer_t out_buf;

            memset(&out_buf,0, sizeof(qahw_out_buffer_t));

            /* FIXME: can be more efficient by not using _full */
            pa_sink_render_full(pa_sdata->sink, qahw_sdata->sink_buffer_size, &chunk);
            pa_assert(chunk.length == qahw_sdata->sink_buffer_size);

            data = pa_memblock_acquire(chunk.memblock);
            out_buf.buffer = data;
            out_buf.bytes = chunk.length;

            if ((rc = qahw_out_write(qahw_sdata->out_handle, &out_buf)) < 0) {
                pa_log_error("Could not write data: %d", rc);
            } else {
                qahw_sdata->bytes_written += rc;
            }

            pa_memblock_release(chunk.memblock);
            pa_memblock_unref(chunk.memblock);
#if 0
            /* its not needed as hal write is blocking call */
            /* Now sleep for one fragment duration */
            pa_rtpoll_set_timer_relative(pa_sdata->rtpoll, qahw_sdata->buffer_duration_us);
            pa_log_debug("Sleep");
#endif
        } else {
            /* Disable the timer since we're not running */
            pa_rtpoll_set_timer_disabled(pa_sdata->rtpoll);
        }

        rc = pa_rtpoll_run(pa_sdata->rtpoll);
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

static int create_qahw_sink(qahw_module_handle_t *module_handle, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                            audio_output_flags_t flags, int sink_iohandle, struct sink_data *sdata) {
    int rc;
    struct qahw_sink_data *qahw_sdata;

    pa_assert(ss);
    pa_assert(map);
    pa_assert(module_handle);

    qahw_sdata = pa_xnew0(struct qahw_sink_data, 1);

    qahw_fill_sink_info(qahw_sdata, ss, map, devices, flags, sink_iohandle);

    pa_log_debug("opening sink with configuration flag = 0x%x, format %d, sample_rate %d, channel_mask 0x%x",
                 qahw_sdata->flags, qahw_sdata->config.format, qahw_sdata->config.sample_rate, qahw_sdata->config.channel_mask);

    rc = qahw_open_output_stream(module_handle, qahw_sdata->handle, qahw_sdata->devices, qahw_sdata->flags, &qahw_sdata->config,
            &qahw_sdata->out_handle, qahw_sdata->device_url);
    if (rc) {
        qahw_sdata->out_handle = NULL;
        pa_log_error("Could not open output stream %d", rc);
        pa_xfree(qahw_sdata);
        qahw_sdata = NULL;
        goto exit;
    }

    qahw_sdata->module_handle = module_handle;

    pa_log_debug("qahw sink opened %p", qahw_sdata->out_handle);

    qahw_sdata->sink_buffer_size = qahw_out_get_buffer_size(qahw_sdata->out_handle);
    if (qahw_sdata->sink_buffer_size <= 0) {
        qahw_close_output_stream(qahw_sdata->out_handle);
        pa_log_error("Invalid buffer size %zu", qahw_sdata->sink_buffer_size);
        pa_xfree(qahw_sdata);
        qahw_sdata = NULL;
        goto exit;
    }

    qahw_sdata->sink_latency_ms = qahw_out_get_latency(qahw_sdata->out_handle);
    pa_log_debug("sink latency %dms", qahw_sdata->sink_latency_ms);

    sdata->qahw_sdata = qahw_sdata;

exit:
    return qahw_sdata ? 0: -1;
}

static int close_qahw_sink(struct qahw_sink_data *qahw_sdata) {
    int rc = -1;

    pa_assert(qahw_sdata);
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

    pa_xfree(qahw_sdata);

    return rc;
}

static int create_pa_sink(pa_module *m, pa_sample_spec *ss, pa_channel_map *map, char *sink_name, pa_card *card,
                          const char *profile_name, const char *driver, struct sink_data *sink_data) {
    pa_sink_new_data new_data;
    struct pa_sink_data *pa_sdata;
    pa_device_port *port;
    pa_card_profile *profile;
    void *state, *state2;

    pa_assert(sink_data->qahw_sdata);

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

    /* associate port with sink, first get port in a card then for each profile in that port check if matches with input profile */
    PA_HASHMAP_FOREACH(port, card->ports, state) {
        if (!(port->direction & PA_DIRECTION_OUTPUT))
            continue;

        PA_HASHMAP_FOREACH(profile, port->profiles, state2) {
            profile = pa_hashmap_get(port->profiles, profile_name);

            if (profile && !pa_streq(profile->name, profile_name)) {
                pa_log_debug("adding port %s to sink %s", port->name, sink_name);
                pa_assert_se(pa_hashmap_put(new_data.ports, port->name, port) == 0);
                pa_device_port_ref(port);
            }
        }
    }

    pa_sdata->sink = pa_sink_new(m->core, &new_data, PA_SINK_HARDWARE);
    pa_sink_new_data_done(&new_data);

    if (!pa_sdata->sink) {
        pa_log_error("Could not create pa sink");
        goto fail;
    }

    pa_log_debug("pa sink opened %p", pa_sdata->sink);
    sink_data->pa_sdata = pa_sdata;

    pa_sdata->sink->userdata = (void *)sink_data;
    pa_sdata->sink->parent.process_msg = qahw_sink_process_msg;
    pa_sdata->sink->set_port = qahw_sink_set_port_cb;

    pa_sink_set_asyncmsgq(pa_sdata->sink, pa_sdata->thread_mq.inq);
    pa_sink_set_rtpoll(pa_sdata->sink, pa_sdata->rtpoll);

    pa_sink_set_max_request(pa_sdata->sink, sink_data->qahw_sdata->sink_buffer_size);
    pa_sink_set_max_rewind(pa_sdata->sink, 0);
    pa_sink_set_fixed_latency(pa_sdata->sink, sink_data->qahw_sdata->sink_latency_ms * PA_USEC_PER_MSEC);

    pa_sink_set_set_volume_callback(pa_sdata->sink, qahw_sink_set_volume_cb);
    pa_sdata->sink->n_volume_steps = 15; /* TODO: What should be value */

    pa_sdata->thread = pa_thread_new(sink_name, qahw_sink_thread_func, sink_data);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

    pa_sink_put(pa_sdata->sink);

    pa_xfree(sink_name);

    return 0;

fail :
    if (!pa_sdata)
        return -1;

    if (pa_sdata->rtpoll)
        pa_rtpoll_free(pa_sdata->rtpoll);

    if (pa_sdata->sink)
        pa_sink_unref(pa_sdata->sink);

    pa_xfree(pa_sdata);
    sink_data->pa_sdata = NULL;

    pa_xfree(sink_name);

    return -1;
}

static int close_pa_sink(struct pa_sink_data *pa_sdata) {
    pa_assert(pa_sdata);
    pa_assert(pa_sdata->sink);
    pa_assert(pa_sdata->thread);
    pa_assert(pa_sdata->rtpoll);

    pa_log_debug("closing pa sink %p", pa_sdata->sink);

    pa_sink_unlink(pa_sdata->sink);

    pa_asyncmsgq_send(pa_sdata->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
    pa_thread_free(pa_sdata->thread);

    pa_sink_unref(pa_sdata->sink);

    pa_thread_mq_done(&pa_sdata->thread_mq);

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

    rc = create_qahw_sink(module_handle, ss, map, sink_devices, flags, sink_iohandle, sdata);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could create open qahw sink, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    name = pa_sprintf_malloc("qahw_sink.%s_%s_%d", module_name, get_sink_name(flags), sink_iohandle);
    pa_log_debug("Opening sink for profile %s with name %s", profile_name, name);

    rc = create_pa_sink(m, ss, map, name, card, profile_name, driver, sdata);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa sink for sink %s, error %d", name, rc);
        close_qahw_sink(sdata->qahw_sdata);
        pa_xfree(sdata);
        sdata = NULL;
    }

    *handle = (sink_handle_t *)sdata;

exit:
    return rc;
}

void close_sink(sink_handle_t *handle) {
    struct sink_data *sdata = (struct sink_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->pa_sdata);

    close_pa_sink(sdata->pa_sdata);
    close_qahw_sink(sdata->qahw_sdata);

    pa_xfree(sdata);
}
