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

#include <pulsecore/core-util.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/thread.h>
#include <pulsecore/shared.h>

#include "qsthw_api.h"
#include "qsthw_defs.h"
#include "qsthw-util.h"

#define OK 0
#define QSTHW_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qsthw"
#define QSTHW_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Qsthw"
#define QSTHW_DBUS_SESSION_IFACE "org.PulseAudio.Ext.Qsthw.Session"
#define PA_DBUS_QSTHW_MODULE_IFACE_VERSION 0x101

static const char* const valid_modargs[] = {
    "module",
    NULL,
};

enum {
    QSTHW_THREAD_IDLE,
    QSTHW_THREAD_READ_QUEUED,
    QSTHW_THREAD_EXIT,
    QSTHW_THREAD_STOP_BUFFERING
};

struct qsthw_module_data {
    pa_module *module;
    pa_modargs *modargs;

    char *module_name;
    char *obj_path;
    pa_dbus_protocol *dbus_protocol;
    const qsthw_module_handle_t *st_mod_handle;
    pa_qsthw_hooks *qsthw;
    bool is_session_started;
};

struct qsthw_session_data {
    struct qsthw_module_data *common;
    sound_model_handle_t ses_handle;
    char *obj_path;
    int thread_state;
    void *read_buf;
    unsigned int read_bytes;
    pa_thread *async_thread;
    pa_mutex *mutex;
    pa_cond *cond;
};

static int unload_sm(DBusConnection *conn, struct qsthw_session_data *ses_data);
static void get_properties(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void get_version(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void get_interface_version(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void global_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void load_sound_model(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void unload_sound_model(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void start_recognition(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void stop_recognition(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void get_buffer_size(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void request_read_buffer(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void read_buffer(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void stop_buffering(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void get_param_data(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum module_handler_index {
    MODULE_HANDLER_GET_PROPERTIES,
    MODULE_HANDLER_GET_VERSION,
    MODULE_HANDLER_SET_PARAMS,
    MODULE_HANDLER_LOAD_SOUND_MODEL,
    MODULE_HANDLER_GET_INTERFACE_VERSION,
    MODULE_HANDLER_MAX
};

enum session_handler_index {
    SESSION_HANDLER_UNLOAD_SOUND_MODEL,
    SESSION_HANDLER_START_RECOGNITION,
    SESSION_HANDLER_STOP_RECOGNITION,
    SESSION_HANDLER_SET_PARAMS,
    SESSION_HANDLER_GET_BUFFER_SIZE,
    SESSION_HANDLER_REQUEST_READ_BUFFER,
    SESSION_HANDLER_READ_BUFFER,
    SESSION_HANDLER_STOP_BUFFERING,
    SESSION_HANDLER_GET_PARAM_DATA,
    SESSION_HANDLER_MAX
};

pa_dbus_arg_info get_properties_args[] = {
    {"properties", "(ssu(uqqqay)uuuububbu)", "out"},
};

pa_dbus_arg_info get_version_args[] = {
    {"version", "i", "out"},
};

pa_dbus_arg_info get_interface_version_args[] = {
    {"version", "i", "out"},
};

pa_dbus_arg_info load_sound_model_args[] = {
    {"sound_model", "((i(uqqqay)(uqqqay))a(uuauss))","in"},
    {"opaque_data", "ay", "in"},
    {"object_path", "o","out"}
};

pa_dbus_arg_info unload_sound_model_args[] = {
};

pa_dbus_arg_info start_recognition_args[] = {
    {"recognition_config", "(iuba(uuua(uu)))","in"},
    {"opaque_data", "ay", "in"},
};

pa_dbus_arg_info stop_recognition_args[] = {
};

pa_dbus_arg_info set_parameters_args[] = {
    {"kv_pairs", "s", "in"},
};

pa_dbus_arg_info get_buffer_size_args[] = {
    {"buffer_size", "i", "out"},
};

pa_dbus_arg_info request_read_buffer_args[] = {
    {"bytes", "u", "in"},
};

pa_dbus_arg_info read_buffer_args[] = {
    {"bytes", "u", "in"},
    {"buf", "ay", "out"},
};

pa_dbus_arg_info stop_buffering_args[] = {
};

pa_dbus_arg_info get_param_data_args[] = {
    {"param", "s", "in"},
    {"payload", "ay", "out"},
};

/* recognition config event.
 * XXX Skipped unused offload_info in audio_config as it is
 * used for compressed offload playback streams.
 */
pa_dbus_arg_info detection_event_args[] = {
    {"recognition_event", "(iiibiiib(uuuu))a(uuua(uu))t", NULL},
    {"opaque_data", "ay", NULL}
};

pa_dbus_arg_info read_buffer_available_event_args[] = {
    {"read_buffer_sequence", "u", NULL},
    {"read_status", "i", NULL},
    {"read_buffer", "ay", NULL}
};

pa_dbus_arg_info stop_buffering_done_event_args[] = {
    {"status", "i", NULL},
};

static pa_dbus_method_handler qsthw_module_handlers[MODULE_HANDLER_MAX] = {
    [MODULE_HANDLER_GET_PROPERTIES] = {
        .method_name = "GetProperties",
        .arguments = get_properties_args,
        .n_arguments = sizeof(get_properties_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = get_properties},
    [MODULE_HANDLER_GET_VERSION] = {
        .method_name = "GetVersion",
        .arguments = get_version_args,
        .n_arguments = sizeof(get_version_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = get_version},
    [MODULE_HANDLER_GET_INTERFACE_VERSION] = {
        .method_name = "GetInterfaceVersion",
        .arguments = get_interface_version_args,
        .n_arguments = sizeof(get_interface_version_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = get_interface_version},
    [MODULE_HANDLER_SET_PARAMS] = {
        .method_name = "SetParameters",
        .arguments = set_parameters_args,
        .n_arguments = sizeof(set_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = global_set_parameters},
    [MODULE_HANDLER_LOAD_SOUND_MODEL] = {
        .method_name = "LoadSoundModel",
        .arguments = load_sound_model_args,
        .n_arguments = sizeof(load_sound_model_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = load_sound_model},
};

static pa_dbus_method_handler qsthw_session_handlers[SESSION_HANDLER_MAX] = {
    [SESSION_HANDLER_UNLOAD_SOUND_MODEL] = {
        .method_name = "UnloadSoundModel",
        .arguments = unload_sound_model_args,
        .n_arguments = sizeof(unload_sound_model_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = unload_sound_model},
    [SESSION_HANDLER_START_RECOGNITION] = {
        .method_name = "StartRecognition",
        .arguments = start_recognition_args,
        .n_arguments = sizeof(start_recognition_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = start_recognition},
    [SESSION_HANDLER_STOP_RECOGNITION] = {
        .method_name = "StopRecognition",
        .arguments = stop_recognition_args,
        .n_arguments = sizeof(stop_recognition_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = stop_recognition},
    [SESSION_HANDLER_SET_PARAMS] = {
        .method_name = "SetParameters",
        .arguments = set_parameters_args,
        .n_arguments = sizeof(set_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = set_parameters},
    [SESSION_HANDLER_GET_BUFFER_SIZE] = {
        .method_name = "GetBufferSize",
        .arguments = get_buffer_size_args,
        .n_arguments = sizeof(get_buffer_size_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = get_buffer_size},
    [SESSION_HANDLER_REQUEST_READ_BUFFER] = {
        .method_name = "RequestReadBuffer",
        .arguments = request_read_buffer_args,
        .n_arguments = sizeof(request_read_buffer_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = request_read_buffer},
    [SESSION_HANDLER_READ_BUFFER] = {
        .method_name = "ReadBuffer",
        .arguments = read_buffer_args,
        .n_arguments = sizeof(read_buffer_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = read_buffer},
    [SESSION_HANDLER_STOP_BUFFERING] = {
        .method_name = "StopBuffering",
        .arguments = stop_buffering_args,
        .n_arguments = sizeof(stop_buffering_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = stop_buffering},
    [SESSION_HANDLER_GET_PARAM_DATA] = {
        .method_name = "GetParamData",
        .arguments = get_param_data_args,
        .n_arguments = sizeof(get_param_data_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = get_param_data},
};

enum signal_index {
    SIGNAL_DETECTION_EVENT,
    SIGNAL_READ_BUFFER_AVAILABLE_EVENT,
    SIGNAL_STOP_BUFFERING_DONE_EVENT,
    SIGNAL_MAX
};

static pa_dbus_signal_info det_event_signals[SIGNAL_MAX] = {
    [SIGNAL_DETECTION_EVENT] = {
        .name = "DetectionEvent",
        .arguments = detection_event_args,
        .n_arguments = sizeof(detection_event_args)/sizeof(pa_dbus_arg_info)},
    [SIGNAL_READ_BUFFER_AVAILABLE_EVENT] = {
        .name = "ReadBufferAvailableEvent",
        .arguments = read_buffer_available_event_args,
        .n_arguments = sizeof(read_buffer_available_event_args)/sizeof(pa_dbus_arg_info)},
    [SIGNAL_STOP_BUFFERING_DONE_EVENT] = {
        .name = "StopBufferingDoneEvent",
        .arguments = stop_buffering_done_event_args,
        .n_arguments = sizeof(stop_buffering_done_event_args)/sizeof(pa_dbus_arg_info)},
};

static pa_dbus_interface_info module_interface_info = {
    .name = QSTHW_DBUS_MODULE_IFACE,
    .method_handlers = qsthw_module_handlers,
    .n_method_handlers = MODULE_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

static pa_dbus_interface_info session_interface_info = {
    .name = QSTHW_DBUS_SESSION_IFACE,
    .method_handlers = qsthw_session_handlers,
    .n_method_handlers = SESSION_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = det_event_signals,
    .n_signals = SIGNAL_MAX
};

static void signal_read_buffer_available(struct qsthw_session_data *ses_data,
                                         unsigned int read_buffer_sequence, int status) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, array_i;

    pa_log_info("[%d] Posting read buffer available, seq %u, status %d",
                ses_data->ses_handle, read_buffer_sequence, status);

    pa_assert_se(message = dbus_message_new_signal(ses_data->obj_path,
            session_interface_info.name,
            det_event_signals[SIGNAL_READ_BUFFER_AVAILABLE_EVENT].name));
    dbus_message_iter_init_append(message, &arg_i);

    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &read_buffer_sequence);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_INT32, &status);

    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE,
                                         &ses_data->read_buf, ses_data->read_bytes);
    dbus_message_iter_close_container(&arg_i, &array_i);

    pa_dbus_protocol_send_signal(ses_data->common->dbus_protocol, message);
    dbus_message_unref(message);
}

static void signal_stop_buffering_done(struct qsthw_session_data *ses_data,
                                       int status) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i;

    pa_log_info("[%d] Posting stop buffering done status %d",
                ses_data->ses_handle, status);

    pa_assert_se(message = dbus_message_new_signal(ses_data->obj_path,
            session_interface_info.name,
            det_event_signals[SIGNAL_STOP_BUFFERING_DONE_EVENT].name));
    dbus_message_iter_init_append(message, &arg_i);

    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_INT32, &status);

    pa_dbus_protocol_send_signal(ses_data->common->dbus_protocol, message);
    dbus_message_unref(message);
}

static void async_thread_func(void *userdata) {
    struct qsthw_session_data *ses_data = (struct qsthw_session_data *)userdata;
    sound_model_handle_t sm_handle = ses_data->ses_handle;
    unsigned int bytes = 0, read_buffer_sequence = 0;
    int ret = 0;

    pa_log_debug("[%d]Starting Async Thread", sm_handle);

    pa_mutex_lock(ses_data->mutex);
    while (ses_data->thread_state != QSTHW_THREAD_EXIT) {
        pa_log_debug("[%d]Async Thread wait", sm_handle);
        pa_cond_wait(ses_data->cond, ses_data->mutex);
        pa_log_debug("[%d]Async Thread wakeup", sm_handle);

        if (ses_data->thread_state == QSTHW_THREAD_STOP_BUFFERING) {
            pa_mutex_unlock(ses_data->mutex);
            pa_log_debug("[%d]Stop buffering", sm_handle);
            ret = qsthw_stop_buffering(ses_data->common->st_mod_handle, sm_handle);
            if (ret < 0 )
                pa_log_debug("[%d]Stop buffering failed with error %d", sm_handle, ret);

            pa_mutex_lock(ses_data->mutex);
            signal_stop_buffering_done(ses_data, ret);
        }

        if (ses_data->thread_state != QSTHW_THREAD_READ_QUEUED)
            continue;

        if (ses_data->read_buf == NULL || ses_data->read_bytes != bytes) {
            if (ses_data->read_buf)
                pa_xfree(ses_data->read_buf);
            ses_data->read_buf = pa_xmalloc0(ses_data->read_bytes);
            bytes = ses_data->read_bytes;
        }

        pa_mutex_unlock(ses_data->mutex);
        ret = qsthw_read_buffer(ses_data->common->st_mod_handle,
                                sm_handle, ses_data->read_buf, bytes);
        if (ret < 0)
            pa_log_debug("[%d]Read failed with error %d", sm_handle, ret);
        pa_mutex_lock(ses_data->mutex);

        if (ses_data->thread_state == QSTHW_THREAD_READ_QUEUED) {
            signal_read_buffer_available(ses_data, ++read_buffer_sequence, ret);
            ses_data->thread_state = QSTHW_THREAD_IDLE;
        } else if (ses_data->thread_state == QSTHW_THREAD_STOP_BUFFERING) {
            pa_mutex_unlock(ses_data->mutex);
            ret = qsthw_stop_buffering(ses_data->common->st_mod_handle, sm_handle);
            if (ret < 0 )
                pa_log_debug("[%d]Stop buffering failed with error %d", sm_handle, ret);

            pa_mutex_lock(ses_data->mutex);
            signal_stop_buffering_done(ses_data, ret);
        }

    }

    pa_xfree(ses_data->read_buf);
    ses_data->read_buf = NULL;
    pa_mutex_unlock(ses_data->mutex);

    pa_log_debug("[%d]Exiting Async Thread", sm_handle);
}

static void event_callback(struct sound_trigger_recognition_event *event, void *cookie) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, struct_i, struct_ii, array_i, array_ii;
    dbus_uint32_t i, j;

    struct qsthw_session_data *ses_data = (struct qsthw_session_data *)cookie;
    struct qsthw_phrase_recognition_event *qsthw_event;
    struct sound_trigger_phrase_recognition_event *phrase_event;
    int n_elements = 0;
    char *value = NULL;

    pa_log_info("[%d] Callback event received: %d", event->model, event->status);
    ses_data->thread_state = QSTHW_THREAD_IDLE;
    qsthw_event = (struct qsthw_phrase_recognition_event *)event;
    phrase_event = &qsthw_event->phrase_event;

    pa_assert_se(message = dbus_message_new_signal(ses_data->obj_path,
            session_interface_info.name,
            det_event_signals[SIGNAL_DETECTION_EVENT].name));
    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->status);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->type);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->model);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_BOOLEAN, &event->capture_available);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->capture_session);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->capture_delay_ms);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->capture_preamble_ms);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_BOOLEAN, &event->trigger_in_data);

    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_STRUCT, NULL, &struct_ii);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &event->audio_config.sample_rate);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &event->audio_config.channel_mask);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &event->audio_config.format);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &event->audio_config.frame_count);
    dbus_message_iter_close_container(&struct_i, &struct_ii);
    dbus_message_iter_close_container(&arg_i, &struct_i);

    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "(uuua(uu))", &array_i);
    for (i = 0; i < phrase_event->num_phrases; i++) {
        dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
            &phrase_event->phrase_extras[i].id);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
            &phrase_event->phrase_extras[i].recognition_modes);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
            &phrase_event->phrase_extras[i].confidence_level);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "(uu)", &array_ii);
        for (j = 0; j < phrase_event->phrase_extras[i].num_levels; j++) {
            dbus_message_iter_open_container(&array_ii, DBUS_TYPE_STRUCT, NULL, &struct_ii);
            dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32,
                &phrase_event->phrase_extras[i].levels[j].user_id);
            dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32,
                &phrase_event->phrase_extras[i].levels[j].level);
            dbus_message_iter_close_container(&array_ii, &struct_ii);
        }
        dbus_message_iter_close_container(&struct_i, &array_ii);
        dbus_message_iter_close_container(&array_i, &struct_i);
    }
    dbus_message_iter_close_container(&arg_i, &array_i);

    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT64, &qsthw_event->timestamp);

    n_elements = event->data_size;
    value = (char*)qsthw_event + event->data_offset;
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &value, n_elements);
    dbus_message_iter_close_container(&arg_i, &array_i);

    pa_dbus_protocol_send_signal(ses_data->common->dbus_protocol, message);

    if(event->capture_available) {
        ses_data->common->is_session_started = true;
        pa_hook_fire(&ses_data->common->qsthw->hooks[PA_HOOK_QSTHW_START_DETECTION],NULL);
    }

    dbus_message_unref(message);
}

static DBusHandlerResult disconnection_filter_cb(DBusConnection *conn,
                              DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Local", "Disconnected")) {
        /* connection died, unload the session for which callback got triggered */
        pa_log_info("connection died for session %d", ses_data->ses_handle);
        unload_sm(conn, ses_data);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int unload_sm(DBusConnection *conn, struct qsthw_session_data *ses_data) {
    sound_model_handle_t sm_handle = ses_data->ses_handle;
    int status = 0;

    dbus_connection_remove_filter(conn, disconnection_filter_cb, ses_data);
    status = qsthw_unload_sound_model(ses_data->common->st_mod_handle, sm_handle);

    pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol,
            ses_data->obj_path, session_interface_info.name) >= 0);

    pa_xfree(ses_data->obj_path);
    pa_xfree(ses_data);

    return status;
}

/* implementations exposed by session specific object path */
static void set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;
    int status = 0;
    DBusError error;
    sound_model_handle_t sm_handle;
    const char *kv_pairs;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("set parameters");
    sm_handle = ses_data->ses_handle;

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                               &kv_pairs, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    status = qsthw_set_parameters(ses_data->common->st_mod_handle, sm_handle,
                                  kv_pairs);
    if (OK != status) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_parameters failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void get_buffer_size(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;
    int buffer_size;
    DBusError error;
    sound_model_handle_t sm_handle;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("get buffer size");
    sm_handle = ses_data->ses_handle;
    buffer_size = qsthw_get_buffer_size(ses_data->common->st_mod_handle, sm_handle);

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_INT32, &buffer_size);
}

static void request_read_buffer(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;
    unsigned int bytes;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32,
                               &bytes, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    pa_mutex_lock(ses_data->mutex);
    if (bytes == 0 || ses_data->async_thread == NULL ||
        ses_data->thread_state != QSTHW_THREAD_IDLE) {
        pa_mutex_unlock(ses_data->mutex);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "request_read_buffer failed");
        dbus_error_free(&error);
        return;
    }

    ses_data->thread_state = QSTHW_THREAD_READ_QUEUED;
    ses_data->read_bytes = bytes;
    pa_cond_signal(ses_data->cond, 0);
    pa_mutex_unlock(ses_data->mutex);

    pa_dbus_send_empty_reply(conn, msg);
}

static void read_buffer(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;
    int ret = 0;
    DBusError error;
    sound_model_handle_t sm_handle;
    unsigned int bytes;
    void *buf = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32,
                               &bytes, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    sm_handle = ses_data->ses_handle;
    buf = pa_xmalloc0(bytes);
    ret = qsthw_read_buffer(ses_data->common->st_mod_handle, sm_handle,
                            buf, bytes);
    if (ret < 0) {
        pa_xfree(buf);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "read_buffer failed");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &buf, bytes);
    dbus_message_iter_close_container(&arg_i, &array_i);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    pa_xfree(buf);
    dbus_message_unref(reply);
}

static void stop_buffering(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("stop buffering");
    pa_mutex_lock(ses_data->mutex);

    if (ses_data->async_thread == NULL) {
        pa_mutex_unlock(ses_data->mutex);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "stop_buffering failed");
        dbus_error_free(&error);
        return;
    }

    ses_data->thread_state = QSTHW_THREAD_STOP_BUFFERING;
    pa_cond_signal(ses_data->cond, 0);
    pa_mutex_unlock(ses_data->mutex);

    pa_dbus_send_empty_reply(conn, msg);
}

static void get_param_data(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;
    int status = 0;
    DBusError error;
    sound_model_handle_t sm_handle;
    const char *param;
    void *payload;
    unsigned int payload_size = sizeof(qsthw_get_param_payload_t);
    size_t param_data_size = 0;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &param,
                               DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("get param data");
    sm_handle = ses_data->ses_handle;
    payload = pa_xmalloc0(payload_size);
    status = qsthw_get_param_data(ses_data->common->st_mod_handle, sm_handle,
                                  param, payload, payload_size, &param_data_size);
    if (OK != status) {
        pa_xfree(payload);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param_data failed");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &payload,
                                         param_data_size);
    dbus_message_iter_close_container(&arg_i, &array_i);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    pa_xfree(payload);
    dbus_message_unref(reply);
}

static void stop_recognition(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;
    int status = 0;
    DBusError error;
    sound_model_handle_t sm_handle;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("stop recognition");
    sm_handle = ses_data->ses_handle;
    status = qsthw_stop_recognition(ses_data->common->st_mod_handle, sm_handle);
    if (OK != status) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "stop_recognition failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void start_recognition(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = (struct qsthw_session_data *)userdata;
    struct sound_trigger_recognition_config config = {0, };
    struct sound_trigger_recognition_config *rc_config = NULL;
    dbus_int32_t rc_config_size;
    DBusError error;
    DBusMessageIter arg_i, struct_i, struct_ii, struct_iii, array_i, sub_array_i;
    dbus_int32_t i, j, status = 0;
    sound_model_handle_t sm_handle;
    int n_elements = 0, arg_type;
    char *value = NULL;
    char **addr_value = &value;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "start_recognition has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(iuba(uuua(uu)))ay")) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "Invalid signature for start_recognition");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("start recognition");
    /* fire a hook to stop detection */
    if (ses_data->common->is_session_started) {
        pa_hook_fire(&ses_data->common->qsthw->hooks[PA_HOOK_QSTHW_STOP_DETECTION],NULL);
        ses_data->common->is_session_started = false;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &config.capture_handle);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &config.capture_device);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &config.capture_requested);
    dbus_message_iter_next(&struct_i);

    dbus_message_iter_recurse(&struct_i, &array_i);
    i = 0;
    while (((arg_type = dbus_message_iter_get_arg_type(&array_i)) !=
                        DBUS_TYPE_INVALID) &&
            (config.num_phrases < SOUND_TRIGGER_MAX_PHRASES)) {
        dbus_message_iter_recurse(&array_i, &struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &config.phrases[i].id);
        dbus_message_iter_next(&struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &config.phrases[i].recognition_modes);
        dbus_message_iter_next(&struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &config.phrases[i].confidence_level);
        dbus_message_iter_next(&struct_ii);

        dbus_message_iter_recurse(&struct_ii, &sub_array_i);
        j = 0;
        while (((arg_type = dbus_message_iter_get_arg_type(&sub_array_i)) !=
                            DBUS_TYPE_INVALID) &&
                (config.phrases[i].num_levels < SOUND_TRIGGER_MAX_USERS)) {
            dbus_message_iter_recurse(&sub_array_i, &struct_iii);
            dbus_message_iter_get_basic(&struct_iii,
                 &config.phrases[i].levels[j].user_id);
            dbus_message_iter_next(&struct_iii);
            dbus_message_iter_get_basic(&struct_iii,
                 &config.phrases[i].levels[j].level);
            config.phrases[i].num_levels++;
            j++;
            dbus_message_iter_next(&sub_array_i);
        }
        config.num_phrases++;
        i++;
        dbus_message_iter_next(&array_i);
    }
    /* read data size and data offset */
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    config.data_size = n_elements;
    config.data_offset = sizeof(config);

    rc_config_size = sizeof(struct sound_trigger_recognition_config) + config.data_size;
    rc_config = (struct sound_trigger_recognition_config *)pa_xmalloc0(rc_config_size);
    memcpy(rc_config, &config, sizeof(struct sound_trigger_recognition_config));
    memcpy((char *)rc_config + rc_config->data_offset,
           value, n_elements);

    sm_handle = ses_data->ses_handle;
    status = qsthw_start_recognition(ses_data->common->st_mod_handle, sm_handle,
                                    rc_config, event_callback, (void *)ses_data);
    pa_xfree(rc_config);

    if (OK != status) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "start_recognition failed");
        dbus_error_free(&error);
        return;
    }
    pa_dbus_send_empty_reply(conn, msg);
}

static void unload_sound_model(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_session_data *ses_data = userdata;
    DBusError error;
    int status = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    ses_data->thread_state = QSTHW_THREAD_EXIT;
    pa_cond_signal(ses_data->cond, 0);
    pa_thread_free(ses_data->async_thread);
    pa_cond_free(ses_data->cond);
    pa_mutex_free(ses_data->mutex);

    status = unload_sm(conn, ses_data);
    if (OK != status) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "unload_sound_model failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

/* implementations exposed by module global object path */
static void load_sound_model(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_module_data *m_data = userdata;
    struct qsthw_session_data *ses_data = NULL;
    struct sound_trigger_sound_model sound_model;
    struct sound_trigger_phrase_sound_model phrase_sound_model = {0, };
    struct sound_trigger_phrase_sound_model *p_sound_model = NULL;
    struct sound_trigger_sound_model *common_sound_model = NULL;
    DBusError error;
    DBusMessageIter arg_i, struct_i, struct_ii, struct_iii, array_i, sub_array_i;
    dbus_int32_t sm_type, sm_data_size, i, j, status = 0;
    sound_model_handle_t sm_handle;
    int n_elements = 0, arg_type;
    char *value = NULL, *thread_name = NULL;
    char **addr_value = &value;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "load_sound_model has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                 "((i(uqqqay)(uqqqay))a(uuauss))ay")) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "Invalid signature for load_sound_model");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("load sound model");
    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_recurse(&struct_i, &struct_ii);
    dbus_message_iter_get_basic(&struct_ii, &sm_type);

    sound_model.type = sm_type;
    /* read sound_model uuid values */
    dbus_message_iter_next(&struct_ii);
    dbus_message_iter_recurse(&struct_ii, &struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &sound_model.uuid.timeLow);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &sound_model.uuid.timeMid);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &sound_model.uuid.timeHiAndVersion);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &sound_model.uuid.clockSeq);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_recurse(&struct_iii, &array_i);

    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    memcpy(&sound_model.uuid.node[0], value, n_elements);

    /* read sound_model vendor_uuid values */
    dbus_message_iter_next(&struct_ii);
    dbus_message_iter_recurse(&struct_ii, &struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &sound_model.vendor_uuid.timeLow);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &sound_model.vendor_uuid.timeMid);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &sound_model.vendor_uuid.timeHiAndVersion);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &sound_model.vendor_uuid.clockSeq);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_recurse(&struct_iii, &array_i);

    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    memcpy(&sound_model.vendor_uuid.node[0], value, n_elements);

    if (sm_type == SOUND_MODEL_TYPE_KEYPHRASE) {
        memcpy(&phrase_sound_model.common, &sound_model,
               sizeof(struct sound_trigger_sound_model));
        phrase_sound_model.common.data_offset = sizeof(phrase_sound_model);

        /* read phrases fields */
        dbus_message_iter_next(&struct_i);
        dbus_message_iter_recurse(&struct_i, &array_i);
        i = 0;
        while (((arg_type = dbus_message_iter_get_arg_type(&array_i)) !=
                            DBUS_TYPE_INVALID) &&
                (phrase_sound_model.num_phrases < SOUND_TRIGGER_MAX_PHRASES)) {
            dbus_message_iter_recurse(&array_i, &struct_ii);
            dbus_message_iter_get_basic(&struct_ii,
                &phrase_sound_model.phrases[i].id);
            dbus_message_iter_next(&struct_ii);
            dbus_message_iter_get_basic(&struct_ii,
                &phrase_sound_model.phrases[i].recognition_mode);
            dbus_message_iter_next(&struct_ii);

            dbus_message_iter_recurse(&struct_ii, &sub_array_i);
            j = 0;
            while (((arg_type = dbus_message_iter_get_arg_type(&sub_array_i)) !=
                                DBUS_TYPE_INVALID) &&
                    (phrase_sound_model.phrases[i].num_users < SOUND_TRIGGER_MAX_USERS)) {
                dbus_message_iter_get_basic(&sub_array_i,
                      &phrase_sound_model.phrases[i].users[j]);
                phrase_sound_model.phrases[i].num_users++;
                j++;
                dbus_message_iter_next(&sub_array_i);
            }
            dbus_message_iter_next(&struct_ii);
            dbus_message_iter_get_basic(&struct_ii,
                &phrase_sound_model.phrases[i].locale);
            dbus_message_iter_next(&struct_ii);
            dbus_message_iter_get_basic(&struct_ii,
                &phrase_sound_model.phrases[i].text);
            phrase_sound_model.num_phrases++;
            i++;
            dbus_message_iter_next(&array_i);
        }

        /* read opaque data into sound model structure */
        dbus_message_iter_next(&arg_i);
        dbus_message_iter_recurse(&arg_i, &array_i);
        dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
        phrase_sound_model.common.data_size = n_elements;
        sm_data_size = sizeof(phrase_sound_model) +
                       phrase_sound_model.common.data_size;
        p_sound_model = (struct sound_trigger_phrase_sound_model *)pa_xmalloc0(sm_data_size);

        memcpy(p_sound_model, &phrase_sound_model, sizeof(phrase_sound_model));
        memcpy((char*)p_sound_model + p_sound_model->common.data_offset,
               value, n_elements);
        common_sound_model = &p_sound_model->common;
    } else if (sm_type == SOUND_MODEL_TYPE_GENERIC) {
        sound_model.data_offset = sizeof(sound_model);
        /*skip phrase related fields */
        dbus_message_iter_next(&arg_i);
        dbus_message_iter_recurse(&arg_i, &array_i);
        dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
        sound_model.data_size = n_elements;
        sm_data_size = sizeof(sound_model) + sound_model.data_size;

        common_sound_model = (struct sound_trigger_sound_model *)pa_xmalloc0(sm_data_size);
        memcpy(common_sound_model, &sound_model, sizeof(sound_model));
        memcpy((char*)common_sound_model + common_sound_model->data_offset,
               value, n_elements);
    }

    status = qsthw_load_sound_model(m_data->st_mod_handle, common_sound_model,
                    NULL, NULL, &sm_handle);

    pa_xfree(common_sound_model);
    if (OK != status) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "load_sound_model failed");
        dbus_error_free(&error);
        return;
    }

   /* After successful load sound model, allocate session data */
    ses_data = pa_xnew0(struct qsthw_session_data, 1);
    ses_data->common = (struct qsthw_module_data *)userdata;
    ses_data->ses_handle = sm_handle;
    ses_data->obj_path = pa_sprintf_malloc("%s/ses_%d", m_data->obj_path, sm_handle);

    ses_data->mutex = pa_mutex_new(false /* recursive  */, false /* inherit_priority */);
    ses_data->cond = pa_cond_new();
    ses_data->thread_state = QSTHW_THREAD_IDLE;
    ses_data->read_buf = NULL;

    thread_name = pa_sprintf_malloc("qsthw async thread%d", sm_handle);
    if (!(ses_data->async_thread = pa_thread_new(thread_name, async_thread_func, ses_data)))
        pa_log_error("%s: qsthw async thread creation failed", __func__);
    pa_xfree(thread_name);

    pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol,
            ses_data->obj_path, &session_interface_info, ses_data) >= 0);

    pa_assert_se(dbus_connection_add_filter(conn, disconnection_filter_cb, ses_data, NULL));

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, &ses_data->obj_path);
}

static void get_properties(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_module_data *m_data = userdata;
    int status;
    struct sound_trigger_properties properties;
    DBusError error;
    DBusMessageIter arg_i, struct_i, struct_ii, array_i;
    DBusMessage *reply = NULL;
    dbus_int32_t i;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    pa_log_debug("get_properties");
    status = qsthw_get_properties(m_data->st_mod_handle, &properties);
    if (OK != status) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_properties failed");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_STRING, &properties.implementor);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_STRING, &properties.description);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &properties.version);
    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_STRUCT, NULL, &struct_ii);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &properties.uuid.timeLow);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT16, &properties.uuid.timeMid);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT16, &properties.uuid.timeHiAndVersion);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT16, &properties.uuid.clockSeq);

    dbus_message_iter_open_container(&struct_ii, DBUS_TYPE_ARRAY, "y", &array_i);
    for (i = 0; i < 6; i++) {
         dbus_message_iter_append_basic(&array_i, DBUS_TYPE_BYTE, &properties.uuid.node[i]);
    }
    dbus_message_iter_close_container(&struct_ii, &array_i);
    dbus_message_iter_close_container(&struct_i, &struct_ii);

    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &properties.max_sound_models);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &properties.max_key_phrases);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &properties.max_users);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &properties.recognition_modes);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_BOOLEAN, &properties.capture_transition);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &properties.max_buffer_ms);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_BOOLEAN, &properties.concurrent_capture);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_BOOLEAN, &properties.trigger_in_event);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &properties.power_consumption_mw);
    dbus_message_iter_close_container(&arg_i, &struct_i);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

static void get_version(DBusConnection *conn, DBusMessage *msg, void *userdata){
    int version;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("get_version");
    version = qsthw_get_version();
    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_INT32, &version);
}

static void get_interface_version(DBusConnection *conn, DBusMessage *msg, void *userdata){
    int version;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("get_interface_version(%d)", PA_DBUS_QSTHW_MODULE_IFACE_VERSION);
    version = PA_DBUS_QSTHW_MODULE_IFACE_VERSION;
    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_INT32, &version);
}

static void global_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qsthw_module_data *m_data = userdata;
    int status = 0;
    DBusError error;
    sound_model_handle_t sm_handle = 0;
    const char *kv_pairs;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("global set parameters");

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                               &kv_pairs, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }
    status = qsthw_set_parameters(m_data->st_mod_handle, sm_handle,
                                  kv_pairs);
    if (OK != status) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_parameters failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

int pa__init(pa_module *m) {
    struct qsthw_module_data *m_data;
    pa_modargs *ma;
    int i;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        ma = NULL;
        goto error;
    }

    m->userdata = m_data = pa_xnew0(struct qsthw_module_data, 1);
    m_data->qsthw = pa_xnew0(pa_qsthw_hooks, 1);
    m_data->modargs = ma;
    m_data->module = m;

    m_data->module_name = pa_xstrdup(pa_modargs_get_value(ma, "module", QSTHW_MODULE_ID_PRIMARY));

    if (pa_streq(m_data->module_name, QSTHW_MODULE_ID_PRIMARY)) {
        pa_log_debug("Loading qsthw module %s", m_data->module_name);
        m_data->obj_path = pa_sprintf_malloc("%s/%s", QSTHW_DBUS_OBJECT_PATH_PREFIX,
                             "primary");
    } else {
        pa_log_error("Unsupported module %s", m_data->module_name);
        goto error;
    }

    m_data->st_mod_handle = qsthw_load_module(m_data->module_name);
    if (m_data->st_mod_handle == NULL) {
        pa_log_error("module load failed");
        goto error;
    }

    m_data->dbus_protocol = pa_dbus_protocol_get(m->core);
    pa_assert_se(pa_dbus_protocol_add_interface(m_data->dbus_protocol,
            m_data->obj_path, &module_interface_info, m_data) >= 0);

    for (i = 0; i < PA_HOOK_QSTHW_MAX; i++)
        pa_hook_init(&m_data->qsthw->hooks[i], NULL);

    pa_shared_set(m->core, "voice-ui-session", m_data->qsthw);

    return 0;

error:
    pa__done(m);
    return -1;
}

void pa__done(pa_module *m) {
    struct qsthw_module_data *m_data;
    int status;

    pa_assert(m);

    if (!(m_data = m->userdata))
        return;

    if (m_data->obj_path && m_data->dbus_protocol)
        pa_assert_se(pa_dbus_protocol_remove_interface(m_data->dbus_protocol,
                m_data->obj_path, module_interface_info.name) >= 0);

    if (m_data->dbus_protocol)
        pa_dbus_protocol_unref(m_data->dbus_protocol);

    if (m_data->obj_path)
        pa_xfree(m_data->obj_path);

    if (m_data->st_mod_handle) {
        status = qsthw_unload_module(m_data->st_mod_handle);
        if (OK != status)
           pa_log_error("qsthw_unload_module failed, status %d", status);
    }

    if (m_data->module_name)
        pa_xfree(m_data->module_name);

    if (m_data->modargs)
        pa_modargs_free(m_data->modargs);

    pa_shared_remove(m->core, "voice-ui-session");

    pa_xfree(m_data->qsthw);
    pa_xfree(m_data);
    m->userdata = NULL;
}
