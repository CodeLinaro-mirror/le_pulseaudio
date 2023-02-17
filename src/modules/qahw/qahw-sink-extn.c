/*
 * Copyright (c) 2018, 2020, The Linux Foundation. All rights reserved.
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

#include <stdio.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-sink-extn.h"

#define QAHW_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qahw/sink"
#define QAHW_DBUS_SINK_IFACE "org.PulseAudio.Ext.Qahw.Sink"

#ifndef memscpy
#define memscpy(dst, dst_size, src, bytes_to_copy) \
        (void) memcpy(dst, src, MIN(dst_size, bytes_to_copy))
#endif

struct pa_qahw_sink_extn_data {
    char *obj_path;
    pa_dbus_protocol *dbus_protocol;
    qahw_stream_handle_t *out_handle;

    pa_queue *set_param_data_requests;
    pa_queue *set_param_requests;
};

struct pa_qahw_sink_extn_set_data {
    int32_t id;
    qahw_param_payload payload;
};

/* key,value  based set params */
static void pa_qahw_sink_get_clock_drift(DBusConnection *conn, DBusMessage *msg, void *userdata);

static void pa_qahw_sink_set_render_window(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_set_start_delay(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_set_drift_correction_flag(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_set_drift_correction_param(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_set_matrix_param(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_set_ch_status_info(DBusConnection *conn, DBusMessage *msg, void *userdata);

/* sink set params based on key,value */
static void pa_qahw_sink_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);

/* signal handler to raise signal to client */
static void pa_qahw_sink_extn_raise_signal(struct pa_qahw_sink_extn_data *qahw_extn_sdata,
                                                                            char *err_str);

enum sink_method_handler_index {
    METHOD_HANDLER_SINK_GET_CLOCK_DRIFT = 0,
    METHOD_HANDLER_SINK_SET_RENDER_WINDOW,
    METHOD_HANDLER_SINK_SET_START_DELAY,
    METHOD_HANDLER_SINK_SET_DRIFT_CORRECTION_FLAG,
    METHOD_HANDLER_SINK_SET_DRIFT_CORRECTION_PARAM,
    METHOD_HANDLER_SINK_SET_MATRIX_PARAMS,
    METHOD_HANDLER_SINK_SET_CHANNEL_STATUS_INFO,
    METHOD_HANDLER_SINK_SET_PARAMETERS,
    METHOD_HANDLER_SINK_GET_PARAMETERS,
    METHOD_HANDLER_SINK_LAST = METHOD_HANDLER_SINK_GET_PARAMETERS,
    METHOD_HANDLER_SINK_MAX = METHOD_HANDLER_SINK_LAST + 1,
};

enum signal_index {
    SIGNAL_FAILURE_EVENT,
    SIGNAL_MAX
};

static pa_dbus_arg_info sink_clock_drift_args[] = {
    {"drift", "(uit)", "out"},
};

static pa_dbus_arg_info sink_render_window_args[] = {
    {"render_window", "(tt)", "in"},
};

static pa_dbus_arg_info sink_start_delay_args[] = {
    {"start_delay", "(t)", "in"},
};

static pa_dbus_arg_info sink_drift_correction_flag_args[] = {
    {"enable", "(b)", "in"},
};

static pa_dbus_arg_info sink_drift_correction_param_args[] = {
    {"adjust_time", "(x)", "in"},
};

static pa_dbus_arg_info sink_matrix_param_args[] = {
    {"matrix_params", "(yaqyaqya{ad}", "in"},
};

static pa_dbus_arg_info sink_ch_status_info_args[] = {
    {"channel_status_info", "ay", "in"},
};

static pa_dbus_arg_info sink_set_parameters_args[] = {
    {"kv_pairs", "s", "in"},
};

static pa_dbus_arg_info sink_get_parameters_args[] = {
    {"kv_pairs", "s", "in"},
    {"value", "s", "out"},
};

pa_dbus_arg_info failure_event_args[] = {
    {"error", "s", NULL},
};

static pa_dbus_method_handler sink_method_handlers[METHOD_HANDLER_SINK_MAX] = {
[METHOD_HANDLER_SINK_GET_CLOCK_DRIFT] = {
        .method_name = "GetClockDrift",
        .arguments = sink_clock_drift_args,
        .n_arguments = sizeof(sink_clock_drift_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_get_clock_drift},
[METHOD_HANDLER_SINK_SET_RENDER_WINDOW] = {
        .method_name = "SetRenderWindow",
        .arguments = sink_render_window_args,
        .n_arguments = sizeof(sink_render_window_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_set_render_window},
[METHOD_HANDLER_SINK_SET_START_DELAY] = {
        .method_name = "SetStartDelay",
        .arguments = sink_start_delay_args,
        .n_arguments = sizeof(sink_start_delay_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_set_start_delay},
[METHOD_HANDLER_SINK_SET_DRIFT_CORRECTION_FLAG] = {
        .method_name = "SetDriftCorrectionFlag",
        .arguments = sink_drift_correction_flag_args,
        .n_arguments = sizeof(sink_drift_correction_flag_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_set_drift_correction_flag},
[METHOD_HANDLER_SINK_SET_DRIFT_CORRECTION_PARAM] = {
        .method_name = "SetDriftCorrectionParam",
        .arguments = sink_drift_correction_param_args,
        .n_arguments = sizeof(sink_drift_correction_param_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_set_drift_correction_param},
[METHOD_HANDLER_SINK_SET_MATRIX_PARAMS] = {
        .method_name = "SetMatrixParam",
        .arguments = sink_matrix_param_args,
        .n_arguments = sizeof(sink_drift_correction_param_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_set_matrix_param},
[METHOD_HANDLER_SINK_SET_CHANNEL_STATUS_INFO] = {
        .method_name = "SetChStatusInfo",
        .arguments = sink_ch_status_info_args,
        .n_arguments = sizeof(sink_ch_status_info_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_set_ch_status_info},
[METHOD_HANDLER_SINK_SET_PARAMETERS] = {
        .method_name = "SetParameters",
        .arguments = sink_set_parameters_args,
        .n_arguments = sizeof(sink_set_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_set_parameters},
[METHOD_HANDLER_SINK_GET_PARAMETERS] = {
        .method_name = "GetParameters",
        .arguments = sink_get_parameters_args,
        .n_arguments = sizeof(sink_get_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_get_parameters},
};

static pa_dbus_signal_info failure_event_signal[SIGNAL_MAX] = {
   [SIGNAL_FAILURE_EVENT] = {
        .name = "FailureEvent",
        .arguments = failure_event_args,
        .n_arguments = sizeof(failure_event_args)/sizeof(pa_dbus_arg_info)},
};

static pa_dbus_interface_info sink_interface_info = {
    .name = QAHW_DBUS_SINK_IFACE,
    .method_handlers = sink_method_handlers,
    .n_method_handlers = METHOD_HANDLER_SINK_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = failure_event_signal,
    .n_signals = SIGNAL_MAX
};

static void pa_qahw_sink_extn_cache_set_param_data_request(
                                            struct pa_qahw_sink_extn_data *qahw_extn_sdata,
                                            qahw_param_payload payload, int32_t id) {
    struct pa_qahw_sink_extn_set_data *set_param_data_req = NULL;

    set_param_data_req = pa_xnew0(struct pa_qahw_sink_extn_set_data, 1);
    set_param_data_req->id = id;
    set_param_data_req->payload = payload;
    pa_queue_push(qahw_extn_sdata->set_param_data_requests, (void *)set_param_data_req);
}

static void pa_qahw_sink_get_clock_drift(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;
    qahw_param_payload payload;

    DBusMessage *reply = NULL;
    DBusMessageIter struct_i, arg_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if(!dbus_message_iter_init(msg, &arg_i)) {
        pa_log("effect_create has no arguments.\n");
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                sink_clock_drift_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for pa_qahw_sink_get_clock_drift");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("get_clock_drift\n");

    if (!qahw_extn_sdata->out_handle) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "No valid handle");
        dbus_error_free(&error);
        return;
    }

    rc = qahw_out_get_param_data(qahw_extn_sdata->out_handle, QAHW_PARAM_AVT_DEVICE_DRIFT, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param_data for QAHW_PARAM_AVT_DEVICE_DRIFT failed");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &payload.drift_params.resync_flag);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &payload.drift_params.avt_device_drift_value);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT64, &payload.drift_params.ref_timer_abs_ts);

    dbus_message_iter_close_container(&arg_i, &struct_i);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    dbus_message_unref(reply);
}

static void pa_qahw_sink_set_render_window(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;
    qahw_param_payload payload;

    DBusMessageIter struct_i, arg_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "pa_qahw_sink_set_render_window has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                sink_render_window_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for sink_render_window_args");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.render_window_params.render_ws);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.render_window_params.render_we);

    pa_log_debug("%s:: render_ws %" PRId64 "us, render_we %" PRId64 "us\n", __func__, payload.render_window_params.render_ws, payload.render_window_params.render_we);

    if (!qahw_extn_sdata->out_handle) {
        pa_qahw_sink_extn_cache_set_param_data_request(qahw_extn_sdata, payload, QAHW_PARAM_OUT_RENDER_WINDOW);
        pa_log_info("%s: set QAHW_PARAM_OUT_RENDER_WINDOW request cached", __func__);
        goto exit;
    }

    rc = qahw_out_set_param_data(qahw_extn_sdata->out_handle, QAHW_PARAM_OUT_RENDER_WINDOW, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data for QAHW_PARAM_OUT_RENDER_WINDOW failed");
        dbus_error_free(&error);
        return;
    }

exit:
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_set_start_delay(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;
    qahw_param_payload payload;

    DBusMessageIter struct_i, arg_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "pa_qahw_sink_set_render_window has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                sink_start_delay_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for sink_start_delay_args");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.start_delay.start_delay);

    pa_log_debug("%s:: start_delay %" PRId64 "us", __func__, payload.start_delay.start_delay);

    if (!qahw_extn_sdata->out_handle) {
        pa_qahw_sink_extn_cache_set_param_data_request(qahw_extn_sdata, payload, QAHW_PARAM_OUT_START_DELAY);
        pa_log_info("%s: set QAHW_PARAM_OUT_START_DELAY request cached", __func__);
        goto exit;
    }

    rc = qahw_out_set_param_data(qahw_extn_sdata->out_handle, QAHW_PARAM_OUT_START_DELAY, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data for QAHW_PARAM_OUT_START_DELAY failed");
        dbus_error_free(&error);
        return;
    }

exit:
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_set_drift_correction_flag(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;
    qahw_param_payload payload;

    DBusMessageIter struct_i, arg_i;
    DBusError error;
    dbus_bool_t drift_correction_flag;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "pa_qahw_sink_set_render_window has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                sink_drift_correction_flag_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for sink_start_delay_args");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &drift_correction_flag);
    payload.drift_enable_param.enable = (bool)drift_correction_flag;

    pa_log_debug("%s:: drift correction  %d", __func__, payload.drift_enable_param.enable);

    if (!qahw_extn_sdata->out_handle) {
        pa_qahw_sink_extn_cache_set_param_data_request(qahw_extn_sdata, payload, QAHW_PARAM_OUT_ENABLE_DRIFT_CORRECTION);
        pa_log_info("%s: set QAHW_PARAM_OUT_ENABLE_DRIFT_CORRECTION request cached", __func__);
        goto exit;
    }

    rc = qahw_out_set_param_data(qahw_extn_sdata->out_handle, QAHW_PARAM_OUT_ENABLE_DRIFT_CORRECTION, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data for QAHW_PARAM_OUT_ENABLE_DRIFT_CORRECTION failed");
        dbus_error_free(&error);
        return;
    }

exit:
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_set_drift_correction_param(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;
    qahw_param_payload payload;

    DBusMessageIter struct_i, arg_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "pa_qahw_sink_set_drift_correction_param has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                sink_drift_correction_param_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for pa_qahw_sink_set_drift_correction_param");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.drift_correction_param.adjust_time);

    pa_log_debug("%s:: adjust time %"PRId64 " ", __func__, payload.drift_correction_param.adjust_time);

    if (!qahw_extn_sdata->out_handle) {
        pa_qahw_sink_extn_cache_set_param_data_request(qahw_extn_sdata, payload, QAHW_PARAM_OUT_ENABLE_DRIFT_CORRECTION);
        pa_log_info("%s: set QAHW_PARAM_OUT_ENABLE_DRIFT_CORRECTION request cached", __func__);
        goto exit;
    }

    rc = qahw_out_set_param_data(qahw_extn_sdata->out_handle, QAHW_PARAM_OUT_ENABLE_DRIFT_CORRECTION, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data for QAHW_PARAM_OUT_ENABLE_DRIFT_CORRECTION failed");
        dbus_error_free(&error);
        return;
    }

exit:
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_set_matrix_param(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;
    qahw_param_payload payload;

    DBusMessageIter array_i, array_ii, arg_i, struct_i;
    DBusError error;
    int n_elements = 0;
    char *value = NULL;
    char **addr_value = &value;
    uint32_t count = 0;
    int arg_type;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "pa_qahw_sink_set_render_window has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                sink_matrix_param_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for sink_render_window_args");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.mix_matrix_params.has_output_channel_map);

    dbus_message_iter_next(&struct_i);

    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);

    if (n_elements > AUDIO_CHANNEL_COUNT_MAX) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED , "set_matrix_param failed, unsupported channel count %d", n_elements);
        dbus_error_free(&error);
    }

    memcpy(&payload.mix_matrix_params.output_channel_map, value, n_elements);
    payload.mix_matrix_params.num_output_channels = n_elements;
    dbus_message_iter_next(&struct_i);

    dbus_message_iter_get_basic(&struct_i, &payload.mix_matrix_params.has_input_channel_map);
    dbus_message_iter_next(&struct_i);

    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);

    if (n_elements > AUDIO_CHANNEL_COUNT_MAX) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED , "set_matrix_param failed, unsupported channel count %d", n_elements);
        dbus_error_free(&error);
        return;
    }

    memcpy(&payload.mix_matrix_params.input_channel_map, value, n_elements);
    payload.mix_matrix_params.num_input_channels = n_elements;
    dbus_message_iter_next(&struct_i);

    dbus_message_iter_get_basic(&struct_i, &payload.mix_matrix_params.has_mixer_coeffs);
    dbus_message_iter_next(&struct_i);

    dbus_message_iter_recurse(&struct_i, &array_i);

    /* read two dimentinal array */
    while ((arg_type = dbus_message_iter_get_arg_type(&array_i)) != DBUS_TYPE_INVALID) {
        if (count >= AUDIO_CHANNEL_COUNT_MAX) {
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED , "set_matrix_param failed, unsupported channel count %d for mixer coeffcients", count);
            dbus_error_free(&error);
            return;
        }

        dbus_message_iter_recurse(&array_i, &array_ii);
        dbus_message_iter_get_fixed_array(&array_ii, addr_value, &n_elements);
        if (n_elements > AUDIO_CHANNEL_COUNT_MAX) {
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED , "set_matrix_param failed, unsupported channel count %d for mixer coeffcients", n_elements);
            dbus_error_free(&error);
            return;
        }

        memcpy(&payload.mix_matrix_params.mixer_coeffs[count], value, n_elements);

        dbus_message_iter_next(&array_i);

        count++;
    }

    if (!qahw_extn_sdata->out_handle) {
        pa_qahw_sink_extn_cache_set_param_data_request(qahw_extn_sdata, payload, QAHW_PARAM_CH_MIX_MATRIX_PARAMS);
        pa_log_info("%s: set QAHW_PARAM_CH_MIX_MATRIX_PARAMS request cached", __func__);
        goto exit;
    }

    rc = qahw_out_set_param_data(qahw_extn_sdata->out_handle, QAHW_PARAM_CH_MIX_MATRIX_PARAMS, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data for QAHW_PARAM_CH_MIX_MATRIX_PARAMS failed");
        dbus_error_free(&error);
        return;
    }

exit:
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_set_ch_status_info(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;
    char *ch_status_info = NULL;
    char **addr_value = &ch_status_info;
    int32_t n_elements = 0;
    DBusMessageIter arg_i, array_i;
    qahw_param_payload payload;
    int rc = 0;

    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "SetParamData has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "ay")) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Invalid signature for SetParamData");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);

    memscpy(payload.ch_status_info.channel_status,
            sizeof(payload.ch_status_info.channel_status) / sizeof(payload.ch_status_info.channel_status[0]),
            ch_status_info, n_elements);

    if (!qahw_extn_sdata->out_handle) {
        pa_qahw_sink_extn_cache_set_param_data_request(qahw_extn_sdata, payload, QAHW_PARAM_CHANNEL_STATUS_INFO);
        pa_log_info("%s: set QAHW_PARAM_CHANNEL_STATUS_INFO request cached", __func__);
        goto exit;
    }

    rc = qahw_out_set_param_data(qahw_extn_sdata->out_handle, QAHW_PARAM_CHANNEL_STATUS_INFO, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "SetChStatusInfo failed");
        dbus_error_free(&error);
        return;
    }

exit:
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc;
    const char *kv_pairs;

    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;

    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s\n", __func__);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                &kv_pairs, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    if (!qahw_extn_sdata->out_handle) {
        pa_queue_push(qahw_extn_sdata->set_param_requests, (void *)kv_pairs);

        pa_log_info("%s: set %s request cached", __func__, kv_pairs);
        goto exit;
    }

    rc = qahw_out_set_parameters(qahw_extn_sdata->out_handle, kv_pairs);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_out_set_parameters failed");
        dbus_error_free(&error);
        return;
    }

exit:
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    const char *kv_pairs;
    char *param;

    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)userdata;

    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s\n", __func__);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                &kv_pairs, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    if (!qahw_extn_sdata->out_handle) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "No valid handle");
        dbus_error_free(&error);
        return;
    }

    param = qahw_out_get_parameters(qahw_extn_sdata->out_handle, kv_pairs);
    if (!param) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_out_get_parameters failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_STRING, &param);

    free(param);
}

int pa_qahw_sink_extn_sink_handle_update(pa_qahw_sink_extn_handle_t *handle, qahw_stream_handle_t *out_handle) {
    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)handle;

    pa_assert(handle);
    pa_assert(out_handle);

    qahw_extn_sdata->out_handle = out_handle;

    return 0;
}

int pa_qahw_sink_extn_create(pa_core *core, int pa_sink_index, pa_qahw_sink_extn_handle_t **handle) {

    struct pa_qahw_sink_extn_data *qahw_extn_sdata;

    pa_assert(core);

    qahw_extn_sdata = pa_xnew0(struct pa_qahw_sink_extn_data, 1);

    qahw_extn_sdata->obj_path = pa_sprintf_malloc("%s%d", QAHW_DBUS_OBJECT_PATH_PREFIX, pa_sink_index);
    qahw_extn_sdata->out_handle = NULL;
    qahw_extn_sdata->dbus_protocol = pa_dbus_protocol_get(core);
    qahw_extn_sdata->set_param_data_requests = pa_queue_new();
    qahw_extn_sdata->set_param_requests = pa_queue_new();

    pa_assert_se(pa_dbus_protocol_add_interface(qahw_extn_sdata->dbus_protocol, qahw_extn_sdata->obj_path, &sink_interface_info, qahw_extn_sdata) >= 0);

    *handle = (pa_qahw_sink_extn_handle_t *)qahw_extn_sdata;

    pa_log_info("pa_qahw_sink_extn created for sink %d", pa_sink_index);

    return 0;
}

int pa_qahw_sink_extn_free(pa_qahw_sink_extn_handle_t *handle) {
    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)handle;

    pa_assert(qahw_extn_sdata);

    pa_assert(qahw_extn_sdata->dbus_protocol);
    pa_assert(qahw_extn_sdata->obj_path);

    pa_assert_se(pa_dbus_protocol_remove_interface(qahw_extn_sdata->dbus_protocol, qahw_extn_sdata->obj_path, sink_interface_info.name) >= 0);

    pa_dbus_protocol_unref(qahw_extn_sdata->dbus_protocol);

    pa_queue_free(qahw_extn_sdata->set_param_data_requests, NULL);
    pa_queue_free(qahw_extn_sdata->set_param_requests, NULL);

    pa_xfree(qahw_extn_sdata->obj_path);
    pa_xfree(qahw_extn_sdata);

    return 0;
}

void pa_qahw_sink_extn_process_requests(pa_qahw_sink_extn_handle_t *handle) {
    struct pa_qahw_sink_extn_data *qahw_extn_sdata = (struct pa_qahw_sink_extn_data *)handle;
    struct pa_qahw_sink_extn_set_data *set_param_data_req = NULL;
    const char *kvpair = NULL;
    char *err_str = NULL;
    int rc = 0;

    pa_assert(qahw_extn_sdata);
    pa_assert(qahw_extn_sdata->out_handle);
    pa_assert(qahw_extn_sdata->set_param_data_requests);
    pa_assert(qahw_extn_sdata->set_param_requests);

    while (!pa_queue_isempty(qahw_extn_sdata->set_param_data_requests)) {
        set_param_data_req = (struct pa_qahw_sink_extn_set_data *)pa_queue_pop(qahw_extn_sdata->set_param_data_requests);

        rc = qahw_out_set_param_data(qahw_extn_sdata->out_handle, set_param_data_req->id,
                                                          &(set_param_data_req->payload));
        if (rc) {
            pa_log_error("%s: set_param_data for %d failed, error %d", __func__, set_param_data_req->id, rc);
            err_str = pa_sprintf_malloc("%s%d", "Failed to set param: ", set_param_data_req->id);
            pa_qahw_sink_extn_raise_signal(qahw_extn_sdata, err_str);
        }

        pa_xfree(set_param_data_req);
        pa_xfree(err_str);
        set_param_data_req = NULL;
        err_str = NULL;
    }

    while (!pa_queue_isempty(qahw_extn_sdata->set_param_requests)) {
        kvpair = (const char *)pa_queue_pop(qahw_extn_sdata->set_param_requests);

        rc = qahw_out_set_parameters(qahw_extn_sdata->out_handle, kvpair);
        if (rc) {
            pa_log_error("%s: set_parameters for %s failed, error %d", __func__, kvpair, rc);
            err_str = pa_sprintf_malloc("%s%s", "Failed to set ", kvpair);
            pa_qahw_sink_extn_raise_signal(qahw_extn_sdata, err_str);
        }

        pa_xfree(err_str);
        err_str = NULL;
    }
}

static void pa_qahw_sink_extn_raise_signal(struct pa_qahw_sink_extn_data *qahw_extn_sdata,
                                                                            char *err_str) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i;

    pa_assert(qahw_extn_sdata);
    pa_assert(err_str);

    /* Raising signal to client on the event of failure of cached requests */
    pa_assert_se(message = dbus_message_new_signal(qahw_extn_sdata->obj_path, sink_interface_info.name,
                                                      failure_event_signal[SIGNAL_FAILURE_EVENT].name));

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_STRING, &(err_str));

    pa_dbus_protocol_send_signal(qahw_extn_sdata->dbus_protocol, message);

    dbus_message_unref(message);
}
