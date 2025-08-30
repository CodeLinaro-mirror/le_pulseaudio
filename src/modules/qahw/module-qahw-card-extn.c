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

#include <stdio.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-card-extn.h"
#include "qahw-utils.h"

#define QAHW_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qahw"
#define QAHW_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Qahw.Module"

enum {
    QAHW_THREAD_IDLE,
    QAHW_THREAD_SET_PARAM_EVENT,
    QAHW_THREAD_EXIT
};

struct qahw_module_extn_data {
    char *obj_path;
    pa_dbus_protocol *dbus_protocol;
    qahw_module_handle_t *module_handle;
    pa_thread *async_thread;
    int thread_state;
    char *final_kvpairs;
    pa_mutex *mutex;
    pa_cond *cond;
    pa_card *card;
};

static struct qahw_module_extn_data *qahw_extn_mdata = NULL;

/* enum based set/get params */
static void qahw_module_set_port_config(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_module_set_aptx_dec_params(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_module_get_source_tracking_params(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_module_get_sound_focus_params(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_module_set_sound_focus_params(DBusConnection *conn, DBusMessage *msg, void *userdata);

/* key,value based set params*/
static void qahw_module_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_module_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_module_set_parameters_async(DBusConnection *conn, DBusMessage *msg, void *userdata);

static void qahw_set_channel_config(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum module_method_handler_index {
    METHOD_HANDLER_SET_PORT_CONFIG = 0,
    METHOD_HANDLER_SET_APTX_DEC_PARAMS,
    METHOD_HANDLER_GET_SOURCE_TRACKING,
    METHOD_HANDLER_GET_SOUND_FOCUS,
    METHOD_HANDLER_SET_SOUND_FOCUS,
    METHOD_HANDLER_SET_PARAMETERS,
    METHOD_HANDLER_SET_PARAMETERS_ASYNC,
    METHOD_HANDLER_GET_PARAMETERS,
    MODULE_HANDLER_SET_CHANNEL_CONFIG,
    METHOD_HANDLER_MODULE_LAST = MODULE_HANDLER_SET_CHANNEL_CONFIG,
    METHOD_HANDLER_MODULE_MAX = METHOD_HANDLER_MODULE_LAST + 1,
};

static pa_dbus_arg_info set_port_config_args[] = {
    {"port_config", "(uuusssayq)", "in"},
};

static pa_dbus_arg_info set_aptx_dec_args[] = {
    {"aptx_dec", "(uuu)", "in"},
};

static pa_dbus_arg_info get_source_tracking_args[] = {
    {"source_track", "(ayqaqay)", "out"},
};

static pa_dbus_arg_info get_sound_focus_args[] = {
    {"sound_focus", "(aqayq)", "out"},
};

static pa_dbus_arg_info set_sound_focus_args[] = {
    {"sound_focus", "(aqayq)", "in"},
};

static pa_dbus_arg_info set_parameters_args[] = {
    {"kv_pairs", "s", "in"},
};

static pa_dbus_arg_info get_parameters_args[] = {
    {"kv_pairs", "s", "in"},
    {"value", "s", "out"},
};

static pa_dbus_arg_info set_channel_config_args[] = {
    {"channel_config", "(suas)", "in"},
};

static pa_dbus_arg_info set_parameters_async_args[] = {
    {"kvpairs", "s", "in"},
};

static pa_dbus_arg_info set_param_async_event_args[] = {
    {"status", "i", NULL},
};

static pa_dbus_method_handler module_method_handlers[METHOD_HANDLER_MODULE_MAX] = {
[METHOD_HANDLER_SET_PORT_CONFIG] = {
        .method_name = "SetPortConfig",
        .arguments = set_port_config_args,
        .n_arguments = sizeof(set_port_config_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_set_port_config},
[METHOD_HANDLER_SET_APTX_DEC_PARAMS] = {
        .method_name = "SetAptxDecParam",
        .arguments = set_aptx_dec_args,
        .n_arguments = sizeof(set_aptx_dec_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_set_aptx_dec_params},
[METHOD_HANDLER_GET_SOURCE_TRACKING] = {
        .method_name = "GetSourceTracking",
        .arguments = get_source_tracking_args,
        .n_arguments = sizeof(get_source_tracking_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_get_source_tracking_params},
[METHOD_HANDLER_GET_SOUND_FOCUS] = {
        .method_name = "GetSoundFocus",
        .arguments = get_sound_focus_args,
        .n_arguments = sizeof(get_sound_focus_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_get_sound_focus_params},
[METHOD_HANDLER_SET_SOUND_FOCUS] = {
        .method_name = "SetSoundFocus",
        .arguments = set_sound_focus_args,
        .n_arguments = sizeof(set_sound_focus_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_set_sound_focus_params},
[METHOD_HANDLER_SET_PARAMETERS] = {
        .method_name = "SetParameters",
        .arguments = set_parameters_args,
        .n_arguments = sizeof(set_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_set_parameters},
[METHOD_HANDLER_SET_PARAMETERS_ASYNC] = {
        .method_name = "SetParametersAsync",
        .arguments = set_parameters_async_args,
        .n_arguments = sizeof(set_parameters_async_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_set_parameters_async},
[METHOD_HANDLER_GET_PARAMETERS] = {
        .method_name = "GetParameters",
        .arguments = get_parameters_args,
        .n_arguments = sizeof(get_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_get_parameters},
[MODULE_HANDLER_SET_CHANNEL_CONFIG] = {
        .method_name = "SetChannelConfig",
        .arguments = set_channel_config_args,
        .n_arguments = sizeof(set_channel_config_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_set_channel_config},
};

enum signal_index {
    SIGNAL_SET_PARAMS_ASYNC_DONE,
    SIGNAL_MAX
};

static pa_dbus_signal_info set_param_async_signals[SIGNAL_MAX] = {
    [SIGNAL_SET_PARAMS_ASYNC_DONE] = {
    .name = "SetParamsAsyncEventDone",
    .arguments = set_param_async_event_args,
    .n_arguments = sizeof(set_param_async_event_args)/sizeof(pa_dbus_arg_info)},
};

static pa_dbus_interface_info module_interface_info = {
    .name = QAHW_DBUS_MODULE_IFACE,
    .method_handlers = module_method_handlers,
    .n_method_handlers = METHOD_HANDLER_MODULE_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = set_param_async_signals,
    .n_signals = SIGNAL_MAX
};

static void signal_set_parameters_done(struct qahw_module_extn_data *mdata, int status) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i;

    pa_log_info("Posting set parameters done status %d", status);

    pa_assert_se(message = dbus_message_new_signal(mdata->obj_path,
            module_interface_info.name,
            set_param_async_signals[SIGNAL_SET_PARAMS_ASYNC_DONE].name));
    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_INT32, &status);
    pa_dbus_protocol_send_signal(mdata->dbus_protocol, message);
    dbus_message_unref(message);
}

static int update_module_kvpairs(struct qahw_module_extn_data *mdata) {
    char *token, *token1, *rest, *temp_kvpairs;
    bool found_bus = false;
    const char *bus_name, *prop_name;
    void *state = NULL;
    pa_device_port *port;
    audio_devices_t *device = NULL;

    temp_kvpairs = pa_xstrdup(mdata->final_kvpairs);
    rest = (char *)temp_kvpairs;

    while ((token = strtok_r(rest, ";", &rest))) {
        if (strstr(token,"bus") != NULL) {
            found_bus = true;
            break;
        }
    }

    if (found_bus) {
        rest = (char *)token;

        while ((token1 = strtok_r(rest, "=", &rest))) {
            if (pa_streq(token1, "bus")) {
                bus_name = strtok_r(rest, "=", &rest);
                break;
            }
        }

        PA_HASHMAP_FOREACH(port, mdata->card->ports, state) {
            prop_name = pa_proplist_gets(port->proplist, PA_PROP_DEVICE_BUS);
            if (prop_name != NULL) {
                if (pa_streq(prop_name, bus_name)) {
                    device = PA_DEVICE_PORT_DATA(port);
                    break;
                }
            }
        }

        if (device != NULL) {
            mdata->final_kvpairs = pa_sprintf_malloc("%s;device=%u",mdata->final_kvpairs, *device);
        } else {
            mdata->final_kvpairs = pa_xstrdup(mdata->final_kvpairs);
        }
    }

    pa_xfree(temp_kvpairs);
    return 0;
}

static void async_thread_func(void *userdata) {
    struct qahw_module_extn_data *mdata = (struct qahw_module_extn_data *)userdata;
    int ret;

    pa_log_debug("Starting Async Thread");

    pa_mutex_lock(mdata->mutex);
    while (mdata->thread_state != QAHW_THREAD_EXIT) {
        pa_log_debug("Async Thread wait");
        pa_cond_wait(mdata->cond, mdata->mutex);
        pa_log_debug("Async Thread wakeup");

        if (mdata->thread_state == QAHW_THREAD_SET_PARAM_EVENT) {
            pa_mutex_unlock(mdata->mutex);
            pa_log_debug("Async Set Parameters");

            if (update_module_kvpairs(mdata) != 0) {
                ret = -1;
                signal_set_parameters_done(mdata, ret);
            }

            ret = qahw_set_parameters(mdata->module_handle, mdata->final_kvpairs);
            if (ret)
                pa_log_debug("Async Set Parameters failed with error %d", ret);

            pa_mutex_lock(mdata->mutex);
            signal_set_parameters_done(mdata, ret);
            pa_xfree(mdata->final_kvpairs);
            mdata->final_kvpairs = NULL;
        }
    }

    pa_mutex_unlock(mdata->mutex);

    pa_log_debug("Exiting Async thread");
}

static void qahw_module_set_parameters_async(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct qahw_module_extn_data *mdata = (struct qahw_module_extn_data *)userdata;

    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s\n", __func__);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                &mdata->final_kvpairs, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    pa_mutex_lock(mdata->mutex);
    if (mdata->async_thread == NULL) {
        pa_mutex_unlock(mdata->mutex);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_module_set_parameters_async failed");
        return;
    }

    mdata->thread_state = QAHW_THREAD_SET_PARAM_EVENT;
    pa_cond_signal(mdata->cond, 0);
    pa_mutex_unlock(mdata->mutex);
    pa_dbus_send_empty_reply(conn, msg);
}

static void qahw_module_set_port_config(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    qahw_param_payload payload;

    pa_encoding_t encoding;
    pa_device_port *p;

    char *encoding_str = NULL;
    char *format_str = NULL;
    char *port_name = NULL;

    DBusMessageIter struct_i, arg_i, array_i;
    DBusError error;

    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s", __func__);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "qahw_module_set_port_config has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                set_port_config_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for set_port_config_args");
        dbus_error_free(&error);
        return;
    }


    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.device_cfg_params.sample_rate);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.device_cfg_params.channels);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.device_cfg_params.bit_width);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &encoding_str);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &format_str);

    encoding = pa_encoding_from_string(encoding_str);
    if (encoding == PA_ENCODING_INVALID) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported encoding %d", encoding);
        dbus_error_free(&error);
        return;
    } else if (encoding == PA_ENCODING_PCM) {
        payload.device_cfg_params.format = pa_qahw_util_get_qahw_format_from_pa_sample(pa_parse_sample_format(format_str));
    } else {
        payload.device_cfg_params.format = pa_qahw_util_get_qahw_format_from_pa_encoding(encoding);
    }

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &port_name);

    p = pa_hashmap_get(qahw_extn_mdata->card->ports, port_name);
    if (!p) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data failed, unsupported port %s", port_name);
        dbus_error_free(&error);
        return;
    }
    payload.device_cfg_params.device = *((int *)PA_DEVICE_PORT_DATA(p));

    pa_log_debug("device is %d", payload.device_cfg_params.device);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);

    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    if (n_elements > (int)AUDIO_CHANNEL_COUNT_MAX) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED , "set_param_data failed, unsupported channel count %d", n_elements);
        dbus_error_free(&error);
    }
    memcpy(&payload.device_cfg_params.channel_map, value, n_elements);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.device_cfg_params.channel_allocation);

    rc = qahw_set_param_data(qahw_extn_mdata->module_handle, QAHW_PARAM_DEVICE_CONFIG, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data for QAHW_PARAM_DEVICE_CONFIG failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void qahw_module_set_aptx_dec_params(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    qahw_param_payload payload;

    DBusMessageIter struct_i, arg_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "qahw_module_set_aptx_dec_params has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                set_aptx_dec_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for set_aptx_dec_args");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.aptx_params.bt_addr.nap);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.aptx_params.bt_addr.uap);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.aptx_params.bt_addr.lap);

    pa_log_debug("%s:: %d %d %d", __func__, payload.aptx_params.bt_addr.nap, payload.aptx_params.bt_addr.uap, payload.aptx_params.bt_addr.lap);

    rc = qahw_set_param_data(qahw_extn_mdata->module_handle, QAHW_PARAM_APTX_DEC, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "module_set_param_data for QAHW_PARAM_APTX_DEC failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void qahw_module_get_source_tracking_params(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    qahw_param_payload payload;
    char *value = NULL;

    DBusMessage *reply = NULL;
    DBusMessageIter struct_i, arg_i, array_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s\n", __func__);

    rc = qahw_get_param_data(qahw_extn_mdata->module_handle, QAHW_PARAM_SOURCE_TRACK, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param_data for QAHW_PARAM_SOURCE_TRACK failed");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));

    dbus_message_iter_init_append(reply, &arg_i);

    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_STRUCT, NULL, &struct_i);

    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y", &array_i);
    value = (char *)&payload.st_params.vad;
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &value, MAX_SECTORS);
    dbus_message_iter_close_container(&struct_i, &array_i);

    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT16, &payload.st_params.doa_speech);

    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "q", &array_i);
    value = (char *)&payload.st_params.doa_noise;
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_UINT16, &value, 3);
    dbus_message_iter_close_container(&struct_i, &array_i);

    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y", &array_i);
    value = (char *)&payload.st_params.polar_activity;
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &value, 360);
    dbus_message_iter_close_container(&struct_i, &array_i);

    dbus_message_iter_close_container(&arg_i, &struct_i);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    dbus_message_unref(reply);
}

static void qahw_module_get_sound_focus_params(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    qahw_param_payload payload;
    char *value = NULL;

    DBusMessage *reply = NULL;
    DBusMessageIter struct_i, arg_i, array_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s\n", __func__);

    rc = qahw_get_param_data(qahw_extn_mdata->module_handle, QAHW_PARAM_SOUND_FOCUS, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param_data for QAHW_PARAM_SOUND_FOCUS");
        dbus_error_free(&error);
        return;
    }
    pa_log_debug("%d\n", rc);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_STRUCT, NULL, &struct_i);

    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "q", &array_i);
    value = (char *)&payload.sf_params.start_angle;
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_UINT16, &value, MAX_SECTORS);
    dbus_message_iter_close_container(&struct_i, &array_i);

    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y", &array_i);
    value = (char *)&payload.sf_params.enable;
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &value, MAX_SECTORS);
    dbus_message_iter_close_container(&struct_i, &array_i);

    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT16, &payload.sf_params.gain_step);

    dbus_message_iter_close_container(&arg_i, &struct_i);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    dbus_message_unref(reply);
}

static void qahw_module_set_sound_focus_params(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc = 0;

    qahw_param_payload payload;

    DBusMessageIter struct_i, arg_i, array_i;
    DBusError error;

    int n_elements = 0;
    char *value = NULL;
    char **addr_value = &value;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "qahw_module_set_sound_focus_params");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                set_sound_focus_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for set_sound_focus_args");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_recurse(&arg_i, &struct_i);

    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    memcpy(&payload.sf_params.start_angle, value, n_elements);

    dbus_message_iter_next(&struct_i);

    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    memcpy(&payload.sf_params.enable, value, n_elements);

    dbus_message_iter_next(&struct_i);

    dbus_message_iter_get_basic(&struct_i, &payload.sf_params.gain_step);

    rc = qahw_set_param_data(qahw_extn_mdata->module_handle, QAHW_PARAM_SOUND_FOCUS, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data for QAHW_PARAM_SOUND_FOCUS");
        dbus_error_free(&error);
        return;
    }

   pa_dbus_send_empty_reply(conn, msg);
}

static void qahw_module_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc;
    const char *kv_pairs;

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

    rc = qahw_set_parameters(qahw_extn_mdata->module_handle, kv_pairs);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_set_parameters failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void qahw_module_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    const char *kv_pairs;
    char *param;

    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                &kv_pairs, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    param = qahw_get_parameters(qahw_extn_mdata->module_handle, kv_pairs);
    if (!param) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_get_parameters failed");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s:kv_param = %s response = %s\n", __func__, kv_pairs, param);

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_STRING, &param);

    free(param);
}

static void qahw_set_channel_config(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
    int rc = 0;
    uint32_t i = 0;
    qahw_param_payload payload;
    char *str[AUDIO_CHANNEL_COUNT_MAX];
    int arg_type;
    pa_channel_map channel_map;
    struct qahw_out_channel_map_param channel_map_qahw;
    void *state = NULL;
    const char *bus_name, *prop_name = NULL;
    pa_device_port *port;
    audio_devices_t *audio_device;

    DBusError error;
    DBusMessageIter arg, struct_i, array_i;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg)) {
        pa_log_error("SetChannelConfig has no arguments\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "SetChannelConfig has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(suas)")) {
        pa_log_error("Invalid signature for SetChannelConfig\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for SetPortConfig");
        dbus_error_free(&error);
        return;
    }

    pa_log_info("Unmarshalling SetChannelConfig message\n");

    memset(&channel_map_qahw, 0, sizeof(struct qahw_out_channel_map_param));
    memset(&(payload.device_cfg_params), 0, sizeof(struct qahw_device_cfg_param));

    dbus_message_iter_recurse(&arg, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &bus_name);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &payload.device_cfg_params.channels);

    channel_map.channels = payload.device_cfg_params.channels;

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);

    PA_HASHMAP_FOREACH(port, qahw_extn_mdata->card->ports, state) {
        prop_name = pa_proplist_gets(port->proplist, PA_PROP_DEVICE_BUS);
        if (prop_name != NULL) {
            if (pa_streq(prop_name, bus_name)) {
                audio_device = PA_DEVICE_PORT_DATA(port);
                payload.device_cfg_params.device = *audio_device;
                break;
            }
        }
    }

    if (!prop_name) {
        pa_log_info("%s: port property not defined\n", __func__);
        return;
    }
    pa_log_debug("Server:device is 0x%x", payload.device_cfg_params.device);
    pa_log_debug("Server:channels %d", payload.device_cfg_params.channels);

    while (((arg_type = dbus_message_iter_get_arg_type(&array_i)) != DBUS_TYPE_INVALID) && (i < payload.device_cfg_params.channels)) {
        dbus_message_iter_get_basic(&array_i, &str[i]);
        pa_log_debug("%s: Server:channel_map[%d] is %s", __func__, i, str[i]);

        channel_map.map[i] = pa_channel_position_from_string(str[i]);

        dbus_message_iter_next(&array_i);

        i++;
    }

    if (!pa_qahw_channel_map_to_qahw(&channel_map, &(channel_map_qahw))) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported channel map received");
        return;
    }

    pa_assert(payload.device_cfg_params.channels == channel_map_qahw.channels);

    for (i = 0; i < payload.device_cfg_params.channels; i++)
         payload.device_cfg_params.channel_map[i] = channel_map_qahw.channel_map[i];

    rc = qahw_set_param_data(qahw_extn_mdata->module_handle, QAHW_PARAM_DEVICE_CONFIG, &payload);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param_data for QAHW_PARAM_DEVICE_CONFIG failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

int pa_qahw_module_extn_init(pa_core *core, pa_card *card, qahw_module_handle_t *module_handle) {
    char *thread_name = NULL;

    pa_assert(core);
    pa_assert(module_handle);

    if (qahw_extn_mdata) {
        pa_log_info("%s: Module already intialized",__func__);
        return -1;
    }

    pa_log_info("%s", __func__);

    qahw_extn_mdata = pa_xnew0(struct qahw_module_extn_data, 1);

    qahw_extn_mdata->obj_path = pa_sprintf_malloc("%s/primary", QAHW_DBUS_OBJECT_PATH_PREFIX);
    qahw_extn_mdata->module_handle = module_handle;
    qahw_extn_mdata->dbus_protocol = pa_dbus_protocol_get(core);
    qahw_extn_mdata->card = card;
    qahw_extn_mdata->async_thread = NULL;
    qahw_extn_mdata->final_kvpairs = NULL;

    thread_name = pa_sprintf_malloc("qahw async thread");
    if (!(qahw_extn_mdata->async_thread = pa_thread_new(thread_name, async_thread_func, qahw_extn_mdata)))
        pa_log_error("%s: qahw async thread creation failed", __func__);

    qahw_extn_mdata->mutex = pa_mutex_new(false /* recursive  */, false /* inherit_priority */);
    qahw_extn_mdata->cond = pa_cond_new();
    qahw_extn_mdata->thread_state = QAHW_THREAD_IDLE;
    pa_xfree(thread_name);

    pa_assert_se(pa_dbus_protocol_add_interface(qahw_extn_mdata->dbus_protocol, qahw_extn_mdata->obj_path, &module_interface_info, qahw_extn_mdata) >= 0);

    return 0;
}

int pa_qahw_module_extn_deinit(void) {

    pa_assert(qahw_extn_mdata);

    qahw_extn_mdata->thread_state = QAHW_THREAD_EXIT;
    pa_cond_signal(qahw_extn_mdata->cond, 0);
    pa_thread_free(qahw_extn_mdata->async_thread);
    pa_cond_free(qahw_extn_mdata->cond);
    pa_mutex_free(qahw_extn_mdata->mutex);

    pa_assert(qahw_extn_mdata->dbus_protocol);
    pa_assert(qahw_extn_mdata->obj_path);

    pa_assert_se(pa_dbus_protocol_remove_interface(qahw_extn_mdata->dbus_protocol, qahw_extn_mdata->obj_path, module_interface_info.name) >= 0);

    pa_dbus_protocol_unref(qahw_extn_mdata->dbus_protocol);

    pa_xfree(qahw_extn_mdata->obj_path);
    pa_xfree(qahw_extn_mdata);
    qahw_extn_mdata = NULL;

    return 0;
}
