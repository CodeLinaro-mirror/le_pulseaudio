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
#include <errno.h>
#include <unistd.h>

#include <pulse/rtclock.h>
#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sink.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/core-format.h>

#include "qahw-source.h"
#include "qahw-utils.h"
#include "qahw-source-extn.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>

#define PA_ALTERNATE_SOURCE_RATE 44100
//#define SOURCE_DUMP_ENABLED

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
} qahw_source_data;

typedef struct {
    bool first;
    pa_source *source;
    pa_rtpoll *rtpoll;
    pa_thread_mq thread_mq;
    pa_thread *thread;
    pa_idxset *formats;
} pa_source_data;

typedef struct {
    qahw_source_data *qahw_sdata;
    pa_source_data *pa_sdata;
    pa_qahw_source_extn_handle_t *source_extn_handle;
} pa_qahw_source_data;

static int restart_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                              audio_input_flags_t flags, int source_id, qahw_source_data *qahw_sdata);
static int create_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                              audio_input_flags_t flags, int source_id, pa_qahw_source_data *sdata);
static int close_qahw_source(qahw_source_data *qahw_sdata);

static const uint32_t supported_source_rates[] =
                          {8000, 11025, 16000, 22050, 44100, 48000, 96000, 192000};

static const char *pa_qahw_source_get_name_from_flags(audio_input_flags_t flags) {
    const char *name = NULL;

    if (flags == AUDIO_INPUT_FLAG_NONE)
        name = "audio-record";
    else if (flags == AUDIO_INPUT_FLAG_FAST)
        name = "record-low-latency";
    else if (flags & QAHW_INPUT_FLAG_COMPRESS)
        name = "record-compress";

    return name;
}

static void pa_qahw_source_fill_info(qahw_source_data *qahw_sdata, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                                audio_input_flags_t flags, int source_iohandle) {

    if (encoding == PA_ENCODING_PCM)
        qahw_sdata->config.format = pa_qahw_util_get_qahw_format_from_pa_sample(ss->format);
    else
        qahw_sdata->config.format = pa_qahw_util_get_qahw_format_from_pa_encoding(encoding);

    qahw_sdata->config.sample_rate = ss->rate;
    qahw_sdata->config.channel_mask = audio_channel_in_mask_from_count(ss->channels);

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
}

static int pa_qahw_source_start(qahw_source_data *sdata) {
    return 0;
}

static int pa_qahw_source_standby(qahw_source_data *sdata) {
    pa_assert(sdata);
    pa_assert(sdata->in_handle);

    pa_log_info("%s", __func__);

    qahw_in_standby(sdata->in_handle);

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

    kvpair = pa_sprintf_malloc("%s=%d", QAHW_PARAMETER_STREAM_ROUTING, port_device_data->device);
    pa_log_info("port name: %s kvpair %s device 0x%x", p->name, kvpair, port_device_data->device);

    rc = qahw_in_set_parameters(source_data->qahw_sdata->in_handle, kvpair);
    if (rc)
        pa_log_error("qahw in routing failed %d",rc);

    pa_xfree(kvpair);

    return rc;
}

static int pa_qahw_source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
	pa_qahw_source_data *source_data = (pa_qahw_source_data *)(PA_SOURCE(o)->userdata);
    int r = 0;

    pa_assert(source_data);
    pa_assert(source_data->pa_sdata->source);

    switch (code) {
        case PA_SOURCE_MESSAGE_SET_STATE: {
            pa_source_state_t new_state = (pa_source_state_t) PA_PTR_TO_UINT(data);
            pa_log_debug("New state is: %d", new_state);

            if (PA_SOURCE_IS_OPENED(new_state) && !PA_SOURCE_IS_OPENED(source_data->pa_sdata->source->thread_info.state))
                r = pa_qahw_source_start(source_data->qahw_sdata);
            else if (new_state == PA_SOURCE_SUSPENDED)
                r = pa_qahw_source_standby(source_data->qahw_sdata);

            /* Error */
            if (r < 0)
                return r;

            break;
        }
        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            *((pa_usec_t*) data) = 0;
            return 0;
        }
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static int pa_qahw_source_reconfigure_cb(pa_source *s, pa_sample_spec *spec, bool passthrough) {
    pa_encoding_t encoding = PA_ENCODING_INVALID;
    pa_format_info *f;
    pa_qahw_source_data *sdata = (pa_qahw_source_data *) s->userdata;
    pa_source_data *pa_sdata = NULL;
    qahw_source_data *qahw_sdata = NULL;

    bool supported = false;
    uint32_t i, rc;
    uint32_t old_rate;

    pa_assert(s);
    pa_assert(s->userdata);
    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->qahw_sdata);

    pa_sdata = sdata->pa_sdata;
    qahw_sdata = sdata->qahw_sdata;

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
        old_rate = pa_sdata->source->sample_spec.rate; /*take backup*/
        pa_sdata->source->sample_spec.rate = spec->rate;

        PA_IDXSET_FOREACH(f, pa_sdata->formats, i) {
            /* currently a source supports single format */
            encoding = f->encoding;
            break;
        }

        if (encoding == PA_ENCODING_INVALID) {
            pa_log_info("Format not populated ");
            return -1;
        }

        qahw_sdata->devices = *((audio_devices_t *)PA_DEVICE_PORT_DATA(pa_sdata->source->active_port));

        pa_log_info("Updating rate for device %d, new rate is %d", qahw_sdata->devices, spec->rate);

        rc = restart_qahw_source(qahw_sdata->module_handle, encoding, &pa_sdata->source->sample_spec, &pa_sdata->source->channel_map, qahw_sdata->devices,
                                qahw_sdata->flags, qahw_sdata->handle, qahw_sdata);
        if (PA_UNLIKELY(rc)) {
            pa_sdata->source->sample_spec.rate = old_rate; /*restore old rate if failed*/
            pa_log_error("Could create reopen qahw source, error %d", rc);
            return -1;
        }

        pa_qahw_source_extn_source_handle_update(sdata->source_extn_handle, qahw_sdata->in_handle);

        pa_source_set_fixed_latency(pa_sdata->source, pa_bytes_to_usec(qahw_sdata->source_buffer_size, &s->sample_spec));
        return 0;
    }

    pa_log_info("Source could not set sample rate of %d Hz", spec->rate);
    return -1;
}

static pa_idxset* pa_qahw_source_get_formats(pa_source *s) {
    pa_qahw_source_data *sdata = (pa_qahw_source_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);

    return pa_idxset_copy(sdata->pa_sdata->formats, (pa_copy_func_t) pa_format_info_copy);
}

static void pa_qahw_source_thread_func(void *userdata) {
    pa_qahw_source_data *source_data = (pa_qahw_source_data *)userdata;
    pa_source_data *pa_sdata = source_data->pa_sdata;
    qahw_source_data *qahw_sdata = source_data->qahw_sdata;

    pa_log_debug("Source IO Thread starting up");

    pa_thread_mq_install(&pa_sdata->thread_mq);

    for (;;) {
        int ret;
        bool wait = true;

        if (PA_SOURCE_IS_OPENED(pa_sdata->source->thread_info.state)) {
            pa_memchunk chunk;
            void *data;
            qahw_in_buffer_t in_buf;

            memset(&in_buf, 0, sizeof(qahw_in_buffer_t));

            chunk.memblock = pa_memblock_new(pa_sdata->source->core->mempool, (size_t) qahw_sdata->source_buffer_size);
            data = pa_memblock_acquire(chunk.memblock);
            chunk.length = pa_memblock_get_length(chunk.memblock);
            chunk.index = 0;

            in_buf.buffer = data;
            in_buf.bytes = chunk.length;

            if ((ret = qahw_in_read(qahw_sdata->in_handle, &in_buf)) < 0) 
                pa_log_error("Could not read data: %d qahw handle %p", ret, qahw_sdata->in_handle);
            else 
                chunk.length = ret;
#ifdef SOURCE_DUMP_ENABLED
            pa_log_error(" chunk length %d chunk index %d in_buf.bytes %d ",chunk.length, chunk.index, ret);
            if ((ret = write(qahw_sdata->write_fd, in_buf.buffer, ret)) < 0)
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

static int open_qahw_source(qahw_module_handle_t *module_handle, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t devices,
                              audio_input_flags_t flags, int source_id, qahw_source_data *qahw_sdata) {
    int rc;
#ifdef SOURCE_DUMP_ENABLED
    char *file_name;
#endif

    pa_assert(ss);
    pa_assert(map);
    pa_assert(module_handle);
    pa_assert(qahw_sdata);
    pa_qahw_source_fill_info(qahw_sdata, encoding, ss, map, devices, flags, source_id);
#ifdef SOURCE_DUMP_ENABLED
    file_name = pa_sprintf_malloc("/data/pcmdump_source_%d", qahw_sdata->handle);

    qahw_sdata->write_fd = open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
    if(qahw_sdata->write_fd < 0)
        pa_log_error("Could not open write fd %d for source index %d", qahw_sdata->write_fd, qahw_sdata->handle);

    pa_xfree(file_name);
#endif

    pa_log_debug("opening source with configuration flag = 0x%x, encoding %d,format %d, sample_rate %d, channel_mask 0x%x device %d",
                 qahw_sdata->flags, encoding, qahw_sdata->config.format, qahw_sdata->config.sample_rate, qahw_sdata->config.channel_mask, qahw_sdata->devices);

    rc = qahw_open_input_stream(module_handle, qahw_sdata->handle, qahw_sdata->devices, &qahw_sdata->config, &qahw_sdata->in_handle, qahw_sdata->flags,
                                qahw_sdata->device_url, AUDIO_SOURCE_MIC);
    if (rc) {
        qahw_sdata->in_handle = NULL;
        pa_log_error("Could not open input stream %d", rc);
        goto fail;
    }
    qahw_sdata->module_handle = module_handle;

    pa_log_debug("qahw source opened %p", qahw_sdata->in_handle);

    qahw_sdata->source_buffer_size = qahw_in_get_buffer_size(qahw_sdata->in_handle);
    if (qahw_sdata->source_buffer_size <= 0) {
        qahw_close_input_stream(qahw_sdata->in_handle);
        rc = -1;
        goto fail;
    }

fail:
    return rc;
}

static int close_qahw_source(qahw_source_data *qahw_sdata) {
    int rc = -1;

    pa_assert(qahw_sdata);
    pa_assert(qahw_sdata->in_handle);

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

    rc = open_qahw_source(module_handle, encoding, ss, map, devices, flags, source_id, qahw_sdata);
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
                              audio_input_flags_t flags, int source_id, pa_qahw_source_data *sdata) {
   int rc;

   sdata->qahw_sdata = pa_xnew0(qahw_source_data, 1);

   rc = open_qahw_source(module_handle, encoding, ss, map, devices, flags, source_id, sdata->qahw_sdata);
   if (rc) {
       pa_log_error("open_qahw_source failed, error %d", rc);
       pa_xfree(sdata->qahw_sdata);
       sdata->qahw_sdata = NULL;
   }

    return rc;
}

static int create_pa_source(pa_module *m, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, char *source_name, pa_card *card,
                           const char *profile_name, const char *driver, pa_qahw_source_data *source_data,
                           pa_qahw_card_source_usecase_id_t source_id, pa_qahw_card_usecase_type_t usecase_type) {
    pa_source_new_data new_data;
    pa_source_data *pa_sdata;
    qahw_source_data *qahw_sdata = NULL;
    pa_device_port *port;
    pa_card_profile *profile;
    void *state, *state2;
    pa_format_info *format;
    pa_sample_spec new_ss;

    bool port_source_mapping = false;

    pa_qahw_card_port_device_data *port_device_data;

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

    memcpy(&new_ss, ss, sizeof(pa_sample_spec));

    /* convert to transmission to media rate, as pa expects same
       for PA_ENCODING_UNKNOWN_4X_IEC61937 media_rate = transmission_rate/4 */
    if (encoding == PA_ENCODING_UNKNOWN_4X_IEC61937)
        new_ss.rate = ss->rate / 4;

    pa_log_info("ss->rate %d ss->channels %d", new_ss.rate, new_ss.channels);
    pa_source_new_data_set_sample_spec(&new_data, &new_ss);
    pa_source_new_data_set_channel_map(&new_data, map);
    pa_source_new_data_set_alternate_sample_rate(&new_data, PA_ALTERNATE_SOURCE_RATE);

    /* associate port with source, first get port in a card then for each profile in that port check if matches with input profile */
    PA_HASHMAP_FOREACH(port, card->ports, state) {
        PA_HASHMAP_FOREACH(profile, port->profiles, state2) {
            port_device_data = PA_DEVICE_PORT_DATA(port);
            pa_assert(port_device_data);

            if (!((port->direction & PA_DIRECTION_INPUT) && (port_device_data->usecase_id.source_id & source_id)))
                continue;

            profile = pa_hashmap_get(port->profiles, profile_name);
                pa_log_error(" port %s to source %s", port->name, source_name);

            if ((profile) && pa_streq(profile->name, profile_name)) {
                pa_log_error("adding port %s to source %s", port->name, source_name);
                port_source_mapping = true;
                pa_assert_se(pa_hashmap_put(new_data.ports, port->name, port) >= 0);
                pa_device_port_ref(port);
            }
        }
    }

    if (!port_source_mapping) {
        pa_log_error("%s: source_id %d creation failed as no port mapped, ",__func__, source_id);
        goto fail;
    }

    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_STRING, pa_qahw_source_get_name_from_flags(source_data->qahw_sdata->flags));

    pa_sdata->source = pa_source_new(m->core, &new_data, PA_SOURCE_HARDWARE);
    if (!pa_sdata->source) {
        pa_log_error("Could not create source");
        goto fail;
    }

    pa_log_info("pa source opened %p", pa_sdata->source);
    pa_source_new_data_done(&new_data);

    pa_sdata->source->userdata = (void *)source_data;
    pa_sdata->source->parent.process_msg = pa_qahw_source_process_msg;
    pa_sdata->source->set_port = pa_qahw_source_set_port_cb;
    pa_sdata->source->reconfigure = pa_qahw_source_reconfigure_cb;
    pa_sdata->source->get_formats = pa_qahw_source_get_formats;
    pa_source_set_asyncmsgq(pa_sdata->source, pa_sdata->thread_mq.inq);
    pa_source_set_rtpoll(pa_sdata->source, pa_sdata->rtpoll);

    /* Source only supports only one format */
    pa_sdata->formats = pa_idxset_new(NULL, NULL);

    /* dynamic source support only single static configuration and pa client is supposed open session with same configuration hence publish
       complete source config.
    */
    if ((usecase_type == PA_QAHW_CARD_USECASE_TYPE_DYNAMIC) && (encoding == PA_ENCODING_PCM))
        format = pa_format_info_from_sample_spec2(&new_ss, map, true, true, true);
    /* for non pcm and dynamic source,  only publish encoding and sample rate, other format params are determined ased on format */
    else if (usecase_type == PA_QAHW_CARD_USECASE_TYPE_DYNAMIC) {
        format = pa_format_info_new();
        pa_format_info_set_rate(format, new_ss.rate);
    /* for non pcm only publish encoding */
    } else  {
        format = pa_format_info_new();
    }

    format->encoding = encoding;

    pa_idxset_put(pa_sdata->formats, format, NULL);

    qahw_sdata = source_data->qahw_sdata;

    pa_source_set_max_rewind(pa_sdata->source, 0);
    pa_source_set_fixed_latency(pa_sdata->source, pa_bytes_to_usec(qahw_sdata->source_buffer_size, &new_ss));

    pa_sdata->thread = pa_thread_new(source_name, pa_qahw_source_thread_func, source_data);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

    /* keep pa source and qahw port in sync, qahw is opened with some default port, update qahw with active port decided by pa source */
    pa_qahw_source_set_port_cb(pa_sdata->source, pa_sdata->source->active_port);

    pa_source_put(pa_sdata->source);

    pa_xfree(source_name);

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

    pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);

    pa_thread_mq_done(&pa_sdata->thread_mq);

    pa_rtpoll_free(pa_sdata->rtpoll);

    return 0;
}

int pa_qahw_source_get_config(pa_qahw_source_handle_t *handle, pa_sample_spec *ss, pa_channel_map *map, pa_encoding_t *encoding) {
    pa_qahw_source_data *sdata = (pa_qahw_source_data *)handle;
    pa_format_info *f;

    uint32_t i;
    int ret = -1;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->source);

    PA_IDXSET_FOREACH(f, sdata->pa_sdata->formats, i) {
        /* currently a source supports single format */
        pa_format_info_to_sample_spec(f, ss, map);
        *encoding = f->encoding;
        ret = 0;
        break;
    }

    return ret;
}

int pa_qahw_source_create(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name, const char *profile_name,
                          pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t source_devices, int32_t flags,
                          pa_qahw_card_source_usecase_id_t source_id, pa_qahw_card_usecase_type_t usecase_type, pa_qahw_source_handle_t **handle) {
    int rc;
    char *name;
    pa_qahw_source_data *sdata;

    pa_assert(m);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module_handle);
    pa_assert(module_name);
    pa_assert(profile_name);
    pa_assert(ss);
    pa_assert(map);

    pa_log_debug("Opening source for profile %s", profile_name);
    sdata = pa_xnew0(pa_qahw_source_data, sizeof(pa_qahw_source_data));

    rc = create_qahw_source(module_handle, encoding, ss, map, source_devices, flags, source_id, sdata);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could not open qahw source, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    name = pa_sprintf_malloc("qahw_source.%s_%s_%d", module_name, pa_qahw_source_get_name_from_flags(flags), source_id);
    pa_log_debug("Opening source for profile %s with name %s", profile_name, name);
    rc = create_pa_source(m, encoding, ss, map, name, card, profile_name, driver, sdata, source_id, usecase_type);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa source for source %s, error %d", name, rc);
        free_qahw_source(sdata->qahw_sdata);
        pa_xfree(sdata);
        sdata = NULL;
    }

    rc = pa_qahw_source_extn_create(sdata->pa_sdata->source->core, sdata->qahw_sdata->in_handle, sdata->pa_sdata->source->index, &sdata->source_extn_handle);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create qahw source extn %s, error %d", name, rc);
        free_qahw_source(sdata->qahw_sdata);
        free_pa_source(sdata->pa_sdata);
        pa_xfree(sdata);
        sdata = NULL;
    }


    *handle = (pa_qahw_source_handle_t *)sdata;

exit:
    return rc;
}

void pa_qahw_source_close(pa_qahw_source_handle_t *handle) {
    pa_qahw_source_data *sdata = (pa_qahw_source_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->qahw_sdata);
    pa_assert(sdata->pa_sdata);

    pa_qahw_source_extn_free(sdata->source_extn_handle);
    free_pa_source(sdata->pa_sdata);
    free_qahw_source(sdata->qahw_sdata);
    pa_xfree(sdata);
}
