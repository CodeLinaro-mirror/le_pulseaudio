/***
    Copyright (c)  {2019} The Linux Foundation. All rights reserved.

    This file is part of PulseAudio.

    Copyright 2010 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#define AUDIO_BUFFER_DUMP 0

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "pulsecore/config.h"

#include <pulsecore/module.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/dbus-util.h>

#define MOD_EXPORT __attribute__((visibility("default")))

MOD_EXPORT int pa__init(pa_module *m);
MOD_EXPORT void pa__done(pa_module *m);
MOD_EXPORT int pa__get_n_used(pa_module *m);
MOD_EXPORT const char *pa__get_author(void);
MOD_EXPORT const char *pa__get_description(void);
MOD_EXPORT const char *pa__get_usage(void);
MOD_EXPORT const char *pa__get_version(void);
MOD_EXPORT const char *pa__get_deprecated(void);
MOD_EXPORT bool pa__load_once(void);

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <gst/audio/audio-channels.h>
#include <pulsecore/thread.h>
#include <dlfcn.h>

#if AUDIO_BUFFER_DUMP
#include <fcntl.h>
#endif //AUDIO_BUFFER_DUMP

#include "inc/module-filter-sink.h"

PA_MODULE_AUTHOR("Linux Foundation");
PA_MODULE_DESCRIPTION(_("Audio filter effect sink"));
PA_MODULE_VERSION( PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE( false);
PA_MODULE_USAGE(_(
        "sink_name=<name for the sink> "
        "sink_properties=<properties for the sink> "
        "master=<name of sink to filter> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map> "
        /* Provide filter plugins which are needed for processing raw audio as a argument like
         * filter_plugins=audioconvert. It will work as appsrc -> filter plugins -> appsink.
         * Provide only valid audio processing plugins as an arguments like audioconvert, audiorate etc.
         * If want to pass multiple plugins then separate them with "," like
         * filter_plugins=audiorate,audioconvert. */
        "filter_plugins=<filter plugins> "
        /* Provide filter plugin module name along with path, needed for prepare gstreamer pipeline and
         * to communicate using dbus while pipeline is runnning */
        "filter_plugin_module=<filter_plugins_module> "
        /* Provide filter properties which will be pass to the element in gstreamer pipeline
         * Example: filter_properties=<property1>=<value1>,<property2>=<value2>  */
        "filter_properties=<filter_properties> "
));

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)

/* Pipeline states  */
typedef enum {
    G_READY,
    G_PAUSED,
    G_PLAYING
} g_state;

struct userdata {
    pa_module *module;

    pa_sink *sink;
    pa_sink_input *sink_input;

    pa_memblockq *memblockq;

    pa_sample_format_t input_format;
    unsigned input_rate;
    unsigned input_channels;
    unsigned input_bit_depth;

    pa_sample_format_t output_format;
    unsigned output_rate;
    unsigned output_channels;
    unsigned output_bit_depth;

    pa_thread *thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    filter_plugin_handle filter_hdl;
    GstCaps *appsrc_caps;

    g_state pipeline_state;
    void *dl_handle;
    filter_plugin_cb filter_plugins_callbacks;

    pa_dbus_protocol *dbus_protocol;
    char *dbus_path;

#if AUDIO_BUFFER_DUMP
    int fd;
    int fd_out;
    int write_type;
#endif //AUDIO_BUFFER_DUMP
};

static const char* const valid_modargs[] = {
        "sink_name",
        "sink_properties",
        "sink_master",
        "format",
        "rate",
        "channels",
        "channel_map",
        "filter_plugins",
        "filter_plugin_module",
        "filesink_enable",
        "filesink_location",
        "filter_properties",
        "autoloaded",
        NULL
};

/* PulseAudio-GStreamer channel map struct */
typedef struct {
    pa_channel_position_t pa_chls;
    GstAudioChannelPosition gst_chls;
} pa_gst_chl_map_t;

/* PulseAudio-GStreamer channel map table */
const pa_gst_chl_map_t pa_gst_chl_map_table[] = {
        { PA_CHANNEL_POSITION_FRONT_LEFT,GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT },
        { PA_CHANNEL_POSITION_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT },
        { PA_CHANNEL_POSITION_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER },
        { PA_CHANNEL_POSITION_LFE, GST_AUDIO_CHANNEL_POSITION_LFE1 },
        { PA_CHANNEL_POSITION_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_LEFT },
        { PA_CHANNEL_POSITION_REAR_RIGHT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT },
        { PA_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT },
        { PA_CHANNEL_POSITION_SIDE_RIGHT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT },
        { PA_CHANNEL_POSITION_TOP_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT },
        { PA_CHANNEL_POSITION_TOP_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT },
        { PA_CHANNEL_POSITION_TOP_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT },
        { PA_CHANNEL_POSITION_TOP_REAR_RIGHT, GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT }
};

static void dbus_init(struct userdata *u) {

    pa_log("FUNC : %s ", __func__);
    pa_assert_se(u);

    GList *list = NULL;
    filter_element_info *filter_plugin = NULL;
    u->dbus_protocol = NULL;
    u->dbus_path = NULL;

    u->dbus_protocol = pa_dbus_protocol_get(u->sink->core);

    for (list = u->filter_hdl.filter_plugins; list; list = list->next) {
        filter_plugin = list->data;

        if (u->dl_handle) {
            if (!u->dbus_path)
                u->dbus_path = pa_sprintf_malloc("%s_%d", u->filter_plugins_callbacks.get_dbus_path(filter_plugin->name),
                        u->filter_hdl.filesink_enable ? 3 : u->sink->index);

            filter_plugin->dbus_interface_info = u->filter_plugins_callbacks.get_dbus_inteface_info(filter_plugin->name);
        }

        if (u->dbus_path && filter_plugin->dbus_interface_info)
            pa_assert_se(
                    pa_dbus_protocol_add_interface(u->dbus_protocol, u->dbus_path, filter_plugin->dbus_interface_info,
                            filter_plugin->dbus_handle) >= 0);
    }
}

static void dbus_done(struct userdata *u) {
    pa_log("FUNC : %s ", __func__);
    pa_assert_se(u);

    GList *list = NULL;
    filter_element_info *filter_plugin = NULL;

    if (!u->dbus_protocol) {
        pa_assert(!u->dbus_path);
        return;
    }

    for (list = u->filter_hdl.filter_plugins; list; list = list->next) {
        filter_plugin = list->data;

        if (filter_plugin->dbus_interface_info)
            pa_assert_se(
                    pa_dbus_protocol_remove_interface(u->dbus_protocol, u->dbus_path,
                            filter_plugin->dbus_interface_info->name) >= 0);
    }

    pa_xfree(u->dbus_path);
    pa_dbus_protocol_unref(u->dbus_protocol);

    u->dbus_path = NULL;
    u->dbus_protocol = NULL;
}

static void gst_enqueue_filter_plugin(struct userdata *u, gchar *plugin_name);
static void gst_dequeue_all_filter_plugin(struct userdata *u);
static int start_gstreamer(void *);
static void push_buffer(struct userdata *, char *, int);
static char* pull_data(struct userdata *, int *);
static int gstreamer_init(void *);
static guint64 retrieve_gst_mask_from_pa(struct userdata *u);

static void gst_enqueue_filter_plugin(struct userdata *u, gchar *plugin_name) {
    filter_element_info *filter_plugin = NULL;

    pa_assert(u);

    filter_plugin = g_slice_new(filter_element_info);
    filter_plugin->name = g_strdup(plugin_name);
    filter_plugin->element = NULL;
    filter_plugin->dbus_handle = NULL;
    filter_plugin->dbus_interface_info = NULL;

    u->filter_hdl.filter_plugins = g_list_append(u->filter_hdl.filter_plugins, filter_plugin);
    return;
}

static void gst_dequeue_all_filter_plugin(struct userdata *u) {
    GList *list = NULL;
    filter_element_info *filter_plugin = NULL;

    pa_assert(u);

    for (list = u->filter_hdl.filter_plugins; list; list = list->next) {
        filter_plugin = list->data;
        g_free(filter_plugin->name);
        g_slice_free(filter_element_info, filter_plugin);
    }

    g_list_free(u->filter_hdl.filter_plugins);
    u->filter_hdl.filter_plugins = NULL;
    return;
}

static void gstreamer_thread(void *userdata) {
    struct userdata *u = userdata;
    pa_assert(u);

    GstState state = GST_STATE_NULL;
    GstCaps *new_caps;
    GstAudioInfo audio_info;
    GstAudioFormat gst_format;
    guint64 channel_mask = 0;
    GstAudioChannelPosition position[64] = { GST_AUDIO_CHANNEL_POSITION_NONE };
    GList *list = NULL;
    filter_element_info *filter_plugin = NULL;
    GstElement *src_element = NULL;
    int i = 0, ret = 0;

    /* Initialization */
    gst_init(NULL, NULL);

    u->filter_plugins_callbacks.create_gst_pipeline(&u->filter_hdl);
    if (!u->filter_hdl.pipeline) {
        pa_log("create_gst_pipeline failed, terminating");
        goto gst_launch_fail;
    }

    /* Create dbus handle and assign element to handle */
    for (list = u->filter_hdl.filter_plugins; list; list = list->next) {
        filter_plugin = list->data;

        if (u->dl_handle) {
            u->filter_plugins_callbacks.init(filter_plugin->name, &filter_plugin->dbus_handle);
            u->filter_plugins_callbacks.set_gst_element(filter_plugin->name, filter_plugin->dbus_handle,
                    filter_plugin->element);
        }
    }

    /* Prepare appsrc caps */
    channel_mask = retrieve_gst_mask_from_pa(u);
    pa_log("channel_mask %llx", channel_mask);

    if (u->input_format == PA_SAMPLE_S16LE) {
        u->input_bit_depth = 2;
        gst_format = GST_AUDIO_FORMAT_S16LE;
    } else if (u->input_format == PA_SAMPLE_S24LE) {
        u->input_bit_depth = 3;
        gst_format = GST_AUDIO_FORMAT_S24LE;
    } else if (u->input_format == PA_SAMPLE_FLOAT32LE) {
        u->input_bit_depth = 4;
        gst_format = GST_AUDIO_FORMAT_F32LE;
    } else if (u->input_format == PA_SAMPLE_S32LE) {
        u->input_bit_depth = 4;
        gst_format = GST_AUDIO_FORMAT_S32LE;
    } else {
        pa_log("Error retrieving gstreamer audio format");
        goto gst_launch_fail;
    }

    if (u->output_format == PA_SAMPLE_S16LE) {
        u->output_bit_depth = 2;
    } else if (u->output_format == PA_SAMPLE_S24LE) {
        u->output_bit_depth = 3;
    } else if (u->output_format == PA_SAMPLE_FLOAT32LE) {
        u->output_bit_depth = 4;
    } else if (u->output_format == PA_SAMPLE_S32LE) {
        u->output_bit_depth = 4;
    } else {
        pa_log("Error retrieving gstreamer audio format");
        goto gst_launch_fail;
    }

    gst_audio_info_init(&audio_info);
    gst_audio_info_set_format(&audio_info, gst_format, u->input_rate, u->input_channels, position);

    new_caps = gst_audio_info_to_caps(&audio_info);
    if (new_caps == NULL) {
        pa_log("Unable to set caps");
        goto gst_launch_fail;
    }

    g_object_set(G_OBJECT(u->filter_hdl.appsrc), "caps", new_caps, NULL);
    u->appsrc_caps = (GstCaps *) gst_caps_copy(new_caps);

    /* Start pipeline so it could process incoming data */
    pa_log("Setting g_main_loop_run to GST_STATE_PLAYING");
    if (gst_element_set_state(u->filter_hdl.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        pa_log("Unable to set the pipeline to the playing state");
        gst_object_unref(u->filter_hdl.pipeline);
        u->filter_hdl.pipeline = NULL;
        goto gst_launch_fail;
    }

    /* Notify client module on gstreamer pipeline status */
    pthread_mutex_lock(&u->mutex);
    pthread_cond_signal(&u->cond);
    pthread_mutex_unlock(&u->mutex);

    dbus_init(u);

    /* Loop will run until pipeline enters NULL state, will block here */
    do {
        /* API doc says :
         * For elements that did not return GST_STATE_CHANGE_ASYNC,
         * this function returns the current and pending state immediately.
         * not sure if it blocking or killing CPU cycles, need to confirm ? */
        gst_element_get_state(u->filter_hdl.pipeline, &state, NULL, GST_CLOCK_TIME_NONE);
    } while (state != GST_STATE_NULL);

    /* Reaches here once EOS received */
    pa_log("g_main_loop_run returned, stopping playback");
    ret = 1;

    dbus_done(u);

gst_launch_fail:
    /* Release dbus handle */
    for (list = u->filter_hdl.filter_plugins; list; list = list->next) {
        filter_plugin = list->data;

        if (!filter_plugin->dbus_handle)
            continue;

        if (u->dl_handle)
            u->filter_plugins_callbacks.deinit(filter_plugin->name, filter_plugin->dbus_handle);
    }

    pa_log("Deleting pipeline");

    /* This will delete all pipeline elements */
    if (u->filter_hdl.pipeline) {
        pa_log("Unreferring pipeline");
        gst_object_unref(GST_OBJECT(u->filter_hdl.pipeline));
        u->filter_hdl.pipeline = NULL;
        pa_log("Pipeline unreferred");
    }

    if (!ret) {
        pthread_mutex_lock(&u->mutex);
        pthread_cond_signal(&u->cond);
        pthread_mutex_unlock(&u->mutex);
    }

    return NULL;
}

static int start_gstreamer(void *userdata) {
    struct userdata *u = userdata;
    GstState state = GST_STATE_NULL;
    int ret = -1;

    if (!(u->thread = pa_thread_new("gstreamer_thread", gstreamer_thread, u))) {
        pa_log("Failed to create gstreamer thread");
        return ret;
    }

    /* Wait for a while so that worker thread
     * finishes initializing gstreamer pipeline
     */
    pthread_mutex_lock(&u->mutex);
    pthread_cond_wait(&u->cond, &u->mutex);
    pthread_mutex_unlock(&u->mutex);

    if (u->filter_hdl.pipeline) {
        if (GST_STATE(u->filter_hdl.pipeline) == GST_STATE_READY) {
            state = GST_STATE_READY;

            if (state == GST_STATE_READY) {
                ret = 0;
            } else {
                pa_log("Gstreamer launch failed to run");
            }
        }
    }

    return ret;
}

static void push_buffer(struct userdata *u, char *data, int size) {
    GstFlowReturn ret;
    GstBuffer *buffer;
    GstMapInfo info;

    pa_log("Enter %s", __func__);

    buffer = gst_buffer_new_allocate(NULL, size, NULL);
    gst_buffer_map(buffer, &info, GST_MAP_WRITE);
    memmove(info.data, data, size);

    gst_buffer_unmap(buffer, &info);

    ret = gst_app_src_push_buffer(GST_APP_SRC(u->filter_hdl.appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        pa_log("appsrc push failed");
    }

    pa_log("%s done, size %d", __func__, size);
    return;
}

static char* pull_data(struct userdata *u, int* out_len) {
    GstSample* sample;
    GstBuffer* buffer;
    GstMapInfo map;
    char* ret_ptr = NULL;

    sample = gst_app_sink_pull_sample(GST_APP_SINK(u->filter_hdl.appsink));
    if (sample == NULL) {
        pa_log("gst_app_sink_pull_sample failed");
        return NULL;
    }
    pa_log("sink sample pulled");

    buffer = gst_sample_get_buffer(sample);
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    ret_ptr = (char *) malloc(map.size);

    memmove(ret_ptr, map.data, map.size);
    *out_len = map.size;

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    pa_log("%s done, size %d\n", __func__, *out_len);
    return ret_ptr;
}

static int gstreamer_init(void *userdata) {
    int ret = -1;
    ret = start_gstreamer(userdata);
    return ret;
}

static void set_g_state(struct userdata *u, g_state state) {
    pthread_mutex_lock(&u->mutex);
    u->pipeline_state = state;
    pthread_mutex_unlock(&u->mutex);
}

static g_state get_g_state(struct userdata *u) {
    g_state s;

    pthread_mutex_lock(&u->mutex);
    s = u->pipeline_state;
    pthread_mutex_unlock(&u->mutex);
    return s;
}

static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: {
            /* The sink is put before the sink input, don't access it now.
             * Also, the sink input is first shut down, then the sink.
             */
            if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)
                    || !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state)) {
                *((int64_t*) data) = 0;
                return 0;
            }

            *((int64_t*) data) =
                    /* Get the latency of the master sink */
                    pa_sink_get_latency_within_thread(u->sink_input->sink, true)
                    +
                    /* Add the latency internal to the sink input */
                    pa_bytes_to_usec(pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq),
                            &u->sink_input->sink->sample_spec);
            return 0;
        }

        case PA_SINK_MESSAGE_SET_STATE: {
            pa_sink_state_t new_state = (pa_sink_state_t) PA_PTR_TO_UINT(data);

            /* When set to running or idle for the first time, request a rewind
             * of the master sink to make sure we are heard immediately */
            if ((new_state == PA_SINK_IDLE || new_state == PA_SINK_RUNNING)
                    && u->sink->thread_info.state == PA_SINK_INIT) {
                pa_log_debug("Requesting rewind due to state change.");
                pa_sink_input_request_rewind(u->sink_input, 0, false, true, true);
            }
            break;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int sink_set_state_in_main_thread_cb(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;
    pa_log("Enter %s", __func__);

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(state) || !PA_SINK_INPUT_IS_LINKED(u->sink_input->state))
        return 0;

#if AUDIO_BUFFER_DUMP
    if ((state == PA_SINK_IDLE) && (u->sink_input->state == PA_SINK_INPUT_CORKED)) {
        u->fd = pa_open_cloexec("/data/pulseaudio_src.pcm", O_WRONLY | O_CREAT | O_TRUNC, 00666);

        if (u->fd < 0) {
            pa_log("Failed to create or open pulseaudio_src.pcm");
        }

        u->fd_out = pa_open_cloexec("/data/pulseaudio_dst.pcm", O_WRONLY | O_CREAT | O_TRUNC, 00666);

        if (u->fd_out < 0) {
            pa_log("Failed to create or open pulseaudio_dst.pcm");
        }
    }
#endif //AUDIO_BUFFER_DUMP

    if ((state == PA_SINK_SUSPENDED) && (u->sink_input->state == PA_SINK_INPUT_RUNNING)) {
        /* Add your code to handle filter module in suspended state  */
    }

    if ((state == PA_SINK_RUNNING) && (u->sink_input->state == PA_SINK_INPUT_RUNNING)) {
        pa_log("Set pipeline to playing state");

        if (!u->filter_hdl.filesink_enable) {
            if (u->filter_hdl.pipeline) {
                if (u->filter_hdl.appsrc) {
                    gst_app_src_set_caps(u->filter_hdl.appsrc, u->appsrc_caps);
                }

                if (gst_element_set_state(u->filter_hdl.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
                    pa_log("Unable to set the pipeline to the Playing state");
                    gst_object_unref(u->filter_hdl.pipeline);
                    u->filter_hdl.pipeline = NULL;
                }
            }
            set_g_state(u, G_PLAYING);
        }
        pa_log("Pipeline play state done");
    }

    if ((state == PA_SINK_IDLE) && (u->sink_input->state == PA_SINK_INPUT_RUNNING)) {
        pa_log("Set pipeline to Ready and then Paused state");
#if AUDIO_BUFFER_DUMP
        if (u->fd >= 0)
            pa_assert_se(pa_close(u->fd) == 0);
        if (u->fd_out >= 0)
            pa_assert_se(pa_close(u->fd_out) == 0);
#endif //AUDIO_BUFFER_DUMP

        if (u->filter_hdl.filesink_enable) {
            if (u->filter_hdl.pipeline) {
                gst_element_send_event(u->filter_hdl.pipeline, gst_event_new_eos());
            }
        } else {
            if (u->filter_hdl.pipeline) {
                set_g_state(u, G_PAUSED);

                if (u->filter_hdl.appsrc) {
                    gst_app_src_set_caps(u->filter_hdl.appsrc, NULL);
                }

                if (gst_element_set_state(u->filter_hdl.pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
                    pa_log("Unable to set the pipeline to the Ready state");
                    gst_object_unref(u->filter_hdl.pipeline);
                    u->filter_hdl.pipeline = NULL;
                }

                if (gst_element_set_state(u->filter_hdl.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
                    pa_log("Unable to set the pipeline to the Playing state");
                    gst_object_unref(u->filter_hdl.pipeline);
                    u->filter_hdl.pipeline = NULL;
                }
            }
        }
        pa_log("Pipeline manipulations done");
    }

    pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);

    pa_log("Exit %s", __func__);
    return 0;
}

static void sink_request_rewind_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) || !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    /* Hand it over to the master sink */
    pa_sink_input_request_rewind(u->sink_input, s->thread_info.rewind_nbytes + pa_memblockq_get_length(u->memblockq),
            true, false, false);
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) || !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    /* Hand it over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(u->sink_input, pa_sink_get_requested_latency_within_thread(s));
}

static void sink_set_mute_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(s->state) || !PA_SINK_INPUT_IS_LINKED(u->sink_input->state))
        return;

    pa_sink_input_set_mute(u->sink_input, s->muted, s->save_muted);
}

static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;
    char *src, *dst;
    size_t fs;
    unsigned n;
    int len = 0;
    char *buf = NULL;

    pa_memchunk tchunk;
    pa_usec_t current_latency
    PA_GCC_UNUSED;

    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = i->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return -1;

    /* Plugins require sample size of 256 blocks  */
    if (!u->filter_hdl.filesink_enable)
        nbytes = 256 * u->input_channels * u->input_bit_depth;

    /* Process rewind request that is queued up */
    pa_sink_process_rewind(u->sink, 0);

    pa_sink_render_full(u->sink, nbytes, &tchunk);

    /* In case of fixed block size, next line is not needed */
    tchunk.length = PA_MIN(nbytes, tchunk.length);

    if (!u->filter_hdl.filesink_enable)
        tchunk.length = nbytes;

    pa_assert(tchunk.length > 0);

    fs = pa_frame_size(&i->sample_spec);
    n = (unsigned) (tchunk.length / fs);

    pa_assert(n > 0);

    chunk->index = 0;
    chunk->length = n * fs;
    chunk->memblock = pa_memblock_new(i->sink->core->mempool, chunk->length);

    src = pa_memblock_acquire_chunk(&tchunk);
    dst = pa_memblock_acquire(chunk->memblock);

    if (!pa_memblock_is_silence(tchunk.memblock) && (get_g_state(u) == G_PLAYING)) {
#if AUDIO_BUFFER_DUMP
        if (pa_write(u->fd, src, n * fs, &u->write_type) < 0) {
            pa_log("Write to the pulseaudio_src.pcm failed");
            return -1;
        }
#endif //AUDIO_BUFFER_DUMP

        push_buffer(u, src, tchunk.length);

        if (!u->filter_hdl.filesink_enable)
            buf = pull_data(u, &len);

        if (buf != NULL && len != 0) {
            pa_memblock_release(chunk->memblock);
            pa_memblock_unref(chunk->memblock);

            chunk->length = len;
            chunk->memblock = pa_memblock_new(i->sink->core->mempool, chunk->length);

            dst = pa_memblock_acquire(chunk->memblock);
            memcpy(dst, buf, len);
#if AUDIO_BUFFER_DUMP
            if (pa_write(u->fd_out, dst, len, &u->write_type) < 0) {
                pa_log("Write to the pulseaudio_dst.pcm failed");
                return -1;
            }
#endif //AUDIO_BUFFER_DUMP
            free(buf);
        } else {
            pa_log("Could not read data from gstreamer app sink");

            if (u->filter_hdl.filesink_enable)
                memset(dst, '\0', n * fs);
            else
                memcpy(dst, src, n * fs);
        }
    } else {
        memcpy(dst, src, n * fs);
    }

    pa_memblock_release(tchunk.memblock);
    pa_memblock_release(chunk->memblock);
    pa_memblock_unref(tchunk.memblock);

    /* To get the latency
     * Get the latency of the master sink
     * Add the latency internal to the sink input
     */
    current_latency = pa_sink_get_latency_within_thread(i->sink, false)
                + pa_bytes_to_usec(pa_memblockq_get_length(i->thread_info.render_memblockq), &i->sink->sample_spec);

    return 0;
}

static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    size_t amount = 0;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* Return here if sink is not yet linked */
    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    if (u->sink->thread_info.rewind_nbytes > 0) {
        size_t max_rewrite;

        max_rewrite = nbytes + pa_memblockq_get_length(u->memblockq);
        amount = PA_MIN(u->sink->thread_info.rewind_nbytes, max_rewrite);
        u->sink->thread_info.rewind_nbytes = 0;

        if (amount > 0) {
            pa_memblockq_seek(u->memblockq, -(int64_t) amount, PA_SEEK_RELATIVE, true);
            /* Add some code here if filter is to be reset  */
        }
    }

    pa_sink_process_rewind(u->sink, amount);
    pa_memblockq_rewind(u->memblockq, nbytes);
}

static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_memblockq_set_maxrewind(u->memblockq, nbytes);
    pa_sink_set_max_rewind_within_thread(u->sink, nbytes);
}

static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* If fixed block size is needed round nbytes up to multiples of it here.
     * PA_ROUND_UP macro can be useful.
     */
    pa_sink_set_max_request_within_thread(u->sink, nbytes);
}

static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (PA_SINK_IS_LINKED(u->sink->thread_info.state))
        pa_sink_detach_within_thread(u->sink);
    pa_sink_set_rtpoll(u->sink, NULL);
}

static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_sink_set_rtpoll(u->sink, i->sink->thread_info.rtpoll);
    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    /* If fixed block size needed, add the latency for 1 block minus 1 sample */
    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);

    /* If fixed block size needed, round
     * pa_sink_input_get_max_request(i) up to multiples of it
     */
    pa_sink_set_max_request_within_thread(u->sink, pa_sink_input_get_max_request(i));
    pa_sink_set_max_rewind_within_thread(u->sink, pa_sink_input_get_max_rewind(i));

    if (PA_SINK_IS_LINKED(u->sink->thread_info.state))
        pa_sink_attach_within_thread(u->sink);
}

static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* The order here matters. First kill the sink so that streams
     * can properly be moved away while the sink input is
     * still connected to the master
     */
    pa_sink_input_cork(u->sink_input, true);
    pa_sink_unlink(u->sink);
    pa_sink_input_unlink(u->sink_input);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_sink_unref(u->sink);
    u->sink = NULL;

    pa_module_unload_request(u->module, true);
}

static void sink_input_mute_changed_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_mute_changed(u->sink, i->muted);
}

static guint64 retrieve_gst_mask_from_pa(struct userdata *u) {
    pa_assert(u);

    guint64 channel_mask = 0;
    int i, j;
    GstAudioChannelPosition position[64] = { GST_AUDIO_CHANNEL_POSITION_NONE };

    if (u->input_channels == 2 || u->input_channels == 6) {
        channel_mask = gst_audio_channel_get_fallback_mask(u->input_channels);
        pa_log("channel_mask %llx", channel_mask);
    } else if (u->input_channels == 8) {
        for (i = 0; i < u->input_channels; i++) {
            for (j = 0; j < 12; j++) {
                if (u->sink->channel_map.map[i] == pa_gst_chl_map_table[j].pa_chls) {
                    channel_mask |= (1 << pa_gst_chl_map_table[j].gst_chls);
                    break;
                }
            }
        }

        if ((channel_mask != 0x303f) && (channel_mask != 0xc3f)) {
            channel_mask = gst_audio_channel_get_fallback_mask(u->input_channels);
        }
        pa_log("channel_mask %llx", channel_mask);
    } else if (u->input_channels == 12) {
        for (i = 0; i < 12; i++) {
            channel_mask |= (1 << pa_gst_chl_map_table[i].gst_chls);
        }
        pa_log("channel_mask %llx", channel_mask);
    }

    return channel_mask;
}

static int sink_reconfigure_cb(pa_sink *s, pa_sample_spec *spec, pa_channel_map *map, bool passthrough) {
    struct userdata *u = s->userdata;
    pa_log("Enter %s", __func__);

    pa_assert(u);

    /* Between App source & filter plugin */
    GstCaps *new_caps;
    GstAudioInfo audio_info;
    GstAudioFormat gst_format;
    guint64 channel_mask = 0;
    GstAudioChannelPosition position[64] = { GST_AUDIO_CHANNEL_POSITION_NONE };

    if (!PA_SINK_IS_OPENED(s->state)) {
        pa_log("updating sampling rate from %u to %u", u->sink->sample_spec.rate, spec->rate);
        pa_log("updating format from %u to %u", u->sink->sample_spec.format, spec->format);
        pa_log("updating channels from %u to %u", u->sink->sample_spec.channels, spec->channels);

        u->sink->sample_spec.rate = spec->rate;
        u->sink->sample_spec.format = spec->format;
        u->sink->sample_spec.channels = spec->channels;

        u->sink->channel_map = *map;
    }

    u->input_rate = u->sink->sample_spec.rate;
    u->input_channels = u->sink->sample_spec.channels;
    u->input_format = u->sink->sample_spec.format;

    channel_mask = retrieve_gst_mask_from_pa(u);
    pa_log("channel_mask retrieved %llx", channel_mask);

    if (channel_mask == 0) {
        pa_log("Error retrieving gstreamer channel mask");
        return -1;
    }

    gst_audio_channel_positions_from_mask(u->input_channels, channel_mask, position);

    if (u->input_format == PA_SAMPLE_S16LE) {
        u->input_bit_depth = 2;
        gst_format = GST_AUDIO_FORMAT_S16LE;
    } else if (u->input_format == PA_SAMPLE_S24LE) {
        u->input_bit_depth = 3;
        gst_format = GST_AUDIO_FORMAT_S24LE;
    } else if (u->input_format == PA_SAMPLE_FLOAT32LE) {
        u->input_bit_depth = 4;
        gst_format = GST_AUDIO_FORMAT_F32LE;
    } else if (u->input_format == PA_SAMPLE_S32LE) {
        u->input_bit_depth = 4;
        gst_format = GST_AUDIO_FORMAT_S32LE;
    } else {
        pa_log("Error retrieving gstreamer audio format");
        return -1;
    }

    gst_audio_info_init(&audio_info);
    gst_audio_info_set_format(&audio_info, gst_format, u->input_rate, u->input_channels, position);
    new_caps = gst_audio_info_to_caps(&audio_info);

    if (new_caps == NULL) {
        pa_log("Unable to set caps");
        return -1;
    }

    if (u->filter_hdl.appsrc) {
        gst_app_src_set_caps(u->filter_hdl.appsrc, new_caps);
        gst_caps_unref(u->appsrc_caps);
        u->appsrc_caps = (GstCaps *) gst_caps_copy(new_caps);
    }

    pa_log("Exit %s", __func__);
    return 0;
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_sample_spec ss_output;
    pa_channel_map map;
    pa_channel_map map_output;
    pa_modargs *ma;
    pa_sink *master = NULL;
    pa_sink_input_new_data sink_input_data;
    pa_sink_new_data sink_data;
    pa_memchunk silence;
    const char *filter_plugins;
    const char *filter_plugin_module;

    void (*filter_plugin_callbacks)(filter_plugin_cb *);
    char *error = NULL;

    pa_log("Enter %s", __func__);
    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (!(master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "sink_master", NULL), PA_NAMEREG_SINK))) {
        pa_log("Master sink not found");
        goto fail;
    }

    if (!(u = pa_xnew0(struct userdata, 1))) {
        pa_log("Get memory for userdata failed");
        goto fail;
    }
    u->module = m;
    m->userdata = u;

    ss.format = PA_SAMPLE_S24LE;
    ss.rate = master->sample_spec.rate;
    ss.channels = 2;
    map = *(pa_channel_map *) pa_channel_map_init_stereo(&map);

    ss_output = master->sample_spec;
    ss_output.format = PA_SAMPLE_S24LE;
    map_output = master->channel_map;

    if (ss_output.channels >= 8) {
        ss_output.channels = 8;
        map_output = *(pa_channel_map *) pa_channel_map_init_auto(&map_output, 8, PA_CHANNEL_MAP_WAVEEX);
        map_output.map[6] = PA_CHANNEL_POSITION_TOP_FRONT_LEFT;
        map_output.map[7] = PA_CHANNEL_POSITION_TOP_FRONT_RIGHT;
    } else if (ss_output.channels >= 6) {
        ss_output.channels = 6;
        map_output = *(pa_channel_map *) pa_channel_map_init_auto(&map_output, 6, PA_CHANNEL_MAP_WAVEEX);
    }

    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss_output, &map_output, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    u->filter_hdl.filter_properties = pa_xstrdup(pa_modargs_get_value(ma, "filter_properties", NULL));
    if (!u->filter_hdl.filter_properties)
        pa_log("No properties given ");

    u->filter_hdl.filesink_enable = 0;
    u->filter_hdl.filesink_location = NULL;
    if (pa_modargs_get_value_boolean(ma, "filesink_enable", &u->filter_hdl.filesink_enable) < 0) {
        pa_log("filesink_enable= expects a boolean value as parameter");
        goto fail;
    }

    if (u->filter_hdl.filesink_enable) {
        ss = ss_output;
        map = map_output;

        u->filter_hdl.filesink_location = pa_xstrdup(pa_modargs_get_value(ma, "filesink_location", NULL));
        if (!u->filter_hdl.filesink_location) {
            pa_log("File sink location get failed");
            u->filter_hdl.filesink_location = pa_xstrdup("/data/filesink.wav");
        }
        pa_log("File sink location %s", u->filter_hdl.filesink_location);
    }

    u->input_channels = ss.channels;
    u->input_format = ss.format;
    u->input_rate = ss.rate;
    u->output_channels = ss_output.channels;
    u->output_format = ss_output.format;
    u->output_rate = ss_output.rate;

    u->filter_hdl.out_channels = ss_output.channels;

    /* Create sink */
    pa_sink_new_data_init(&sink_data);

    sink_data.driver = __FILE__;
    sink_data.module = m;

    if (!(sink_data.name = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL))))
        sink_data.name = pa_sprintf_malloc("%s.filtersink", master->name);

    pa_sink_new_data_set_sample_spec(&sink_data, &ss);
    pa_sink_new_data_set_channel_map(&sink_data, &map);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");
    pa_proplist_sets(sink_data.proplist, "device.filtersink.name", sink_data.name);

    if (pa_modargs_get_proplist(ma, "sink_properties", sink_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }

    sink_data.avoid_processing = true;
    u->sink = pa_sink_new(m->core, &sink_data, (master->flags & (PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY)) | 0);
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg_cb;
    u->sink->set_state_in_main_thread = sink_set_state_in_main_thread_cb;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->request_rewind = sink_request_rewind_cb;
    u->sink->reconfigure = sink_reconfigure_cb;
    pa_sink_set_set_mute_callback(u->sink, sink_set_mute_cb);
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;

    pa_sink_input_new_data_set_sink(&sink_input_data, master, false, true);
    sink_input_data.origin_sink = u->sink;
    pa_proplist_setf(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "Filter Sink Stream from %s",
            pa_proplist_gets(u->sink->proplist, PA_PROP_DEVICE_DESCRIPTION));
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &ss_output);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map_output);
    sink_input_data.flags |= PA_SINK_INPUT_START_CORKED;

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!u->sink_input) {
        pa_log("Failed to create sink input");
        goto fail;
    }

    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->mute_changed = sink_input_mute_changed_cb;
    u->sink_input->userdata = u;
    u->sink->input_to_master = u->sink_input;

    pa_sink_input_get_silence(u->sink_input, &silence);
    u->memblockq = pa_memblockq_new("module-filter-sink memblockq", 0,
            MEMBLOCKQ_MAXLENGTH, 0, &ss_output, 1, 1, 0, &silence);
    pa_memblock_unref(silence.memblock);

    /* Custom initialization should be done here */
#if AUDIO_BUFFER_DUMP
    u->fd = -1;
    u->fd_out = -1;
    u->write_type = 1;
#endif //AUDIO_BUFFER_DUMP

    if (!(filter_plugins = pa_modargs_get_value(ma, "filter_plugins", NULL))) {
        pa_log("Plugin name not passed in command line while loading filter module");
        goto fail;
    }

    /* Parse plugins name from filter_plugins arguments*/
    if (filter_plugins) {
        const char *state = NULL;
        char *pname;
        while ((pname = pa_split(filter_plugins, ",", &state))) {
            pa_log("Filter plugin: %s", pname);
            gst_enqueue_filter_plugin(u, pname);
            pa_xfree(pname);
        }
    }

    /* Parse plugin module and assign callback functions */
    u->dl_handle = NULL;
    if (!(filter_plugin_module = pa_modargs_get_value(ma, "filter_plugin_module", NULL))) {
        pa_log("Plugin name not passed in command line while loading filter module");
        goto fail;
    }

    u->dl_handle = dlopen(filter_plugin_module, RTLD_NOW);
    if (!u->dl_handle) {
        pa_log("%s\n", dlerror());
        goto fail;
    }

    filter_plugin_callbacks = (void (*)(filter_plugin_cb *)) dlsym(u->dl_handle, "set_filter_plugin_callbacks");
    error = dlerror();
    if (error != NULL) {
        pa_log("%s\n", dlerror());
        goto fail;
    }

    filter_plugin_callbacks(&u->filter_plugins_callbacks);

    if (pthread_mutex_init(&u->mutex, NULL) != 0) {
        pa_log("Failed to initialize mutex");
        goto fail;
    }

    if (pthread_cond_init(&u->cond, NULL) != 0) {
        pa_log("Failed to initialize cond var");
        goto fail;
    }

    if (gstreamer_init(u) != 0) {
        pa_log("Failed to initialize gstreamer pipeline");
        goto fail;
    }

    u->pipeline_state = G_READY;

    /* The order here is important. The input must be put first,
     * otherwise streams might attach to the sink before the sink
     * input is attached to the master. */
    pa_sink_input_put(u->sink_input);
    pa_sink_put(u->sink);
    pa_sink_input_cork(u->sink_input, false);

    pa_modargs_free(ma);

    pa_log("Exit %s", __func__);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);
    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module*m) {
    struct userdata *u;
    pa_log("Enter %s", __func__);

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->filter_hdl.pipeline)
        gst_element_set_state(u->filter_hdl.pipeline, GST_STATE_NULL);

    if (u->thread) {
        pa_thread_free(u->thread);
        u->thread = NULL;
    }

    if (u->appsrc_caps)
        gst_caps_unref(u->appsrc_caps);

    pthread_mutex_destroy(&u->mutex);
    pthread_cond_destroy(&u->cond);

    gst_dequeue_all_filter_plugin(u);

    if (u->filter_hdl.filter_properties)
        pa_xfree(u->filter_hdl.filter_properties);

    if (u->filter_hdl.filesink_location)
        pa_xfree(u->filter_hdl.filesink_location);

    if (u->dl_handle)
        dlclose(u->dl_handle);

    /* Refer sink_input_kill_cb() for the destruction order */
    if (u->sink_input)
        pa_sink_input_cork(u->sink_input, true);

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->sink_input) {
        pa_sink_input_unlink(u->sink_input);
        pa_sink_input_unref(u->sink_input);
    }

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    pa_xfree(u);
    pa_log("Exit %s", __func__);
}
