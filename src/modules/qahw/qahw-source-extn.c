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

#include "qahw-source-extn.h"

#define QAHW_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qahw/source"
#define QAHW_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Qahw"
#define QAHW_DBUS_SOURCE_IFACE "org.PulseAudio.Ext.Qahw.Source"

struct pa_qahw_source_extn_data {
    char *obj_path;
    pa_dbus_protocol *dbus_protocol;
    qahw_stream_handle_t *in_handle;
};

/* source set params based on key,value */
static void pa_qahw_source_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_source_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum source_method_handler_index {
    METHOD_HANDLER_SOURCE_SET_PARAMETERS,
    METHOD_HANDLER_SOURCE_GET_PARAMETERS,
    METHOD_HANDLER_SOURCE_LAST = METHOD_HANDLER_SOURCE_GET_PARAMETERS,
    METHOD_HANDLER_SOURCE_MAX = METHOD_HANDLER_SOURCE_LAST + 1,
};

static pa_dbus_arg_info source_set_parameters_args[] = {
    {"kv_pairs", "s", "in"},
};

static pa_dbus_arg_info source_get_parameters_args[] = {
    {"kv_pairs", "s", "in"},
    {"value", "s", "out"},
};


static pa_dbus_method_handler source_method_handlers[METHOD_HANDLER_SOURCE_MAX] = {
[METHOD_HANDLER_SOURCE_SET_PARAMETERS] = {
        .method_name = "SourceSetParameters",
        .arguments = source_set_parameters_args,
        .n_arguments = sizeof(source_set_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_source_set_parameters},
[METHOD_HANDLER_SOURCE_GET_PARAMETERS] = {
        .method_name = "GetParameters",
        .arguments = source_get_parameters_args,
        .n_arguments = sizeof(source_get_parameters_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_source_get_parameters},
};

static pa_dbus_interface_info source_interface_info = {
    .name = QAHW_DBUS_SOURCE_IFACE,
    .method_handlers = source_method_handlers,
    .n_method_handlers = METHOD_HANDLER_SOURCE_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

static void pa_qahw_source_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int rc;
    const char *kv_pairs;

    struct pa_qahw_source_extn_data *qahw_extn_sdata = (struct pa_qahw_source_extn_data *)userdata;

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

    rc = qahw_in_set_parameters(qahw_extn_sdata->in_handle, kv_pairs);
    if (rc) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_in_set_parameters failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_source_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    const char *kv_pairs;
    char *param;

    struct pa_qahw_source_extn_data *qahw_extn_sdata = (struct pa_qahw_source_extn_data *)userdata;

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

    param = qahw_in_get_parameters(qahw_extn_sdata->in_handle, kv_pairs);
    if (!param) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_in_get_parameters failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_STRING, &param);

    free(param);
}

int pa_qahw_source_extn_source_handle_update(pa_qahw_source_extn_handle_t *handle, qahw_stream_handle_t *in_handle) {
    struct pa_qahw_source_extn_data *qahw_extn_sdata = (struct pa_qahw_source_extn_data *)handle;

    pa_assert(handle);
    pa_assert(in_handle);

    qahw_extn_sdata->in_handle = in_handle;

    return 0;
}

int pa_qahw_source_extn_create(pa_core *core, qahw_stream_handle_t *in_handle, int pa_source_index, pa_qahw_source_extn_handle_t **handle) {

    struct pa_qahw_source_extn_data *qahw_extn_sdata;

    pa_assert(core);
    pa_assert(in_handle);

    qahw_extn_sdata = pa_xnew0(struct pa_qahw_source_extn_data, 1);

    qahw_extn_sdata->obj_path = pa_sprintf_malloc("%s%d", QAHW_DBUS_OBJECT_PATH_PREFIX, pa_source_index);
    qahw_extn_sdata->in_handle = in_handle;
    qahw_extn_sdata->dbus_protocol = pa_dbus_protocol_get(core);

    pa_assert_se(pa_dbus_protocol_add_interface(qahw_extn_sdata->dbus_protocol, qahw_extn_sdata->obj_path, &source_interface_info, qahw_extn_sdata) >= 0);

    *handle = (pa_qahw_source_extn_handle_t *)qahw_extn_sdata;

    pa_log_info("pa_qahw_source_extn created for source %d", pa_source_index);

    return 0;
}

int pa_qahw_source_extn_free(pa_qahw_source_extn_handle_t *handle) {
    struct pa_qahw_source_extn_data *qahw_extn_sdata = (struct pa_qahw_source_extn_data *)handle;

    pa_assert(qahw_extn_sdata);

    pa_assert(qahw_extn_sdata->dbus_protocol);
    pa_assert(qahw_extn_sdata->obj_path);

    pa_assert_se(pa_dbus_protocol_remove_interface(qahw_extn_sdata->dbus_protocol, qahw_extn_sdata->obj_path, source_interface_info.name) >= 0);

    pa_dbus_protocol_unref(qahw_extn_sdata->dbus_protocol);

    pa_xfree(qahw_extn_sdata->obj_path);
    pa_xfree(qahw_extn_sdata);

    return 0;
}


