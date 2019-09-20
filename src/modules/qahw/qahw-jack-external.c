/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
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

#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/core-util.h>

#include "qahw-jack-common.h"
#include "qahw-jack-format.h"

#define QAHW_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qahw/port"
#define QAHW_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Qahw.Module"

typedef struct {
    char *obj_path;
    pa_dbus_protocol *dbus_protocol;
    pa_hook event_hook;
    pa_qahw_jack_type_t jack_type;
} pa_qahw_external_jack_data;

enum module_method_handler_index {
    METHOD_HANDLER_START_STREAM = 0,
    METHOD_HANDLER_START_COMPRESS_STREAM,
    METHOD_HANDLER_STOP_STREAM,
    METHOD_HANDLER_SET_PARAM,
    METHOD_HANDLER_MODULE_LAST = METHOD_HANDLER_SET_PARAM,
    METHOD_HANDLER_MODULE_MAX = METHOD_HANDLER_MODULE_LAST + 1,
};

static void qahw_jack_external_start_stream(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_jack_external_start_compress_stream(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_jack_external_stop_stream(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void qahw_jack_external_set_param(DBusConnection *conn, DBusMessage *msg, void *userdata);

static pa_dbus_arg_info start_stream_args[] = {
    {"stream_config", "(suss)", "in"},
};

static pa_dbus_arg_info start_compress_stream_args[] = {
    {"stream_config", "(sussu)", "in"},
};

static pa_dbus_arg_info stop_stream_args[] = {
};

static pa_dbus_arg_info set_param_args[] = {
    {"param_string", "s", "in"},
};

static pa_dbus_method_handler module_method_handlers[METHOD_HANDLER_MODULE_MAX] = {
[METHOD_HANDLER_START_STREAM] = {
        .method_name = "StartStream",
        .arguments = start_stream_args,
        .n_arguments = sizeof(start_stream_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_jack_external_start_stream },
[METHOD_HANDLER_START_COMPRESS_STREAM] = {
        .method_name = "StartCompressStream",
        .arguments = start_compress_stream_args,
        .n_arguments = sizeof(start_compress_stream_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_jack_external_start_compress_stream },
[METHOD_HANDLER_STOP_STREAM] = {
        .method_name = "StopStream",
        .arguments = stop_stream_args,
        .n_arguments = sizeof(stop_stream_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_jack_external_stop_stream },
[METHOD_HANDLER_SET_PARAM] = {
        .method_name = "SetParam",
        .arguments = set_param_args,
        .n_arguments = sizeof(set_param_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = qahw_jack_external_set_param },
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


static void qahw_jack_external_start_stream(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_jack_out_config config;
    pa_qahw_jack_event_data_t event_data;
    pa_qahw_external_jack_data *external_jdata = userdata;

    char *encoding_str = NULL;
    char *format_str = NULL;
    char *map = NULL;
    uint32_t rate;

    DBusMessageIter struct_i, arg_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s", __func__);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "qahw_jack_external_start_stream has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                start_stream_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for start_stream");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &encoding_str);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &rate);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &format_str);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &map);

    pa_log_info("%s: external source port %s, encoding %s, rate %d, format  %s map %s", __func__,
                            pa_qahw_util_get_port_name_from_jack_type(external_jdata->jack_type),
                                                            encoding_str, rate, format_str, map);

    /* Set default preempt status as 0 */
    config.preemph_status = 0;
    config.ss.format = PA_SAMPLE_S16LE;
    config.encoding = pa_encoding_from_string(encoding_str);
    if (config.encoding == PA_ENCODING_INVALID) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported encoding %s", encoding_str);
        dbus_error_free(&error);
        return;
    } else if (config.encoding == PA_ENCODING_PCM) {
        config.ss.format = pa_parse_sample_format(format_str);
    }

    if (!pa_channel_map_parse(&config.map, map)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported channel map %s", map);
        dbus_error_free(&error);
        return;
    }

    config.ss.rate = rate;
    config.ss.channels = config.map.channels;

    event_data.jack_type = external_jdata->jack_type;

    /* generate jack available event */
    event_data.event = PA_QAHW_JACK_AVAILABLE;
    pa_hook_fire(&(external_jdata->event_hook), &event_data);

    /* generate jack config update event */
    event_data.pa_qahw_jack_info = &config;
    event_data.event = PA_QAHW_JACK_CONFIG_UPDATE;
    pa_hook_fire(&(external_jdata->event_hook), &event_data);

    pa_dbus_send_empty_reply(conn, msg);
}

static void qahw_jack_external_start_compress_stream(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_jack_out_config config;
    pa_qahw_jack_event_data_t event_data;
    pa_qahw_external_jack_data *external_jdata = userdata;

    char *encoding_str = NULL;
    char *format_str = NULL;
    char *map = NULL;
    uint32_t rate;
    uint32_t preemph_status;

    DBusMessageIter struct_i, arg_i;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s", __func__);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "qahw_jack_external_start_compress_stream has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                start_compress_stream_args[0].type)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for start_compress_stream");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &encoding_str);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &rate);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &format_str);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &map);

    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &preemph_status);

    pa_log_info("%s: external source port %s, encoding %s, rate %d, format  %s map %s preemph status %d", __func__,
                                              pa_qahw_util_get_port_name_from_jack_type(external_jdata->jack_type),
                                                               encoding_str, rate, format_str, map, preemph_status);

    config.preemph_status = preemph_status;
    config.ss.format = PA_SAMPLE_S16LE;
    config.encoding = pa_encoding_from_string(encoding_str);
    if (config.encoding == PA_ENCODING_INVALID) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported encoding %s", encoding_str);
        dbus_error_free(&error);
        return;
    } else if (config.encoding == PA_ENCODING_PCM) {
        config.ss.format = pa_parse_sample_format(format_str);
    }

    if (!pa_channel_map_parse(&config.map, map)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported channel map %s", map);
        dbus_error_free(&error);
        return;
    }

    config.ss.rate = rate;
    config.ss.channels = config.map.channels;

    event_data.jack_type = external_jdata->jack_type;

    /* generate jack available event */
    event_data.event = PA_QAHW_JACK_AVAILABLE;
    pa_hook_fire(&(external_jdata->event_hook), &event_data);

    /* generate jack config update event */
    event_data.pa_qahw_jack_info = &config;
    event_data.event = PA_QAHW_JACK_CONFIG_UPDATE;
    pa_hook_fire(&(external_jdata->event_hook), &event_data);

    pa_dbus_send_empty_reply(conn, msg);
}

static void qahw_jack_external_stop_stream(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_external_jack_data *external_jdata = userdata;
    pa_qahw_jack_event_data_t event_data;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    /* generate jack config update event */
    event_data.jack_type = external_jdata->jack_type;
    event_data.event = PA_QAHW_JACK_UNAVAILABLE;
    pa_hook_fire(&(external_jdata->event_hook), &event_data);

    pa_dbus_send_empty_reply(conn, msg);
}

static void qahw_jack_external_set_param(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_jack_event_data_t event_data;
    pa_qahw_external_jack_data *external_jdata = userdata;
    const char *param = NULL;

    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s", __func__);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &param, DBUS_TYPE_INVALID)) {
        pa_log_error("Invalid signature for SetParam - %s\n", error.message);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for SetParam");
        dbus_error_free(&error);
        return;
    }

    pa_log_info("%s: external source port %s  set param %s", __func__,
                pa_qahw_util_get_port_name_from_jack_type(external_jdata->jack_type), param);

    /* Generate jack set param event */
    event_data.jack_type = external_jdata->jack_type;
    event_data.event = PA_QAHW_JACK_SET_PARAM;
    event_data.pa_qahw_jack_info = (void *)param;
    pa_hook_fire(&(external_jdata->event_hook), &event_data);

    pa_dbus_send_empty_reply(conn, msg);
}

struct pa_qahw_jack_data* pa_qahw_external_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m,
                               pa_hook_slot **hook_slot, pa_qahw_jack_callback_t callback, void *client_data) {
    struct pa_qahw_jack_data *jdata = NULL;
    pa_qahw_external_jack_data *external_jdata = NULL;
    const char *port_name = NULL;
    char *port_name_underscore = NULL;

    jdata = pa_xnew0(struct pa_qahw_jack_data, 1);

    external_jdata = pa_xnew0(pa_qahw_external_jack_data, 1);
    jdata->prv_data = external_jdata;

    port_name = pa_qahw_util_get_port_name_from_jack_type(jack_type);
    if (!port_name) {
        pa_log_error("Invalid port jack %d\n", jack_type);
        return NULL;
    }

    port_name_underscore = pa_xstrdup(port_name);

    /* replace hyphen with underscore as in dbus doesn't allow hyphen in name */
    port_name_underscore = pa_replace(port_name, "-", "_");

    external_jdata->obj_path = pa_sprintf_malloc("%s/%s", QAHW_DBUS_OBJECT_PATH_PREFIX, port_name_underscore);
    external_jdata->dbus_protocol = pa_dbus_protocol_get(m->core);

    pa_xfree(port_name_underscore);

    pa_assert_se(pa_dbus_protocol_add_interface(external_jdata->dbus_protocol, external_jdata->obj_path, &module_interface_info, external_jdata) >= 0);

    external_jdata->jack_type = jack_type;
    jdata->jack_type = jack_type;

    pa_hook_init(&(external_jdata->event_hook), NULL);
    jdata->event_hook = &(external_jdata->event_hook);

    *hook_slot = pa_hook_connect(&(external_jdata->event_hook), PA_HOOK_NORMAL, (pa_hook_cb_t)callback, client_data);

    return jdata;
}

void pa_qahw_external_jack_detection_disable(struct pa_qahw_jack_data *jdata, pa_module *m) {
    pa_qahw_external_jack_data *external_jdata;
    pa_assert(jdata);

    external_jdata = (pa_qahw_external_jack_data *)jdata->prv_data;

    pa_assert_se(pa_dbus_protocol_remove_interface(external_jdata->dbus_protocol, external_jdata->obj_path, module_interface_info.name) >= 0);

    pa_dbus_protocol_unref(external_jdata->dbus_protocol);

    pa_xfree(external_jdata->obj_path);

    pa_hook_done(&(external_jdata->event_hook));

    pa_xfree(external_jdata);

    pa_xfree(jdata);
    jdata = NULL;
}
