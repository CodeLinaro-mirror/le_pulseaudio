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

struct qahw_module_extn_data {
    char *obj_path;
    pa_dbus_protocol *dbus_protocol;
    qahw_module_handle_t *module_handle;
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

enum module_method_handler_index {
    METHOD_HANDLER_SET_PORT_CONFIG = 0,
    METHOD_HANDLER_SET_APTX_DEC_PARAMS,
    METHOD_HANDLER_GET_SOURCE_TRACKING,
    METHOD_HANDLER_GET_SOUND_FOCUS,
    METHOD_HANDLER_SET_SOUND_FOCUS,
    METHOD_HANDLER_SET_PARAMETERS,
    METHOD_HANDLER_GET_PARAMETERS,
    METHOD_HANDLER_MODULE_LAST = METHOD_HANDLER_GET_PARAMETERS,
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
[METHOD_HANDLER_GET_PARAMETERS] = {
        .method_name = "GetParameters",
        .arguments = get_parameters_args,
        .n_arguments = sizeof(get_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_module_get_parameters},
};

static pa_dbus_interface_info module_interface_info = {
    .name = QAHW_DBUS_MODULE_IFACE,
    .method_handlers = module_method_handlers,
    .n_method_handlers = METHOD_HANDLER_MODULE_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

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
    if (n_elements > AUDIO_CHANNEL_COUNT_MAX) {
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

int pa_qahw_module_extn_init(pa_core *core, pa_card *card, qahw_module_handle_t *module_handle) {
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

    pa_assert_se(pa_dbus_protocol_add_interface(qahw_extn_mdata->dbus_protocol, qahw_extn_mdata->obj_path, &module_interface_info, qahw_extn_mdata) >= 0);

    return 0;
}

int pa_qahw_module_extn_deinit(void) {

    pa_assert(qahw_extn_mdata);

    pa_assert(qahw_extn_mdata->dbus_protocol);
    pa_assert(qahw_extn_mdata->obj_path);

    pa_assert_se(pa_dbus_protocol_remove_interface(qahw_extn_mdata->dbus_protocol, qahw_extn_mdata->obj_path, module_interface_info.name) >= 0);

    pa_dbus_protocol_unref(qahw_extn_mdata->dbus_protocol);

    pa_xfree(qahw_extn_mdata->obj_path);
    pa_xfree(qahw_extn_mdata);
    qahw_extn_mdata = NULL;

    return 0;
}
