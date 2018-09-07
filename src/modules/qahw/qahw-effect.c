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

#include "qahw-sink.h"
#include "qahw-effect.h"
#include <qahw_effect_api.h>
#include <qahw_effect_bassboost.h>
#include <qahw_effect_virtualizer.h>
#include <qahw_effect_equalizer.h>
#include <qahw_effect_presetreverb.h>
#include <qahw_effect_audiosphere.h>

typedef struct {
    const char* lib_name;
    qahw_effect_uuid_t *uuid;
} pa_qahw_effect_info;

typedef struct effect_session_info{
    qahw_effect_lib_handle_t lib_handle;
    pa_qahw_effect_handle_t effect_handle;
    int client_count;
    uint32_t sink_id;
    char *dbus_obj_path;
    struct effect_session_info *next;
} pa_qahw_effect_session_info;

typedef struct {
    char *dbus_path;
    pa_dbus_protocol *dbus_protocol;
    pa_card *card;
    uint32_t max_supported_effects;
    uint32_t max_sinks;
    uint32_t max_ports;
    pa_qahw_effect_info *module_effects;
    pa_qahw_effect_data *sink_effects;
    pa_qahw_port_effect_data *port_effects;
    pa_qahw_effect_status *sink_effect_status;
    pa_qahw_effect_session_info *session_info;
} pa_qahw_effect_module_data;

typedef struct {
    pa_qahw_effect_module_data *common;
    qahw_effect_lib_handle_t lib_handle;
    pa_qahw_effect_handle_t effect_handle;
    char *dbus_obj_path;
    uint32_t sink_id;
    uint32_t effect_index;
} pa_qahw_effect_session_data;

static void pa_qahw_module_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_port_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_release(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_get_descriptor(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_get_version(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_command(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum module_handler_index {
    MODULE_HANDLER_GET_MODULE_SUPPORTED_EFFECTS,
    MODULE_HANDLER_SINK_SUPPORTED_EFFECTS,
    MODULE_HANDLER_PORT_SUPPORTED_EFFECTS,
    MODULE_HANDLER_EFFECT_CREATE,
    MODULE_HANDLER_EFFECT_GET_VERSION,
    MODULE_HANDLER_MAX
};

enum session_handler_index {
    SESSION_HANDLER_EFFECT_RELEASE,
    SESSION_HANDLER_EFFECT_GET_DESCRIPTOR,
    SESSION_HANDLER_EFFECT_COMMAND,
    SESSION_HANDLER_MAX
};

pa_dbus_arg_info module_supported_effects_args[] = {
    {"supported_effects", "u", "out"},
    {"descriptors", "a(((uqqqay)(uqqqay)uuqqayay))", "out"},
};

pa_dbus_arg_info sink_supported_effects_args[] = {
    {"sink_index", "u", "in"},
    {"supported_effects", "u", "out"},
    {"uuids", "a((uqqqay))", "out"},
};

pa_dbus_arg_info port_supported_effects_args[] = {
    {"port_name", "s", "in"},
    {"supported_effects", "u", "out"},
    {"device id", "u", "out"},
    {"uuids", "a((uqqqay))", "out"},
};

pa_dbus_arg_info effect_create_args[] = {
    {"uuid", "(uqqqay)", "in"},
    {"sink_index", "u", "in"},
    {"obj_path", "o", "out"},
};

pa_dbus_arg_info effect_release_args[] = {
};

pa_dbus_arg_info effect_get_descriptor_args[] = {
    {"uuid", "(uqqqay)", "in"},
    {"descriptor", "((uqqqay)(uqqqay)uuqqayay)", "out"},
};

pa_dbus_arg_info effect_get_version_args[] = {
    {"version", "i", "out"},
};

pa_dbus_arg_info effect_command_args[] = {
    {"command", "u", "in"},
    {"command_size", "u", "in"},
    {"command_data", "ay", "in"},
    {"data", "ay", "out"},
};

static pa_dbus_method_handler effect_module_handlers[MODULE_HANDLER_MAX] = {
    [MODULE_HANDLER_GET_MODULE_SUPPORTED_EFFECTS] = {
        .method_name = "GetModuleSupportedEffects",
        .arguments = module_supported_effects_args,
        .n_arguments = sizeof(module_supported_effects_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_module_get_supported_effects},
    [MODULE_HANDLER_SINK_SUPPORTED_EFFECTS] = {
        .method_name = "GetSinkSupportedEffects",
        .arguments = sink_supported_effects_args,
        .n_arguments = sizeof(sink_supported_effects_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_get_supported_effects},
    [MODULE_HANDLER_PORT_SUPPORTED_EFFECTS] = {
        .method_name = "GetPortSupportedEffects",
        .arguments = port_supported_effects_args,
        .n_arguments = sizeof(port_supported_effects_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_port_get_supported_effects},
    [MODULE_HANDLER_EFFECT_CREATE] = {
        .method_name = "Create",
        .arguments = effect_create_args,
        .n_arguments = sizeof(effect_create_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_effect_create},
    [MODULE_HANDLER_EFFECT_GET_VERSION] = {
        .method_name = "GetVersion",
        .arguments = effect_get_version_args,
        .n_arguments = sizeof(effect_get_version_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_effect_get_version},
};

static pa_dbus_method_handler effect_session_handlers[SESSION_HANDLER_MAX] = {
    [SESSION_HANDLER_EFFECT_RELEASE] = {
        .method_name = "Release",
        .arguments = effect_release_args,
        .n_arguments = sizeof(effect_release_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_effect_release},
    [SESSION_HANDLER_EFFECT_GET_DESCRIPTOR] = {
        .method_name = "GetDescriptor",
        .arguments = effect_get_descriptor_args,
        .n_arguments = sizeof(effect_get_descriptor_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_effect_get_descriptor},
    [SESSION_HANDLER_EFFECT_COMMAND] = {
        .method_name = "Command",
        .arguments = effect_command_args,
        .n_arguments = sizeof(effect_command_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_effect_command},
};

static pa_dbus_interface_info module_interface_info = {
    .name = QAHW_EFFECT_MODULE_IFACE,
    .method_handlers = effect_module_handlers,
    .n_method_handlers = MODULE_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

static pa_dbus_interface_info session_interface_info = {
    .name = QAHW_EFFECT_SESSION_IFACE,
    .method_handlers = effect_session_handlers,
    .n_method_handlers = SESSION_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

static void pa_qahw_fill_effect_uuid(DBusMessageIter *struct_i, qahw_effect_uuid_t uuid) {
    DBusMessageIter array_i;
    int i = 0;

    dbus_message_iter_append_basic(struct_i, DBUS_TYPE_UINT32, &uuid.timeLow);
    dbus_message_iter_append_basic(struct_i, DBUS_TYPE_UINT16, &uuid.timeMid);
    dbus_message_iter_append_basic(struct_i, DBUS_TYPE_UINT16, &uuid.timeHiAndVersion);
    dbus_message_iter_append_basic(struct_i, DBUS_TYPE_UINT16, &uuid.clockSeq);
    dbus_message_iter_open_container(struct_i, DBUS_TYPE_ARRAY, "y", &array_i);

    for (i = 0; i < 6; i++)
        dbus_message_iter_append_basic(&array_i, DBUS_TYPE_BYTE, &uuid.node[i]);

    dbus_message_iter_close_container(struct_i, &array_i);
}

static char *pa_qahw_get_obj_path(const char *dbus_path,
                                  uint32_t sink_id,
                                  uint32_t effect_index) {
    return pa_sprintf_malloc("%s/sink_%d/effect_%d", dbus_path, sink_id, effect_index);
}

static void pa_qahw_fill_module_effect_data(pa_qahw_effect_module_data *mdata) {
    pa_assert(mdata);

    mdata->module_effects[PA_QAHW_EFFECT_BASSBOOST].lib_name = QAHW_EFFECT_BASSBOOST_LIBRARY;
    mdata->module_effects[PA_QAHW_EFFECT_VIRTUALIZER].lib_name = QAHW_EFFECT_VIRTUALIZER_LIBRARY;
    mdata->module_effects[PA_QAHW_EFFECT_EQUALIZER].lib_name = QAHW_EFFECT_EQUALIZER_LIBRARY;
    mdata->module_effects[PA_QAHW_EFFECT_PRESET_REVERB].lib_name = QAHW_EFFECT_PRESET_REVERB_LIBRARY;
    mdata->module_effects[PA_QAHW_EFFECT_AUDIOSPHERE].lib_name = QAHW_EFFECT_AUDIOSPHERE_LIBRARY;

    memcpy(mdata->module_effects[PA_QAHW_EFFECT_BASSBOOST].uuid, SL_IID_BASSBOOST_UUID, sizeof(qahw_effect_uuid_t));
    memcpy(mdata->module_effects[PA_QAHW_EFFECT_VIRTUALIZER].uuid, SL_IID_VIRTUALIZER_UUID, sizeof(qahw_effect_uuid_t));
    memcpy(mdata->module_effects[PA_QAHW_EFFECT_EQUALIZER].uuid, SL_IID_EQUALIZER_UUID, sizeof(qahw_effect_uuid_t));
    memcpy(mdata->module_effects[PA_QAHW_EFFECT_PRESET_REVERB].uuid, SL_IID_INS_PRESETREVERB_UUID, sizeof(qahw_effect_uuid_t));
    memcpy(mdata->module_effects[PA_QAHW_EFFECT_AUDIOSPHERE].uuid, SL_IID_AUDIOSPHERE_UUID, sizeof(qahw_effect_uuid_t));
}

static int pa_qahw_update_client_count(pa_qahw_effect_module_data *mdata,
                                       char *dbus_obj_path, 
                                       int flag) {
    pa_qahw_effect_session_info *session_info = mdata->session_info;

    pa_log_debug("%s\n", __func__);

    while (session_info != NULL) {
        if (pa_strneq(dbus_obj_path, session_info->dbus_obj_path, strlen(dbus_obj_path))) {
            if (flag) {
                session_info->client_count++;
                return session_info->client_count;
            } else {
                session_info->client_count--;
                return session_info->client_count;
            }
        }
        session_info = session_info->next;
    }

    return -1;
}

static pa_qahw_effect_session_info *pa_qahw_effect_free_session_info(pa_qahw_effect_module_data *mdata,
                                                              char* dbus_obj_path) {
    pa_qahw_effect_session_info *session_info = mdata->session_info;
    pa_qahw_effect_session_info *temp_session_info = NULL;

    pa_log_debug("%s\n", __func__);

    if (session_info != NULL) {
        temp_session_info = session_info->next;
        if (pa_strneq(dbus_obj_path, session_info->dbus_obj_path, strlen(dbus_obj_path))) {
            pa_xfree(session_info);
            return temp_session_info;
        }
    }

    while (temp_session_info != NULL) {
        if (pa_strneq(dbus_obj_path, temp_session_info->dbus_obj_path, strlen(dbus_obj_path))) {
            session_info->next = temp_session_info->next;
            pa_xfree(temp_session_info);
            break;
        }
        session_info = session_info->next;
        temp_session_info = temp_session_info->next;
    }

    return mdata->session_info;
}

static pa_qahw_effect_session_info *pa_qahw_effect_update_session_info(pa_qahw_effect_session_info *session_info,
                                                                pa_qahw_effect_session_data *sdata) {

    pa_qahw_effect_session_info *temp_session_info = pa_xnew0(pa_qahw_effect_session_info, 1);

    pa_log_debug("%s\n", __func__);

    temp_session_info->lib_handle = sdata->lib_handle;
    temp_session_info->effect_handle = sdata->effect_handle;
    temp_session_info->sink_id = sdata->sink_id;
    temp_session_info->dbus_obj_path = sdata->dbus_obj_path;
    temp_session_info->next = session_info;

    return temp_session_info;
}

static void pa_qahw_effect_add_session_info(pa_qahw_effect_module_data *mdata,
                                     pa_qahw_effect_session_data *sdata) {
    pa_log_debug("%s\n", __func__);

    if (mdata->session_info == NULL) {
        mdata->session_info = (pa_qahw_effect_session_info *)pa_xnew0(pa_qahw_effect_session_info, 1);
        mdata->session_info->lib_handle = sdata->lib_handle;
        mdata->session_info->effect_handle = sdata->effect_handle;
        mdata->session_info->sink_id = sdata->sink_id;
        mdata->session_info->dbus_obj_path = sdata->dbus_obj_path;
        mdata->session_info->next = NULL;
    } else
        mdata->session_info = pa_qahw_effect_update_session_info(mdata->session_info, sdata);
}

void pa_qahw_free_sink_effects(pa_qahw_effect_handle_t effect_handle,
                               uint32_t sink_id) {
    pa_qahw_effect_module_data *effect_mdata = (pa_qahw_effect_module_data *)effect_handle;
    pa_qahw_effect_session_info *session_info = effect_mdata->session_info;
    pa_qahw_effect_session_info *temp_session_info = session_info;

    pa_log_debug("%s\n", __func__);

    while (temp_session_info != NULL) {
        if (temp_session_info->sink_id == sink_id) {
            qahw_effect_release(temp_session_info->lib_handle, temp_session_info->effect_handle);
            qahw_effect_unload_library(temp_session_info->lib_handle);
            pa_assert_se(pa_dbus_protocol_remove_interface(effect_mdata->dbus_protocol,
                                temp_session_info->dbus_obj_path, session_interface_info.name) >= 0);
            if (temp_session_info == effect_mdata->session_info)
                effect_mdata->session_info = temp_session_info->next;
            session_info->next = temp_session_info->next;
            pa_xfree(temp_session_info);
        }
        session_info = temp_session_info;
        temp_session_info = temp_session_info->next;
    }
}

static void pa_qahw_effect_get_version(DBusConnection *conn,
                                DBusMessage *msg,
                                void *userdata) {
    int version;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    version = qahw_effect_get_version();
    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_INT32, &version);
}

static void pa_qahw_effect_command(DBusConnection *conn,
                            DBusMessage *msg,
                            void *userdata) {
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;
    DBusError error;
    int n_elements = 0;
    DBusMessageIter arg_i, array_i;
    uint32_t cmd_code;
    uint32_t cmd_size;
    void *cmd_data = NULL;
    uint32_t reply_size;
    void *reply_data = NULL;
    int rc = -1;
    DBusMessage *reply = NULL;
    DBusMessageIter r_arg, r_array_i;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("effect_command has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "effect_command has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "uuay")) {
        pa_log_error("Invalid signature for effect_command.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for effect_command.");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_get_basic(&arg_i, &cmd_code);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &cmd_size);
    dbus_message_iter_next(&arg_i);
    cmd_data = (void *)malloc(cmd_size);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, &cmd_data, &n_elements);

    switch (cmd_code) {
        case QAHW_EFFECT_CMD_INIT:
        case QAHW_EFFECT_CMD_SET_CONFIG:
        case QAHW_EFFECT_CMD_ENABLE:
        case QAHW_EFFECT_CMD_DISABLE:
        case QAHW_EFFECT_CMD_OFFLOAD:
        case QAHW_EFFECT_CMD_SET_PARAM:
            reply_size = sizeof(int32_t);
            break;
        case QAHW_EFFECT_CMD_RESET:
        case QAHW_EFFECT_CMD_SET_DEVICE:
        case QAHW_EFFECT_CMD_SET_AUDIO_MODE:
        case QAHW_EFFECT_CMD_SET_VOLUME:
            reply_size = 0;
            break;
        case QAHW_EFFECT_CMD_GET_CONFIG:
            reply_size = sizeof(qahw_effect_config_t);
            break;
        case QAHW_EFFECT_CMD_GET_PARAM:
            reply_size = sizeof(qahw_effect_param_t) + sizeof(uint32_t) + sizeof(uint16_t);
            break;
        default:
            pa_log_error("Invalid command \n");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid effect command.");
            dbus_error_free(&error);
            return;
    }

    reply_data = (void *)malloc(reply_size);

    rc = qahw_effect_command(ses_data->effect_handle, cmd_code, cmd_size, cmd_data, &reply_size, (void *)reply_data);
    if (rc != 0) {
        pa_log_error("effect_command returns : %d\n", rc);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_command failed.");
        dbus_error_free(&error);
        return;
    }
    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_ARRAY, "y", &r_array_i);
    dbus_message_iter_append_fixed_array(&r_array_i, DBUS_TYPE_BYTE, &reply_data, n_elements);
    dbus_message_iter_close_container(&r_arg, &r_array_i);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_error_free(&error);
    dbus_message_unref(reply);
}

static void pa_qahw_effect_get_descriptor(DBusConnection *conn,
                                   DBusMessage *msg,
                                   void *userdata) {
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;
    DBusError error;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, struct_i, array_i;
    DBusMessageIter r_arg;
    DBusMessageIter r_array_i, r_array_ii;
    DBusMessageIter r_struct_i, r_struct_ii, r_struct_iii;
    qahw_effect_uuid_t uuid;
    qahw_effect_descriptor_t effect_desc;
    int n_elements = 0;
    unsigned int i = 0;
    int rc = -1;
    char *value = NULL;
    char **addr_value = &value;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("effect_get_descriptor has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "effect_get_descriptor has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(uqqqay)")) {
        pa_log_error("Invalid signature for effect_get_descriptor.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for effect_get_descriptor.");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &uuid.timeLow);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &uuid.timeMid);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &uuid.timeHiAndVersion);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &uuid.clockSeq);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    memcpy(&uuid.node[0], value, n_elements);

    rc = qahw_effect_get_descriptor(ses_data->lib_handle, &uuid, &effect_desc);
    if (rc != 0) {
        pa_log_error("effect_get_descriptor returns : %d\n", rc);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_get_descriptor failed.");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_STRUCT, NULL, &r_struct_i);

    dbus_message_iter_open_container(&r_struct_i, DBUS_TYPE_STRUCT, NULL, &r_struct_ii);
    pa_qahw_fill_effect_uuid(&r_struct_ii, effect_desc.type);
    dbus_message_iter_close_container(&r_struct_i, &r_struct_ii);

    dbus_message_iter_open_container(&r_struct_i, DBUS_TYPE_STRUCT, NULL, &r_struct_iii);
    pa_qahw_fill_effect_uuid(&r_struct_iii, effect_desc.uuid);
    dbus_message_iter_close_container(&r_struct_i, &r_struct_iii);

    dbus_message_iter_append_basic(&r_struct_i, DBUS_TYPE_UINT32, &effect_desc.apiVersion);
    dbus_message_iter_append_basic(&r_struct_i, DBUS_TYPE_UINT32, &effect_desc.flags);
    dbus_message_iter_append_basic(&r_struct_i, DBUS_TYPE_UINT16, &effect_desc.cpuLoad);
    dbus_message_iter_append_basic(&r_struct_i, DBUS_TYPE_UINT16, &effect_desc.memoryUsage);

    dbus_message_iter_open_container(&r_struct_i, DBUS_TYPE_ARRAY, "y", &r_array_i);

    for (i = 0; i < strlen(effect_desc.name); i++)
        dbus_message_iter_append_basic(&r_array_i, DBUS_TYPE_BYTE, &effect_desc.name[i]);

    dbus_message_iter_append_basic(&r_array_i, DBUS_TYPE_BYTE, &effect_desc.name[i]);
    dbus_message_iter_close_container(&r_struct_i, &r_array_i);

    dbus_message_iter_open_container(&r_struct_i, DBUS_TYPE_ARRAY, "y", &r_array_ii);

    for (i = 0; i < strlen(effect_desc.implementor); i++)
        dbus_message_iter_append_basic(&r_array_ii, DBUS_TYPE_BYTE, &effect_desc.implementor[i]);

    dbus_message_iter_append_basic(&r_array_ii, DBUS_TYPE_BYTE, &effect_desc.implementor[i]);
    dbus_message_iter_close_container(&r_struct_i, &r_array_ii);
    dbus_message_iter_close_container(&r_arg, &r_struct_i);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_error_free(&error);
    dbus_message_unref(reply);
}

static void pa_qahw_effect_release(DBusConnection *conn,
                            DBusMessage *msg,
                            void *userdata) {
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;
    int rc = -1;
    pa_qahw_effect_status *sink_effect_status = ses_data->common->sink_effect_status;
    int client_count = -1;
    uint32_t i = 0;
    uint32_t num_sinks = ses_data->common->max_sinks;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    client_count = pa_qahw_update_client_count(ses_data->common, ses_data->dbus_obj_path, 0);
    if (client_count == 0) {
        rc = qahw_effect_release(ses_data->lib_handle, ses_data->effect_handle);
        if (rc != 0) {
            pa_log_error("qahw_effect_release returns :%d\n", rc);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_release failed.");
            return;
        }

        rc = qahw_effect_unload_library(ses_data->lib_handle);
        if (rc != 0) {
            pa_log_error("qahw_effect_unload_library returns :%d\n", rc);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_unload_library failed.");
            return;
        }

        for (i = 0; i < num_sinks; i++) {
            if(sink_effect_status[i].sink_id == ses_data->sink_id) {
                sink_effect_status[i].effect_loaded[ses_data->effect_index] = false;
                break;
            }
        }

        ses_data->common->session_info = pa_qahw_effect_free_session_info(ses_data->common, ses_data->dbus_obj_path);
        pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol,
                ses_data->dbus_obj_path, session_interface_info.name) >= 0);
        pa_xfree(ses_data->dbus_obj_path);
        pa_xfree(ses_data);
        pa_dbus_send_empty_reply(conn, msg);
    }
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_effect_create(DBusConnection *conn,
                           DBusMessage *msg,
                           void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    pa_qahw_effect_session_data *ses_data = NULL;
    DBusError error;
    qahw_effect_uuid_t uuid;
    audio_io_handle_t io_handle;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, struct_i;
    uint32_t i = 0;
    int rc = -1;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    pa_qahw_effect_status *sink_effect_status = m_data->sink_effect_status;
    uint32_t sink_id;
    uint32_t effect_index;
    char *obj_path;
    uint32_t num_sinks = m_data->max_sinks;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("effect_create has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "effect_create has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(uqqqay)u")) {
        pa_log_error("Invalid signature for effect_create.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for effect_create.");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &uuid.timeLow);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &uuid.timeMid);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &uuid.timeHiAndVersion);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &uuid.clockSeq);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    memcpy(&uuid.node[0], value, n_elements);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &sink_id);

    for (i = 0; i < m_data->max_supported_effects; i++) {
        if (memcmp(&uuid, m_data->module_effects[i].uuid, sizeof(qahw_effect_uuid_t)) == 0) {
            effect_index = i;
            break;
        }
    }

    if (i == m_data->max_supported_effects) {
        pa_log_error("Unsupported UUID.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported UUID.");
        dbus_error_free(&error);
        return;
    }

    for (i = 0; i < num_sinks; i++) {
        if (sink_effect_status[i].sink_id == sink_id) {
            if (!sink_effect_status[i].effect_loaded[effect_index]) {
                ses_data = pa_xnew0(pa_qahw_effect_session_data, 1);
                ses_data->lib_handle = qahw_effect_load_library(m_data->module_effects[effect_index].lib_name);
                ses_data->effect_index = effect_index;
                ses_data->sink_id = sink_id;
                io_handle = pa_qahw_sink_get_io_handle(sink_effect_status[i].handle);
                rc = qahw_effect_create(ses_data->lib_handle, &uuid, io_handle, &ses_data->effect_handle);
                if (rc != 0) {
                    pa_log_error("EffectCreate failed\n");
                    pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_create failed.");
                    dbus_error_free(&error);
                    return;
                }
                ses_data->dbus_obj_path = pa_qahw_get_obj_path(m_data->dbus_path, ses_data->sink_id, ses_data->effect_index);
                ses_data->common = m_data;
                pa_qahw_effect_add_session_info(m_data, ses_data);
                m_data->session_info->client_count = 0;
                pa_qahw_update_client_count(m_data, ses_data->dbus_obj_path, 1);
                sink_effect_status[i].effect_loaded[ses_data->effect_index] = true;
                pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol,
                        ses_data->dbus_obj_path, &session_interface_info, ses_data) >= 0);
                pa_assert_se((reply = dbus_message_new_method_return(msg)));
                dbus_message_iter_init_append(reply, &arg_i);
                dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &ses_data->dbus_obj_path);
                pa_assert_se(dbus_connection_send(conn, reply, NULL));
                dbus_message_unref(reply);
            } else {
                pa_qahw_update_client_count(m_data, pa_qahw_get_obj_path(m_data->dbus_path, sink_id, effect_index), 1);
                obj_path = pa_qahw_get_obj_path(m_data->dbus_path, sink_id, effect_index);
                pa_assert_se((reply = dbus_message_new_method_return(msg)));
                dbus_message_iter_init_append(reply, &arg_i);
                dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &obj_path);
                pa_assert_se(dbus_connection_send(conn, reply, NULL));
                dbus_error_free(&error);
                dbus_message_unref(reply);
                return;
            }
            break;
        }
    }

    if (i == num_sinks) {
        pa_log_error("Invalid sink id\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid sink id.");
        dbus_error_free(&error);
        return;
    }
}

static void pa_qahw_port_get_supported_effects(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    DBusError error;
    DBusMessageIter arg_i;
    DBusMessage *reply = NULL;
    DBusMessageIter arg, array_i, struct_i;
    char *port_name = NULL;
    int port_id = -1;
    uint32_t i = 0, j = 0;
    uint32_t port_effects = 0;
    uint32_t num_ports = m_data->max_ports;
    pa_device_port *p;
    audio_devices_t *audio_device;

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("port_get_supported_effects has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "port_get_supported_effects has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "s")) {
        pa_log_error("Invalid signature for port_get_supported_effects.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for port_get_supported_effects.");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_get_basic(&arg_i, &port_name);
    for (i = 0; i < num_ports; i++) {
        if (pa_streq(port_name, m_data->port_effects[i].port_name)) {
            for (j = 0; j < m_data->max_supported_effects; j++) {
                if (m_data->port_effects[i].effect_supported[j])
                    port_effects++;
            }
            port_id = i;
            break;
        }
    }

    if (port_id == -1) {
        pa_log_error("Invalid port name %s", port_name);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid port name.");
        dbus_error_free(&error);
        return;
    } else {
        p = pa_hashmap_get(m_data->card->ports, port_name);
        audio_device = PA_DEVICE_PORT_DATA(p);
        pa_assert(audio_device);

        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg);
        dbus_message_iter_append_basic(&arg, DBUS_TYPE_UINT32, &port_effects);
        dbus_message_iter_append_basic(&arg, DBUS_TYPE_UINT32, audio_device);
        dbus_message_iter_open_container(&arg, DBUS_TYPE_ARRAY, "(uqqqay)", &array_i);

        for (i = 0; i < m_data->max_supported_effects; i++) {
            if (m_data->port_effects[port_id].effect_supported[i]) {
                dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
                pa_qahw_fill_effect_uuid(&struct_i, *(m_data->module_effects[i].uuid));
                dbus_message_iter_close_container(&array_i, &struct_i);
            }
        }
        dbus_message_iter_close_container(&arg, &array_i);
    }

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_error_free(&error);
    dbus_message_unref(reply);

}

static void pa_qahw_sink_get_supported_effects(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    DBusError error;
    DBusMessageIter arg_i;
    DBusMessage *reply = NULL;
    DBusMessageIter arg, array_i, struct_i;
    uint32_t sink_id;
    uint32_t i = 0, j = 0, k = 0;
    uint32_t sink_effects = 0;
    pa_qahw_effect_status *sink_effect_status = m_data->sink_effect_status;
    uint32_t num_sinks = m_data->max_sinks;

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("sink_get_supported_efffects has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "sink_get_supported_efffects has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "u")) {
        pa_log_error("Invalid signature for sink_get_supported_efffects.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for sink_get_supported_efffects.");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_get_basic(&arg_i, &sink_id);

    for (i = 0; i < num_sinks; i++) {
        if(sink_effect_status[i].sink_id == sink_id)
            break;
    }

    for (j = 0; j < num_sinks; j++) {
        if(pa_qahw_sink_get_flags(sink_effect_status[i].handle) == m_data->sink_effects[j].flags)
            break;
    }

    for (k = 0; k < m_data->max_supported_effects; k++) {
        if (m_data->sink_effects[j].effect_supported[k])
            sink_effects++;
    }

    if (i == num_sinks) {
        pa_log_error("Invalid sink index.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid sink index.");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg);
    dbus_message_iter_append_basic(&arg, DBUS_TYPE_UINT32, &sink_effects);
    dbus_message_iter_open_container(&arg, DBUS_TYPE_ARRAY, "(uqqqay)", &array_i);

    for (j = 0; j < m_data->max_supported_effects; j++) {
        if (m_data->sink_effects[i].effect_supported[j]) {
            dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
            pa_qahw_fill_effect_uuid(&struct_i, *(m_data->module_effects[j].uuid));
            dbus_message_iter_close_container(&array_i, &struct_i);
        }
    }

    dbus_message_iter_close_container(&arg, &array_i);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_error_free(&error);
    dbus_message_unref(reply);
}

static void pa_qahw_module_get_supported_effects(DBusConnection *conn,
                                          DBusMessage *msg,
                                          void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    DBusMessage *reply = NULL;
    DBusMessageIter arg;
    DBusMessageIter array_i, array_iii, array_iv;
    DBusMessageIter struct_i, struct_ii, struct_iii;
    uint32_t i = 0, j = 0;
    qahw_effect_descriptor_t effect_descriptors[m_data->max_supported_effects];
    qahw_effect_lib_handle_t lib_handle;
    int rc = -1;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    for (i = 0; i < m_data->max_supported_effects; i++) {
        lib_handle = qahw_effect_load_library(m_data->module_effects[i].lib_name);
        rc = qahw_effect_get_descriptor(lib_handle, m_data->module_effects[i].uuid, &effect_descriptors[i]);
        pa_log_info("qahw_effect_get_descriptor() returns %d\n", rc);
        rc = qahw_effect_unload_library(lib_handle);
        pa_log_info("qahw_effect_unload_library() returns %d\n", rc);
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg);
    dbus_message_iter_append_basic(&arg, DBUS_TYPE_UINT32, &m_data->max_supported_effects);
    dbus_message_iter_open_container(&arg, DBUS_TYPE_ARRAY, "((uqqqay)(uqqqay)uuqqayay)", &array_i);

    for (i = 0; i < m_data->max_supported_effects; i++) {
        dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_STRUCT, NULL, &struct_ii);
        pa_qahw_fill_effect_uuid(&struct_ii, effect_descriptors[i].type);
        dbus_message_iter_close_container(&struct_i, &struct_ii);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_STRUCT, NULL, &struct_iii);
        pa_qahw_fill_effect_uuid(&struct_iii, effect_descriptors[i].uuid);
        dbus_message_iter_close_container(&struct_i, &struct_iii);

        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &effect_descriptors[i].apiVersion);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &effect_descriptors[i].flags);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT16, &effect_descriptors[i].cpuLoad);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT16, &effect_descriptors[i].memoryUsage);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y", &array_iii);
        for (j = 0; j < strlen(effect_descriptors[i].name); j++)
            dbus_message_iter_append_basic(&array_iii, DBUS_TYPE_BYTE, &effect_descriptors[i].name[j]);
        dbus_message_iter_append_basic(&array_iii, DBUS_TYPE_BYTE, &effect_descriptors[i].name[j]);
        dbus_message_iter_close_container(&struct_i, &array_iii);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y", &array_iv);
        for (j = 0; j < strlen(effect_descriptors[i].implementor); j++)
            dbus_message_iter_append_basic(&array_iv, DBUS_TYPE_BYTE, &effect_descriptors[i].implementor[j]);
        dbus_message_iter_append_basic(&array_iv, DBUS_TYPE_BYTE, &effect_descriptors[i].implementor[j]);
        dbus_message_iter_close_container(&struct_i, &array_iv);

        dbus_message_iter_close_container(&array_i, &struct_i);
    }
    dbus_message_iter_close_container(&arg, &array_i);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

pa_qahw_effect_handle_t pa_qahw_init_effect(char *dbus_path,
                                            pa_dbus_protocol *dbus_protocol,
                                            pa_qahw_effect_data *sink_effects,
                                            pa_qahw_port_effect_data *port_effects,
                                            pa_qahw_effect_status *effect_status,
                                            pa_card *card,
                                            uint32_t max_sinks,
                                            uint32_t max_ports) {
    pa_qahw_effect_module_data *effect_mdata = pa_xnew0(pa_qahw_effect_module_data, 1);
    uint32_t i = 0;

    pa_log_debug("%s\n", __func__);

    pa_assert(dbus_protocol);
    pa_assert(sink_effects);
    pa_assert(port_effects);
    pa_assert(effect_status);
    pa_assert(card);

    effect_mdata->dbus_path = dbus_path;
    effect_mdata->dbus_protocol = dbus_protocol;
    effect_mdata->max_supported_effects = PA_QAHW_EFFECT_MAX;
    effect_mdata->sink_effects = sink_effects;
    effect_mdata->port_effects = port_effects;
    effect_mdata->sink_effect_status = effect_status;
    effect_mdata->card = card;
    effect_mdata->max_sinks = max_sinks;
    effect_mdata->max_ports = max_ports;

    pa_assert_se(pa_dbus_protocol_add_interface(effect_mdata->dbus_protocol,
                 effect_mdata->dbus_path, &module_interface_info, effect_mdata) >= 0);

    pa_log_info("Interface is added for object %s\n", effect_mdata->dbus_path);

    effect_mdata->module_effects = pa_xnew0(pa_qahw_effect_info, effect_mdata->max_supported_effects);

    for (i = 0; i < effect_mdata->max_supported_effects; i++)
        effect_mdata->module_effects[i].uuid = pa_xnew0(qahw_effect_uuid_t, 1);

    pa_qahw_fill_module_effect_data(effect_mdata);
    return (pa_qahw_effect_handle_t)effect_mdata;
}

void pa_qahw_deinit_effect(pa_qahw_effect_handle_t effect_handle) {
    pa_qahw_effect_module_data *effect_mdata = (pa_qahw_effect_module_data *)effect_handle;
    uint32_t i = 0;

    pa_log_debug("%s\n", __func__);

    pa_assert(effect_mdata);

    for (i = 0; i < effect_mdata->max_supported_effects; i++) {
        if (!effect_mdata->module_effects[i].uuid)
            pa_xfree(effect_mdata->module_effects[i].uuid);
    }

    pa_xfree(effect_mdata->module_effects);

    if (effect_mdata->dbus_path && effect_mdata->dbus_protocol) {
        pa_assert_se(pa_dbus_protocol_remove_interface(effect_mdata->dbus_protocol,
                        effect_mdata->dbus_path, module_interface_info.name) >= 0);
        pa_dbus_protocol_unref(effect_mdata->dbus_protocol);
        pa_xfree(effect_mdata->dbus_path);
    }

    pa_xfree(effect_mdata);
}
