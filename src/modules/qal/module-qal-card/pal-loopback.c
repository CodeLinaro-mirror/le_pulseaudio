/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-format.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/card.h>
#include <pulsecore/core.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include "pal-loopback.h"
#include "qal-utils.h"
#include "qal-card.h"
#include "bt-a2dp-split.h"
#include "hfp.h"

#define PA_PAL_LOOPBACK_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/pal"
#define PA_PAL_LOOPBACK_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Loopback"
#define PA_PAL_LOOPBACK_DBUS_SESSION_IFACE "org.PulseAudio.Ext.Loopback.Session"

/* Usecase handles */
btsco_t *btsco = NULL;
btsink_t *btsink = NULL;

/* Module data handle */
pa_pal_loopback_module_data_t *pa_pal_loopback_mdata_ptr = NULL;

/* Handler function declarations */
static void pa_pal_loopback_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_pal_loopback_destroy(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_pal_loopback_set_volume(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_pal_loopback_set_mute(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_pal_loopback_set_samplerate(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_pal_loopback_get_volume(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_pal_loopback_get_samplerate(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_pal_bt_connect(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_pal_bt_disconnect(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum pa_pal_module_handler_index {
    MODULE_HANDLER_BT_CONNECT,
    MODULE_HANDLER_BT_DISCONNECT,
    MODULE_HANDLER_MAX
};

enum pa_pal_session_handler_index {
    SESSION_HANDLER_CREATE_LOOPBACK,
    SESSION_HANDLER_DESTROY_LOOPBACK,
    SESSION_HANDLER_SET_VOLUME,
    SESSION_HANDLER_GET_VOLUME,
    SESSION_HANDLER_SET_MUTE,
    SESSION_HANDLER_SET_SAMPLE_RATE,
    SESSION_HANDLER_GET_SAMPLE_RATE,
    SESSION_HANDLER_MAX
};

/* Handler args */
pa_dbus_arg_info pa_pal_bt_connect_args[] = {
    {"connection_args", "s", "in"},
    {"object_path", "o", "out"},
};

pa_dbus_arg_info pa_pal_bt_disconnect_args[] = {
    {"connection_args", "s", "in"},
};

pa_dbus_arg_info pa_pal_loopback_create_args[] = {
    /* no args */
};

pa_dbus_arg_info pa_pal_loopback_destroy_args[] = {
    /* no args */
};

pa_dbus_arg_info pa_pal_loopback_set_volume_args[] = {
    {"volume_args", "(ds)", "in"},
};

pa_dbus_arg_info pa_pal_loopback_get_volume_args[] = {
    {"loopback_profile", "s", "in"},
    {"volume_args", "d", "out"},
};

pa_dbus_arg_info pa_pal_loopback_set_mute_args[] = {
    {"mute_args", "(bs)", "in"},
};

pa_dbus_arg_info pa_pal_loopback_set_samplerate_args[] = {
    {"sample_rate", "u", "in"},
};

pa_dbus_arg_info pa_pal_loopback_get_samplerate_args[] = {
    {"sample_rate", "u", "out"},
};

static pa_dbus_method_handler pa_pal_loopback_module_handlers[MODULE_HANDLER_MAX] = {
    [MODULE_HANDLER_BT_CONNECT] = {
        .method_name = "BtConnect",
        .arguments = pa_pal_bt_connect_args,
        .n_arguments = sizeof(pa_pal_bt_connect_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_bt_connect},
    [MODULE_HANDLER_BT_DISCONNECT] = {
        .method_name = "BtDisconnect",
        .arguments = pa_pal_bt_disconnect_args,
        .n_arguments = sizeof(pa_pal_bt_disconnect_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_bt_disconnect},
};

static pa_dbus_method_handler pa_pal_loopback_session_handlers[SESSION_HANDLER_MAX] = {
    [SESSION_HANDLER_CREATE_LOOPBACK] = {
        .method_name = "CreateLoopback",
        .arguments = pa_pal_loopback_create_args,
        .n_arguments = sizeof(pa_pal_loopback_create_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_loopback_create},
    [SESSION_HANDLER_DESTROY_LOOPBACK] = {
        .method_name = "DestroyLoopback",
        .arguments = pa_pal_loopback_destroy_args,
        .n_arguments = sizeof(pa_pal_loopback_destroy_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_loopback_destroy},
    [SESSION_HANDLER_SET_VOLUME] = {
        .method_name = "SetVolume",
        .arguments = pa_pal_loopback_set_volume_args,
        .n_arguments = sizeof(pa_pal_loopback_set_volume_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_loopback_set_volume},
    [SESSION_HANDLER_GET_VOLUME] = {
        .method_name = "GetVolume",
        .arguments = pa_pal_loopback_get_volume_args,
        .n_arguments = sizeof(pa_pal_loopback_get_volume_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_loopback_get_volume},
    [SESSION_HANDLER_SET_MUTE] = {
        .method_name = "SetMute",
        .arguments = pa_pal_loopback_set_mute_args,
        .n_arguments = sizeof(pa_pal_loopback_set_mute_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_loopback_set_mute},
    [SESSION_HANDLER_SET_SAMPLE_RATE] = {
        .method_name = "SetSampleRate",
        .arguments = pa_pal_loopback_set_samplerate_args,
        .n_arguments = sizeof(pa_pal_loopback_set_samplerate_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_loopback_set_samplerate},
    [SESSION_HANDLER_GET_SAMPLE_RATE] = {
        .method_name = "GetSampleRate",
        .arguments = pa_pal_loopback_get_samplerate_args,
        .n_arguments = sizeof(pa_pal_loopback_get_samplerate_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_pal_loopback_get_samplerate},
};

pa_dbus_interface_info pa_pal_loopback_module_interface_info = {
    .name = PA_PAL_LOOPBACK_DBUS_MODULE_IFACE,
    .method_handlers = pa_pal_loopback_module_handlers,
    .n_method_handlers = MODULE_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

pa_dbus_interface_info pa_pal_loopback_session_interface_info = {
    .name = PA_PAL_LOOPBACK_DBUS_SESSION_IFACE,
    .method_handlers = pa_pal_loopback_session_handlers,
    .n_method_handlers = SESSION_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
};

/******* Helper functions ********/
bool check_valid_usecase(char *uc)
{
    for (int i = PA_PAL_UC_BT_A2DP_SINK; i < PA_PAL_UC_BT_MAX; i++) {
        if (strcmp(uc, usecase_name_list[i]) == 0)
            return true;
    }
    pa_log_error("Usecase doesn't exist\n");

    return false;
}

static DBusHandlerResult disconnection_filter_cb(DBusConnection *conn,
        DBusMessage *msg, void *userdata) {
    void *state = NULL;
    const void* key = NULL;
    pa_pal_loopback_module_data_t *m_data = NULL;
    pa_pal_loopback_ses_data_t *ses_data = NULL;
    pa_pal_loopback_config *loopback_config[MAX_LOOPBACK_PROFILES];

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s: Enter\n", __func__);
    m_data = (pa_pal_loopback_module_data_t*)userdata;

    if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Local", "Disconnected")) {
        /* connection died, deinit all the sessions for which callback got triggered */
        pa_log_info("connection died for all sessions\n");

        if (btsink) {
            loopback_config[0] = pa_hashmap_get(m_data->loopback_confs, "bta2dp");
            deinit_btsink(btsink, loopback_config[0]);
            btsink = NULL;
        }

        if (btsco) {
            loopback_config[LB_PROF_HFP_RX] = pa_hashmap_get(m_data->loopback_confs, "hfp_rx");
            loopback_config[LB_PROF_HFP_TX] = pa_hashmap_get(m_data->loopback_confs, "hfp_tx");
            deinit_btsco(btsco, loopback_config);
            btsco = NULL;
        }

        while ((ses_data = pa_hashmap_iterate(m_data->session_data, &state, &key))) {
            pa_assert_se(pa_dbus_protocol_remove_interface(m_data->dbus_protocol, ses_data->obj_path,
                        pa_pal_loopback_session_interface_info.name) >= 0);

            pa_hashmap_remove(m_data->session_data, key);
            pa_xfree(ses_data->obj_path);
            pa_xfree(ses_data);
            m_data->session_count--;
        }
    }
    pa_log_debug("%s: Exit\n", __func__);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/******* Module handler functions ********/
static void pa_pal_bt_connect(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
    int32_t ret = 0;
    char *usecase = NULL;
    pa_pal_loopback_ses_data_t *ses_data = NULL;
    pa_pal_loopback_config *loopback_config[MAX_LOOPBACK_PROFILES];
    DBusMessageIter arg_i;
    DBusError error;
    DBusMessage *reply = NULL;
    pa_pal_loopback_module_data_t *m_data = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    m_data = (pa_pal_loopback_module_data_t*)userdata;

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                &usecase, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        goto error_1;
    }

    pa_log_debug("%s:%d: usecase=%s \n", __func__, __LINE__, usecase);

    /* Validate use case name */
    if (!check_valid_usecase(usecase)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "%s is not a valid usecase", usecase);
        goto error_1;
    }

    /* Check if connection exists for the usecase */
    if (pa_hashmap_get(m_data->session_data, usecase)) {
        pa_log_error("Connection already exists for %s\n", usecase);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Connection already exists for %s\n", usecase);
        goto error_1;
    }

    /* Allocate session data & fill it as per the use case */
    ses_data = pa_xnew0(pa_pal_loopback_ses_data_t, 1);
    if (!ses_data) {
        pa_log_error("Memory allocation failed for session data");
        goto error_1;
    }

    /* Allocate and fill usecase data */
    if (strcmp(usecase, usecase_name_list[PA_PAL_UC_BT_A2DP_SINK]) == 0) {
        loopback_config[0] = pa_hashmap_get(m_data->loopback_confs, "bta2dp");
        if (!loopback_config[0]) {
            pa_log_error("Failed to fetch loopback config");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                    "loopback_conf doesn't exist for the profile");
            goto error_1;
        }
        ret = init_btsink(&btsink, loopback_config[0]);
        if (ret) {
            btsink = NULL;
            goto error_1;
        }
    }
    else if (strcmp(usecase, usecase_name_list[PA_PAL_UC_BT_SCO]) == 0) {
        loopback_config[LB_PROF_HFP_RX] = pa_hashmap_get(m_data->loopback_confs, "hfp_rx");
        if (!loopback_config[LB_PROF_HFP_RX]) {
            pa_log_error("Failed to fetch loopback config");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                    "loopback_conf doesn't exist for the profile");
            goto error_1;
        }
        loopback_config[LB_PROF_HFP_TX] = pa_hashmap_get(m_data->loopback_confs, "hfp_tx");
        if (!loopback_config[LB_PROF_HFP_TX]) {
            pa_log_error("Failed to fetch loopback config");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                    "loopback_conf doesn't exist for the profile");
            goto error_1;
        }
        ret = init_btsco(&btsco, loopback_config);
        if (ret) {
            btsco = NULL;
            goto error_1;
        }
    }
    else {
        pa_log_error("Invalid usecase name %s", usecase);
        goto error_1;
    }

    if (!m_data->session_count)
        pa_assert_se(dbus_connection_add_filter(conn, disconnection_filter_cb, m_data, NULL));

    pa_strlcpy(ses_data->usecase, usecase, MAX_USECASE_NAME_LENGTH);
    ses_data->common = m_data;
    ses_data->obj_path = pa_sprintf_malloc("%s/ses_%u", m_data->dbus_path,
            ++m_data->session_count);
    memcpy(ses_data->loopback_config, loopback_config, sizeof(loopback_config));
    pa_log_info("session obj path %s \n", ses_data->obj_path);

    pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol,
                ses_data->obj_path, &pa_pal_loopback_session_interface_info,
                ses_data) >= 0);

    /* Add usecase and its data to hashmap */
    pa_hashmap_put(m_data->session_data, ses_data->usecase, ses_data);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &ses_data->obj_path);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    dbus_error_free(&error);
    return;

error_1:
    pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "BT connection failed\n");
    dbus_error_free(&error);

    return;
}

static void pa_pal_bt_disconnect(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
    char *usecase = NULL;
    pa_pal_loopback_ses_data_t *ses_data = NULL;
    pa_pal_loopback_config *loopback_config[MAX_LOOPBACK_PROFILES];
    DBusError error;
    pa_pal_loopback_module_data_t *m_data = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    m_data = (pa_pal_loopback_module_data_t*)userdata;

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                &usecase, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s:%d: usecase=%s \n", __func__, __LINE__, usecase);

    /* Validate use case name */
    if (!check_valid_usecase(usecase)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "%s is not a valid usecase", usecase);
        return;
    }

    if (((strcmp(usecase, usecase_name_list[PA_PAL_UC_BT_A2DP_SINK]) == 0) && btsink)){
        loopback_config[0] = pa_hashmap_get(m_data->loopback_confs, "bta2dp");
        deinit_btsink(btsink, loopback_config[0]);
        btsink = NULL;
    }
    else if (((strcmp(usecase, usecase_name_list[PA_PAL_UC_BT_SCO])) == 0) && btsco) {
        loopback_config[LB_PROF_HFP_RX] = pa_hashmap_get(m_data->loopback_confs, "hfp_rx");
        loopback_config[LB_PROF_HFP_TX] = pa_hashmap_get(m_data->loopback_confs, "hfp_tx");
        deinit_btsco(btsco, loopback_config);
        btsco = NULL;
    }

    ses_data = pa_hashmap_get(m_data->session_data, usecase);
    if (!ses_data) {
        pa_log_error("ses_data not found in the records for usecase %s\n", usecase);
        goto error_1;
    }

    pa_assert_se(pa_dbus_protocol_remove_interface(m_data->dbus_protocol, ses_data->obj_path,
                pa_pal_loopback_session_interface_info.name) >= 0);
    --m_data->session_count;

    if (!m_data->session_count)
        dbus_connection_remove_filter(conn, disconnection_filter_cb, m_data);

    pa_hashmap_remove(m_data->session_data, usecase);
    pa_xfree(ses_data->obj_path);
    pa_xfree(ses_data);

    dbus_error_free(&error);
    pa_dbus_send_empty_reply(conn, msg);

    return;

error_1:
    pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "BT disconnection failed\n");
    dbus_error_free(&error);
    return;
}

/******* Session specific functions ********/
static void pa_pal_loopback_create(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
    int32_t ret = 0;
    DBusError error;
    pa_pal_loopback_ses_data_t *ses_data = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    ses_data = (pa_pal_loopback_ses_data_t *)userdata;

    pa_log_debug("Creating loopback for %s usecase\n", ses_data->usecase);
    if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_A2DP_SINK]) == 0) {
        if (btsink->is_running) {
            pa_log_debug("Session already running\n");
            goto done;
        }

        ret = start_btsink(btsink, ses_data->loopback_config[0]);
    }
    else if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_SCO]) == 0) {
        if (btsco->is_running) {
            pa_log_debug("Session already running\n");
            goto done;
        }

        ret = start_hfp(btsco, ses_data->loopback_config);
    }
    else {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                "Invalid usecase name %s", ses_data->usecase);
        goto error;
    }

    if (ret) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Failed to start %s",
                ses_data->usecase);
        goto error;
    }

done:
    dbus_error_free(&error);
    pa_dbus_send_empty_reply(conn, msg);
    return;

error:
    dbus_error_free(&error);
    return;
}

static void pa_pal_loopback_set_volume(DBusConnection *conn, DBusMessage *msg,
        void *userdata)
{
    int ret = E_SUCCESS;
    double vol = 0.0;
    int num_channels = 0;
    DBusMessageIter arg, struct_i;
    char *loopback_profile_name = NULL;
    pa_pal_loopback_ses_data_t *ses_data = NULL;
    pa_pal_loopback_config **loopback_config = NULL;
    pa_pal_card_port_config *port_config = NULL;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    ses_data = (pa_pal_loopback_ses_data_t *)userdata;
    loopback_config = ses_data->loopback_config;

    if (!pa_streq(dbus_message_get_signature(msg),
                pa_pal_loopback_set_volume_args[0].type)) {
        pa_log_error("pa_pal_bt_connection args parse error\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for pa_pal_bt_connection_args");
        ret = E_FAILURE;
        goto error;
    }

    if (!dbus_message_iter_init(msg, &arg)) {
        ret = E_FAILURE;
        goto error;
    }

    dbus_message_iter_recurse(&arg, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &vol);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &loopback_profile_name);

    pa_log_debug("Setting %s volume to %f", ses_data->usecase, vol);

    if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_A2DP_SINK]) == 0) {
        if (!btsink) {
            pa_log_debug("%s connection is not active, ignoring set_volume call\n",
                    ses_data->usecase);
            ret = E_FAILURE;
            goto error;
        }
        port_config = pa_hashmap_first(loopback_config[0]->out_ports);
        if (port_config)
            num_channels = port_config->default_map.channels;

        btsink->volume = vol;
        ret = pa_pal_set_volume(btsink->stream_handle, num_channels, vol);
    }
    else if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_SCO]) == 0) {
        if (!btsco) {
            pa_log_debug("%s connection is not active, ignoring set_volume call\n",
                    ses_data->usecase);
            ret = E_FAILURE;
            goto error;
        }
        if (strcmp(loopback_profile_name, loopback_config[LB_PROF_HFP_RX]->name) == 0) {
            port_config = pa_hashmap_first(loopback_config[LB_PROF_HFP_RX]->in_ports);
            if (port_config)
                num_channels = port_config->default_map.channels;
            btsco->rx_volume = vol;
            ret = pa_pal_set_volume(btsco->rx_stream_handle, num_channels, vol);
        }
        else if (strcmp(loopback_profile_name, loopback_config[LB_PROF_HFP_TX]->name) == 0) {
            btsco->tx_volume = vol;
            port_config = pa_hashmap_first(loopback_config[LB_PROF_HFP_TX]->out_ports);
            if (port_config)
                num_channels = port_config->default_map.channels;
            ret = pa_pal_set_volume(btsco->tx_stream_handle, num_channels, vol);
        }
    }
    else {
        pa_log_error("Invalid usecase %s", ses_data->usecase);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid usecase %s!!\n",
                ses_data->usecase);
    }

    if (ret == -EINVAL)
        pa_log_debug("Volume cached. Will be applied when session goes active");

error:
    dbus_error_free(&error);
    if (ret == E_FAILURE) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set volume for %s failed!!\n",
                ses_data->usecase);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_pal_loopback_set_samplerate(DBusConnection *conn, DBusMessage *msg,
        void *userdata)
{
    int ret = E_SUCCESS;
    DBusError error;
    uint32_t sample_rate = 0;
    pa_pal_loopback_ses_data_t *ses_data = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    ses_data = (pa_pal_loopback_ses_data_t *)userdata;

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32,
                &sample_rate, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_SCO]) == 0) {
        if (!btsco) {
            pa_log_debug("%s connection is not active\n", ses_data->usecase);
            ret = E_FAILURE;
            goto error;
        }
        if (sample_rate == 8000 || sample_rate == 16000) {
            pa_log_debug("Caching the sample rate %u for btsco\n", sample_rate);
            btsco->sample_rate = sample_rate;
        }
        else {
            pa_log_error("Sampling rate %u not supported for usecase %s",
                    sample_rate, ses_data->usecase);
            ret = E_FAILURE;
        }
    }
    else {
        pa_log_error("Invalid usecase %s", ses_data->usecase);
        ret = E_FAILURE;
    }

error:
    dbus_error_free(&error);
    if (ret != E_SUCCESS) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Set sample rate failed!!\n");
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_pal_loopback_set_mute(DBusConnection *conn, DBusMessage *msg,
        void *userdata)
{
    int32_t ret = 0;
    bool is_mute = false;
    char *loopback_profile_name = NULL;
    DBusMessageIter arg, struct_i;
    DBusError error;
    pa_pal_loopback_ses_data_t *ses_data = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    ses_data = (pa_pal_loopback_ses_data_t *)userdata;
    if (!dbus_message_iter_init(msg, &arg)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "pa_pal_loopback_set_mute has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                pa_pal_loopback_set_mute_args[0].type)) {
        pa_log_error("pa_pal_loopback_set_mute args parse error\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
                "Invalid signature for pa_pal_loopback_set_mute");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &is_mute);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &loopback_profile_name);

    pa_log_debug("Set mute %d for %s lb_profile %s", is_mute, ses_data->usecase,
            loopback_profile_name);

    if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_A2DP_SINK]) == 0) {
        if (!btsink) {
            pa_log_debug("%s connection is not active, ignoring set_mute call\n",
                    ses_data->usecase);
            ret = E_FAILURE;
            goto error;
        }
        ret = pal_stream_set_mute(btsink->stream_handle, is_mute);
        if (!ret)
            btsink->is_mute = is_mute;
    }
    else if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_SCO]) == 0) {
        if (!btsco) {
            pa_log_debug("%s connection is not active, ignoring set_mute call\n",
                    ses_data->usecase);
            ret = E_FAILURE;
            goto error;
        }
        if (strcmp(loopback_profile_name, ses_data->loopback_config[LB_PROF_HFP_RX]->name) == 0) {
            ret = pal_stream_set_mute(btsco->rx_stream_handle, is_mute);
            if (!ret)
                btsco->rx_mute = is_mute;
        }
        else if (strcmp(loopback_profile_name, ses_data->loopback_config[LB_PROF_HFP_TX]->name) == 0) {
            ret = pal_stream_set_mute(btsco->tx_stream_handle, is_mute);
            if (!ret)
                btsco->tx_mute = is_mute;
        }
    }
    else {
        pa_log_error("Invalid usecase %s", ses_data->usecase);
        ret = E_FAILURE;
    }

error:
    dbus_error_free(&error);
    if (ret != E_SUCCESS) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set mute for %s failed!!\n",
                ses_data->usecase);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);

    return;
}

static void pa_pal_loopback_get_volume(DBusConnection *conn, DBusMessage *msg,
        void *userdata)
{
    double vol = 0.0;
    DBusError error;
    char *loopback_profile_name = NULL;
    pa_pal_loopback_ses_data_t *ses_data = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    ses_data = (pa_pal_loopback_ses_data_t *)userdata;
    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING,
                &loopback_profile_name, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("Get volume for usecase %s, lb_profile %s", ses_data->usecase,
            loopback_profile_name);

    if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_A2DP_SINK]) == 0) {
        if (!btsink) {
            pa_log_debug("%s connection is not active, ignoring get_volume call\n",
                    ses_data->usecase);
            goto error;
        }
        if (btsink->is_mute)
            vol = 0.0;
        else
            vol = btsink->volume;
    }
    else if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_SCO]) == 0) {
        if (!btsco) {
            pa_log_debug("%s connection is not active, ignoring get_volume call\n",
                    ses_data->usecase);
            goto error;
        }
        if (strcmp(loopback_profile_name, ses_data->loopback_config[LB_PROF_HFP_RX]->name) == 0) {
            if (btsco->rx_mute)
                vol = 0.0;
            else
                vol = btsco->rx_volume;
        }
        else if (strcmp(loopback_profile_name,
                    ses_data->loopback_config[LB_PROF_HFP_TX]->name) == 0) {
            if (btsco->tx_mute)
                vol = 0.0;
            else
                vol = btsco->tx_volume;
        }
    }
    else {
        pa_log_error("Invalid usecase %s", ses_data->usecase);
        goto error;
    }

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_DOUBLE, &vol);
    dbus_error_free(&error);

    return;

error:
    dbus_error_free(&error);
    pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get volume for %s failed!!\n",
            ses_data->usecase);

    return;
}

static void pa_pal_loopback_get_samplerate(DBusConnection *conn, DBusMessage *msg,
        void *userdata)
{
    DBusError error;
    unsigned int sample_rate = 0;
    pa_pal_loopback_ses_data_t *ses_data = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    ses_data = (pa_pal_loopback_ses_data_t *)userdata;

    pa_log_debug("Get volume for usecase %s\n", ses_data->usecase);

    if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_SCO]) == 0) {
        if (!btsco) {
            pa_log_debug("%s connection is not active, ignoring get_samplerate call\n",
                    ses_data->usecase);
            goto error;
        }
        sample_rate = btsco->sample_rate;
    }
    else {
        pa_log_error("Invalid usecase %s", ses_data->usecase);
        goto error;
    }

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_UINT32, &sample_rate);
    dbus_error_free(&error);

    return;

error:
    dbus_error_free(&error);
    pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get sampling rate for %s failed!!\n",
            ses_data->usecase);

    return;
}

static void pa_pal_loopback_destroy(DBusConnection *conn, DBusMessage *msg,
        void *userdata)
{
    int status = 0;
    char usecase[MAX_USECASE_NAME_LENGTH];
    DBusError error;
    pa_pal_loopback_ses_data_t *ses_data = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    ses_data = (pa_pal_loopback_ses_data_t *)userdata;

    if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_A2DP_SINK]) == 0) {
        if (btsink && btsink->is_running)
            status = stop_btsink(btsink);
        else {
            pa_log_debug("No %s session running\n", ses_data->usecase);
            goto error;
        }
    }
    else if (strcmp(ses_data->usecase, usecase_name_list[PA_PAL_UC_BT_SCO]) == 0) {
        if (btsco && btsco->is_running)
            status = stop_hfp(btsco);
        else {
            pa_log_debug("No %s session running\n", ses_data->usecase);
            goto error;
        }
    }
    else {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                "Invalid usecase name %s", usecase);
        goto error;
    }

    if (status) {
        pa_log_error("%s failed !!\n", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "%s for %s failed!!\n",
                __func__, ses_data->usecase);
    }

error:
    pa_dbus_send_empty_reply(conn, msg);
    dbus_error_free(&error);

    return;
}

/******* public functions ********/
int pa_pal_loopback_init(pa_core *core, pa_card *card,
        pa_hashmap *loopback_confs, void *prv_data, pa_module *m)
{
    pa_assert(core);
    pa_assert(card);
    pa_assert(m);
    pa_assert(loopback_confs);

    pa_pal_loopback_mdata_ptr = pa_xnew0(struct pa_pal_loopback_module_data, 1);

    pa_pal_loopback_mdata_ptr->dbus_path = pa_sprintf_malloc("%s/%s",
            PA_PAL_LOOPBACK_DBUS_OBJECT_PATH_PREFIX, "loopback");

    pa_pal_loopback_mdata_ptr->dbus_protocol = pa_dbus_protocol_get(core);

    pa_pal_loopback_mdata_ptr->card = card;
    pa_pal_loopback_mdata_ptr->m = m;
    pa_pal_loopback_mdata_ptr->prv_data = prv_data;
    pa_pal_loopback_mdata_ptr->loopback_confs = loopback_confs;
    pa_pal_loopback_mdata_ptr->session_count = 0;

    pa_pal_loopback_mdata_ptr->session_data =
        pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                NULL, NULL);
    if (!pa_pal_loopback_mdata_ptr->session_data) {
        pa_log_error("Allocation failed for session_data !!");
        return -ENOMEM;
    }

    pa_assert_se(pa_dbus_protocol_add_interface(pa_pal_loopback_mdata_ptr->dbus_protocol,
                pa_pal_loopback_mdata_ptr->dbus_path,
                &pa_pal_loopback_module_interface_info,
                pa_pal_loopback_mdata_ptr) >= 0);

    return E_SUCCESS;
}

void pa_pal_loopback_deinit(void)
{
    if (pa_pal_loopback_mdata_ptr) {
        if (pa_pal_loopback_mdata_ptr->dbus_path &&
                pa_pal_loopback_mdata_ptr->dbus_protocol)
            pa_assert_se(pa_dbus_protocol_remove_interface(pa_pal_loopback_mdata_ptr->dbus_protocol,
                        pa_pal_loopback_mdata_ptr->dbus_path,
                        pa_pal_loopback_module_interface_info.name) >= 0);

        if (pa_pal_loopback_mdata_ptr->dbus_path)
            pa_xfree(pa_pal_loopback_mdata_ptr->dbus_path);

        if (pa_pal_loopback_mdata_ptr->dbus_protocol)
            pa_dbus_protocol_unref(pa_pal_loopback_mdata_ptr->dbus_protocol);

        if (pa_pal_loopback_mdata_ptr->session_data)
            pa_hashmap_free(pa_pal_loopback_mdata_ptr->session_data);

        pa_xfree(pa_pal_loopback_mdata_ptr);
    }
}

