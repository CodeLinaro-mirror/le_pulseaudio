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

#include "qahw-sink.h"
#include "qahw-utils.h"
#include "qahw-effect.h"
#include <qahw_effect_api.h>
#include <qahw_effect_bassboost.h>
#include <qahw_effect_virtualizer.h>
#include <qahw_effect_equalizer.h>
#include <qahw_effect_presetreverb.h>
#include <qahw_effect_audiosphere.h>

static bool module_intialized = false;

typedef struct {
    char *dbus_obj_path;
    pa_dbus_protocol *dbus_protocol;
    pa_card *card;
    uint32_t max_supported_effects;
    pa_hashmap *effects;
    pa_hashmap *sessions;
} pa_qahw_effect_module_data;

typedef struct {
    char *endpoint_name;
    pa_hashmap *endpoints;
} pa_qahw_effect_session_data;

typedef struct {
    char *dbus_obj_path;
    pa_dbus_protocol *dbus_protocol;
    qahw_effect_lib_handle_t lib_handle;
    pa_qahw_effect_handle_t effect_handle;
    int client_count;
    char *type;
    pa_hashmap *sessions;
} pa_qahw_effect_endpoint_info;

typedef struct {
    char *name;
    char *description;
    char *type;
    int index;
    qahw_effect_uuid_t *uuid;
    char* lib_name;
    pa_hashmap *sinks;
    pa_hashmap *ports;
} pa_qahw_effect_info;

typedef struct {
    const char *effect_name;
    const char *lib_name;
} pa_qahw_effect_name_to_lib_mapping;

static void pa_qahw_module_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_port_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_effect_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_port_effect_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_effect_release(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_port_effect_release(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_get_descriptor(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_get_version(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_command(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum module_handler_index {
    MODULE_HANDLER_GET_MODULE_SUPPORTED_EFFECTS,
    MODULE_HANDLER_SINK_SUPPORTED_EFFECTS,
    MODULE_HANDLER_PORT_SUPPORTED_EFFECTS,
    MODULE_HANDLER_SINK_EFFECT_CREATE,
    MODULE_HANDLER_PORT_EFFECT_CREATE,
    MODULE_HANDLER_EFFECT_GET_VERSION,
    MODULE_HANDLER_MAX
};

enum session_handler_index {
    SESSION_HANDLER_SINK_EFFECT_RELEASE,
    SESSION_HANDLER_PORT_EFFECT_RELEASE,
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

pa_dbus_arg_info sink_effect_create_args[] = {
    {"uuid", "(uqqqay)", "in"},
    {"sink_index", "u", "in"},
    {"obj_path", "o", "out"},
};

pa_dbus_arg_info port_effect_create_args[] = {
    {"uuid", "(uqqqay)", "in"},
    {"port_name", "s", "in"},
    {"obj_path", "o", "out"},
};

pa_dbus_arg_info sink_effect_release_args[] = {
};

pa_dbus_arg_info port_effect_release_args[] = {
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
    [MODULE_HANDLER_SINK_EFFECT_CREATE] = {
        .method_name = "SinkEffectCreate",
        .arguments = sink_effect_create_args,
        .n_arguments = sizeof(sink_effect_create_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_effect_create},
    [MODULE_HANDLER_PORT_EFFECT_CREATE] = {
        .method_name = "PortEffectCreate",
        .arguments = port_effect_create_args,
        .n_arguments = sizeof(port_effect_create_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_port_effect_create},
    [MODULE_HANDLER_EFFECT_GET_VERSION] = {
        .method_name = "GetVersion",
        .arguments = effect_get_version_args,
        .n_arguments = sizeof(effect_get_version_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_effect_get_version},
};

static pa_dbus_method_handler effect_session_handlers[SESSION_HANDLER_MAX] = {
    [SESSION_HANDLER_SINK_EFFECT_RELEASE] = {
        .method_name = "SinkEffectRelease",
        .arguments = sink_effect_release_args,
        .n_arguments = sizeof(sink_effect_release_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_sink_effect_release},
    [SESSION_HANDLER_PORT_EFFECT_RELEASE] = {
        .method_name = "PortEffectRelease",
        .arguments = port_effect_release_args,
        .n_arguments = sizeof(port_effect_release_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_port_effect_release},
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

pa_qahw_effect_name_to_lib_mapping effect_lib_mapping[] = {
    { "bassboost", QAHW_EFFECT_BASSBOOST_LIBRARY},
    { "virtualizer", QAHW_EFFECT_VIRTUALIZER_LIBRARY},
    { "equalizer", QAHW_EFFECT_EQUALIZER_LIBRARY},
    { "preset_reverb", QAHW_EFFECT_PRESET_REVERB_LIBRARY},
    { "audiosphere", QAHW_EFFECT_AUDIOSPHERE_LIBRARY},
};

static char *pa_qahw_get_effect_lib_name(char *effect_name) {
    uint32_t i;

    pa_assert(effect_name);

    for (i = 0; i < ARRAY_SIZE(effect_lib_mapping); i++) {
        if (pa_streq(effect_lib_mapping[i].effect_name, effect_name))
            return (char *)effect_lib_mapping[i].lib_name;
    }

    return NULL;
}

static int pa_qahw_get_effect_uuid(char *effect_name, qahw_effect_uuid_t *uuid) {
    int rc = 0;;

    pa_assert(effect_name);

    if (pa_streq(effect_name, "bassboost"))
        memcpy(uuid, SL_IID_BASSBOOST_UUID, sizeof(qahw_effect_uuid_t));
    else if (pa_streq(effect_name, "virtualizer"))
        memcpy(uuid, SL_IID_VIRTUALIZER_UUID, sizeof(qahw_effect_uuid_t));
    else if (pa_streq(effect_name, "equalizer"))
        memcpy(uuid, SL_IID_EQUALIZER_UUID, sizeof(qahw_effect_uuid_t));
    else if (pa_streq(effect_name, "preset_reverb"))
        memcpy(uuid, SL_IID_INS_PRESETREVERB_UUID, sizeof(qahw_effect_uuid_t));
    else if (pa_streq(effect_name, "audiosphere"))
        memcpy(uuid, SL_IID_AUDIOSPHERE_UUID, sizeof(qahw_effect_uuid_t));
    else
        rc = -1;

    return rc;
}

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

static char *pa_qahw_get_obj_path(const char *dbus_obj_path,
                                  const char *sink_name,
                                  uint32_t effect_index) {
    return pa_sprintf_malloc("%s/sink_%s/effect_%d", dbus_obj_path, sink_name, effect_index);
}

void pa_qahw_free_sink_effects(pa_qahw_effect_handle_t effect_handle,
                               uint32_t sink_id) {
    pa_qahw_effect_module_data *effect_mdata = (pa_qahw_effect_module_data *)effect_handle;
    pa_qahw_effect_info *effect;
    pa_qahw_effect_endpoint_info *sink = NULL;

    char *sink_name;
    void *state;

    pa_log_debug("%s\n", __func__);

    sink_name = pa_qahw_sink_get_name_from_pa_sink_id(sink_id);

    if (effect_mdata->effects != NULL) {
        PA_HASHMAP_FOREACH(effect, effect_mdata->effects, state) {
            sink = pa_hashmap_get(effect->sinks, sink_name);
            if (sink != NULL) {
                if ((sink->lib_handle != NULL) && (sink->effect_handle != NULL)) {
                    qahw_effect_release(sink->lib_handle, sink->effect_handle);
                    qahw_effect_unload_library(sink->lib_handle);
                }

                sink->client_count = 0;
                sink->lib_handle = NULL;
                sink->effect_handle = NULL;

                if (sink->type != NULL) {
                    pa_xfree(sink->type);
                    sink->type = NULL;
                }

                if (sink->dbus_obj_path != NULL) {
                    pa_assert_se(pa_dbus_protocol_remove_interface(effect_mdata->dbus_protocol,
                                sink->dbus_obj_path, session_interface_info.name) >= 0);
                    pa_xfree(sink->dbus_obj_path);
                    sink->dbus_obj_path = NULL;
                }
                pa_xfree(sink);
                sink = NULL;
                pa_hashmap_remove(effect->sinks, sink_name);
            } else {
                pa_log_debug("%s: Effect is not loaded on sink %s\n", __func__, sink_name);
            }
        }
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
    pa_qahw_effect_endpoint_info *endpoint = NULL;
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

    endpoint = pa_hashmap_get(ses_data->endpoints, ses_data->endpoint_name);

    if (!endpoint) {
        pa_log_error("%s: Error in getting endpoint\n", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Error in getting endpoint.");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_get_basic(&arg_i, &cmd_code);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &cmd_size);
    dbus_message_iter_next(&arg_i);
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

    rc = qahw_effect_command(endpoint->effect_handle, cmd_code, cmd_size, cmd_data, &reply_size,
                             reply_data);

    if (rc != 0) {
        if (reply_data)
            free(reply_data);
        pa_log_error("effect_command returns : %d\n", rc);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_command failed.");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_ARRAY, "y", &r_array_i);
    dbus_message_iter_append_fixed_array(&r_array_i, DBUS_TYPE_BYTE, &reply_data, reply_size);
    dbus_message_iter_close_container(&r_arg, &r_array_i);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_error_free(&error);
    dbus_message_unref(reply);

    if (reply_data) {
        free(reply_data);
    }
}

static void pa_qahw_effect_get_descriptor(DBusConnection *conn,
                                          DBusMessage *msg,
                                          void *userdata) {
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;
    pa_qahw_effect_endpoint_info *endpoint = NULL;
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
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for effect_get_descriptor.");
        dbus_error_free(&error);
        return;
    }

    endpoint = pa_hashmap_get(ses_data->endpoints, ses_data->endpoint_name);

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

    if (endpoint) {
        rc = qahw_effect_get_descriptor(endpoint->lib_handle, &uuid, &effect_desc);
    } else {
        pa_log_error("%s: Error in getting endpoint\n", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Error in getting endpoint.");
        dbus_error_free(&error);
        return;
    }

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

static void pa_qahw_port_effect_release(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    int rc = -1;
    pa_qahw_effect_endpoint_info *port = NULL;
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;


    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    port = pa_hashmap_get(ses_data->endpoints, ses_data->endpoint_name);

    if (port) {
        port->client_count--;
    } else {
        pa_log_error("%s: Error in getting endpoint\n", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Error in getting endpoint.");
        return;
    }

    if (port->client_count == 0) {
        rc = qahw_effect_release(port->lib_handle, port->effect_handle);

        if (rc != 0) {
            pa_log_error("qahw_effect_release returns :%d\n", rc);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_release failed.");
            return;
        }

        rc = qahw_effect_unload_library(port->lib_handle);

        if (rc != 0) {
            pa_log_error("qahw_effect_unload_library returns :%d\n", rc);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_unload_library failed.");
            return;
        }

        pa_assert_se(pa_dbus_protocol_remove_interface(port->dbus_protocol,
                     port->dbus_obj_path, session_interface_info.name) >= 0);
        pa_dbus_send_empty_reply(conn, msg);
        port->effect_handle = NULL;
        port->lib_handle = NULL;
        pa_xfree(ses_data->endpoint_name);
        ses_data->endpoint_name = NULL;
        pa_xfree(ses_data);
        ses_data = NULL;
        pa_hashmap_remove(port->sessions, port->dbus_obj_path);
        pa_xfree(port->dbus_obj_path);
        port->dbus_obj_path = NULL;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_effect_release(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    int rc = -1;

    pa_qahw_effect_endpoint_info *sink = NULL;
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    sink = pa_hashmap_get(ses_data->endpoints, ses_data->endpoint_name);

    if (sink) {
        sink->client_count--;
    } else {
        pa_log_error("%s: Error in getting endpoint\n", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Error in getting endpoint.");
        return;
    }

    if (sink->client_count == 0) {
        rc = qahw_effect_release(sink->lib_handle, sink->effect_handle);

        if (rc != 0) {
            pa_log_error("qahw_effect_release returns :%d\n", rc);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_release failed.");
            return;
        }

        rc = qahw_effect_unload_library(sink->lib_handle);

        if (rc != 0) {
            pa_log_error("qahw_effect_unload_library returns :%d\n", rc);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_unload_library failed.");
            return;
        }

        pa_assert_se(pa_dbus_protocol_remove_interface(sink->dbus_protocol,
                     sink->dbus_obj_path, session_interface_info.name) >= 0);
        pa_dbus_send_empty_reply(conn, msg);
        sink->effect_handle = NULL;
        sink->lib_handle = NULL;
        pa_xfree(ses_data->endpoint_name);
        ses_data->endpoint_name = NULL;
        pa_xfree(ses_data);
        ses_data = NULL;
        pa_hashmap_remove(sink->sessions, sink->dbus_obj_path);
        pa_xfree(sink->dbus_obj_path);
        sink->dbus_obj_path = NULL;
    }
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_port_effect_create(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    pa_qahw_effect_session_data *ses_data = NULL;
    DBusError error;
    qahw_effect_uuid_t uuid;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, struct_i;
    int rc = -1;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    char *port_name = NULL;
    char *obj_path;
    void *state;
    pa_qahw_effect_info *effect = NULL;
    pa_qahw_effect_endpoint_info *port = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("port_effect_create has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "port_effect_create has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(uqqqay)s")) {
        pa_log_error("Invalid signature for port_effect_create.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for port_effect_create.");
        dbus_error_free(&error);
        return;
    }

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
    dbus_message_iter_get_basic(&arg_i, &port_name);

    PA_HASHMAP_FOREACH(effect, m_data->effects, state)
        if (memcmp(&uuid, effect->uuid, sizeof(qahw_effect_uuid_t)) == 0)
            break;

    if (!effect) {
        pa_log_error("Unsupported UUID.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported UUID.");
        dbus_error_free(&error);
        return;
    }

    port = pa_hashmap_get(effect->ports, port_name);

    if (!port) {
        pa_log_error("Invalid port name.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid port name.");
        dbus_error_free(&error);
        return;
    }

    if (port->client_count == 0) {
        port->lib_handle = qahw_effect_load_library(effect->lib_name);
        rc = qahw_effect_create(port->lib_handle, &uuid, -1, &port->effect_handle);

        if (rc != 0) {
            pa_log_error("PortEffectCreate failed\n");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_create failed.");
            dbus_error_free(&error);
            return;
        }

        port->client_count++;
        port->dbus_obj_path = pa_qahw_get_obj_path(m_data->dbus_obj_path, port_name, effect->index);
        port->dbus_protocol = m_data->dbus_protocol;
        ses_data = pa_xnew0(pa_qahw_effect_session_data, 1);
        ses_data->endpoint_name = pa_xstrdup(port_name);
        ses_data->endpoints = effect->ports;
        pa_hashmap_put(m_data->sessions, port->dbus_obj_path, ses_data);
        pa_assert_se(pa_dbus_protocol_add_interface(port->dbus_protocol,
                     port->dbus_obj_path, &session_interface_info, ses_data) >= 0);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &port->dbus_obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        dbus_message_unref(reply);
    } else {
        port->client_count++;
        obj_path = pa_qahw_get_obj_path(m_data->dbus_obj_path, port_name, effect->index);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        pa_xfree(obj_path);
        dbus_error_free(&error);
        dbus_message_unref(reply);
        return;
    }
}

static void pa_qahw_sink_effect_create(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    pa_qahw_effect_session_data *ses_data;
    DBusError error;
    qahw_effect_uuid_t uuid;
    audio_io_handle_t io_handle;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, struct_i;
    int rc = -1;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    uint32_t sink_id;
    char *obj_path;
    void *state;
    char *sink_name;
    pa_qahw_effect_info *effect = NULL;
    pa_qahw_effect_endpoint_info *sink = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("sink_effect_create has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "sink_effect_create has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(uqqqay)u")) {
        pa_log_error("Invalid signature for sink_effect_create.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for sink_effect_create.");
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

    PA_HASHMAP_FOREACH(effect, m_data->effects, state)
        if (memcmp(&uuid, effect->uuid, sizeof(qahw_effect_uuid_t)) == 0)
            break;

    if (!effect) {
        pa_log_error("Unsupported UUID.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported UUID.");
        dbus_error_free(&error);
        return;
    }

    sink_name = pa_qahw_sink_get_name_from_pa_sink_id(sink_id);

    if (sink_name != NULL) {
        io_handle = pa_qahw_sink_get_io_handle(sink_id);
        if (io_handle == -1) {
            pa_log_error("%s: Unable to retrieve sink io handle\n", __func__);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unable to retrieve sink io handle.");
            dbus_error_free(&error);
            return;
        }
        sink = pa_hashmap_get(effect->sinks, sink_name);
        if (!sink) {
            pa_log_error("%s: Unable to retrieve sink info\n", __func__);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unable to retrieve sink info.");
            dbus_error_free(&error);
            return;
        }
    } else {
        pa_log_error("Invalid sink index.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid sink index.");
        dbus_error_free(&error);
        return;
    }

    if (sink->client_count == 0) {
        sink->lib_handle = qahw_effect_load_library(effect->lib_name);
        rc = qahw_effect_create(sink->lib_handle, &uuid, io_handle, &sink->effect_handle);

        if (rc != 0) {
            pa_log_error("SinkEffectCreate failed\n");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_create failed.");
            dbus_error_free(&error);
            return;
        }

        sink->client_count++;
        sink->dbus_obj_path = pa_qahw_get_obj_path(m_data->dbus_obj_path, sink_name, effect->index);
        sink->dbus_protocol = m_data->dbus_protocol;
        ses_data = pa_xnew0(pa_qahw_effect_session_data, 1);
        ses_data->endpoint_name = pa_xstrdup(sink_name);
        ses_data->endpoints = effect->sinks;
        pa_hashmap_put(m_data->sessions, sink->dbus_obj_path, ses_data);
        sink->sessions = m_data->sessions;
        pa_assert_se(pa_dbus_protocol_add_interface(sink->dbus_protocol,
                     sink->dbus_obj_path, &session_interface_info, ses_data) >= 0);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &sink->dbus_obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        dbus_message_unref(reply);
    } else {
        sink->client_count++;
        obj_path = pa_qahw_get_obj_path(m_data->dbus_obj_path, sink_name, effect->index);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        pa_xfree(obj_path);
        dbus_error_free(&error);
        dbus_message_unref(reply);
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
    pa_qahw_effect_info *effect;
    void *state;
    char *port_name = NULL;
    uint32_t port_effects = 0;
    pa_device_port *p;
    audio_devices_t *audio_device;

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("port_get_supported_effects has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "port_get_supported_effects has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "s")) {
        pa_log_error("Invalid signature for port_get_supported_effects.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for port_get_supported_effects.");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_get_basic(&arg_i, &port_name);

    PA_HASHMAP_FOREACH(effect, m_data->effects, state)
        if (pa_hashmap_get(effect->ports, port_name)
            && pa_hashmap_get(m_data->card->ports, port_name))
            port_effects++;

    if (port_effects <= 0) {
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

        PA_HASHMAP_FOREACH(effect, m_data->effects, state) {
            if (pa_hashmap_get(effect->ports, port_name)) {
                dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
                pa_qahw_fill_effect_uuid(&struct_i, *(effect->uuid));
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
    uint32_t sink_effects = 0;
    pa_qahw_effect_info *effect;
    void *state;
    char *sink_name;

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("sink_get_supported_efffects has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "sink_get_supported_efffects has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "u")) {
        pa_log_error("Invalid signature for sink_get_supported_efffects.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for sink_get_supported_efffects.");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("%s\n", __func__);

    dbus_message_iter_get_basic(&arg_i, &sink_id);

    sink_name = pa_qahw_sink_get_name_from_pa_sink_id(sink_id);
    if (!sink_name) {
        pa_log_error("Invalid sink index.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid sink index.");
        dbus_error_free(&error);
        return;
    }

    /* calculate number of effects supported on this sink */
    PA_HASHMAP_FOREACH(effect, m_data->effects, state)
        if (pa_hashmap_get(effect->sinks, sink_name))
            sink_effects++;

    if (sink_effects <= 0) {
        pa_log_error("Invalid sink index.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid sink index.");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg);
    dbus_message_iter_append_basic(&arg, DBUS_TYPE_UINT32, &sink_effects);
    dbus_message_iter_open_container(&arg, DBUS_TYPE_ARRAY, "(uqqqay)", &array_i);

    PA_HASHMAP_FOREACH(effect, m_data->effects, state) {
        if (pa_hashmap_get(effect->sinks, sink_name)) {
            dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
            pa_qahw_fill_effect_uuid(&struct_i, *(effect->uuid));
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
    pa_qahw_effect_info *effect_info;
    qahw_effect_descriptor_t effect_descriptors;
    qahw_effect_lib_handle_t lib_handle;
    int rc = -1;
    uint32_t j = 0;
    void *state;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg);
    dbus_message_iter_append_basic(&arg, DBUS_TYPE_UINT32, &m_data->max_supported_effects);
    dbus_message_iter_open_container(&arg, DBUS_TYPE_ARRAY, "((uqqqay)(uqqqay)uuqqayay)", &array_i);

    PA_HASHMAP_FOREACH(effect_info, m_data->effects, state) {
        lib_handle = qahw_effect_load_library(effect_info->lib_name);
        rc = qahw_effect_get_descriptor(lib_handle, effect_info->uuid, &effect_descriptors);
        pa_log_info("%s: qahw_effect_get_descriptor() returns %d\n", __func__, rc);
        rc = qahw_effect_unload_library(lib_handle);
        pa_log_info("%s: qahw_effect_unload_library() returns %d\n", __func__, rc);

        dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_STRUCT, NULL, &struct_ii);
        pa_qahw_fill_effect_uuid(&struct_ii, effect_descriptors.type);
        dbus_message_iter_close_container(&struct_i, &struct_ii);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_STRUCT, NULL, &struct_iii);
        pa_qahw_fill_effect_uuid(&struct_iii, effect_descriptors.uuid);
        dbus_message_iter_close_container(&struct_i, &struct_iii);

        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &effect_descriptors.apiVersion);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &effect_descriptors.flags);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT16, &effect_descriptors.cpuLoad);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT16,
                                       &effect_descriptors.memoryUsage);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y", &array_iii);
        for (j = 0; j < strlen(effect_descriptors.name); j++)
            dbus_message_iter_append_basic(&array_iii, DBUS_TYPE_BYTE, &effect_descriptors.name[j]);
        dbus_message_iter_append_basic(&array_iii, DBUS_TYPE_BYTE, &effect_descriptors.name[j]);
        dbus_message_iter_close_container(&struct_i, &array_iii);

        dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y", &array_iv);
        for (j = 0; j < strlen(effect_descriptors.implementor); j++)
            dbus_message_iter_append_basic(&array_iv, DBUS_TYPE_BYTE,
                                           &effect_descriptors.implementor[j]);
        dbus_message_iter_append_basic(&array_iv, DBUS_TYPE_BYTE,
                                       &effect_descriptors.implementor[j]);
        dbus_message_iter_close_container(&struct_i, &array_iv);

        dbus_message_iter_close_container(&array_i, &struct_i);
    }

    dbus_message_iter_close_container(&arg, &array_i);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

static void pa_qahw_free_session(pa_qahw_effect_session_data *session) {
    pa_assert(session);

    pa_log_info("%s: freeing session", __func__);

    if (session != NULL) {
        if (session->endpoint_name != NULL) {
            pa_xfree(session->endpoint_name);
            session->endpoint_name = NULL;
        }

        pa_xfree(session);
        session = NULL;
    }
}

static void pa_qahw_effect_free_endpoint(pa_qahw_effect_endpoint_info *endpoint) {
    pa_assert(endpoint);

    pa_log_info("%s: freeing %s", __func__, endpoint->type);

    if (endpoint != NULL) {
        if ((endpoint->lib_handle != NULL) && (endpoint->effect_handle != NULL)) {
            qahw_effect_release(endpoint->lib_handle, endpoint->effect_handle);
            qahw_effect_unload_library(endpoint->lib_handle);
            endpoint->lib_handle = NULL;
            endpoint->effect_handle = NULL;
        }

        if (endpoint->dbus_obj_path != NULL) {
            pa_assert_se(pa_dbus_protocol_remove_interface(endpoint->dbus_protocol,
                         endpoint->dbus_obj_path, session_interface_info.name) >= 0);
            pa_xfree(endpoint->dbus_obj_path);
            endpoint->dbus_obj_path = NULL;
        }

        if (endpoint->type != NULL) {
            pa_xfree(endpoint->type);
            endpoint->type = NULL;
        }

        pa_xfree(endpoint);
        endpoint = NULL;
    }
};

static void pa_qahw_effect_free(pa_qahw_effect_info *effect) {
    pa_assert(effect);

    pa_log_info("%s: freeing effect %s", __func__, effect->name);

    if (pa_streq(effect->type, "sink")) {
        pa_hashmap_free(effect->sinks);
    } else if (pa_streq(effect->type, "port")) {
        pa_hashmap_free(effect->ports);
    }

    if (effect->name != NULL) {
        pa_xfree(effect->name);
        effect->name = NULL;
    }

    if (effect->type != NULL) {
        pa_xfree(effect->type);
        effect->type = NULL;
    }

    if (effect->uuid != NULL) {
        pa_xfree(effect->uuid);
        effect->uuid = NULL;
    }

    if (effect->lib_name != NULL) {
        pa_xfree(effect->lib_name);
        effect->lib_name = NULL;
    }

    pa_xfree(effect);
    effect = NULL;
};


pa_qahw_effect_handle_t pa_qahw_init_effect(char *dbus_obj_path,
                                            pa_dbus_protocol *dbus_protocol,
                                            pa_hashmap *effects,
                                            pa_card *card ) {

    pa_qahw_effect_module_data *effect_mdata;

    uint32_t i = 0;

    pa_qahw_effect_config *effect_config;
    pa_qahw_sink_config *sink_config;
    pa_qahw_card_port_config *port_config;

    pa_qahw_effect_info *effect_info;
    pa_qahw_effect_endpoint_info *sink = NULL;
    pa_qahw_effect_endpoint_info *port = NULL;
    char *name;

    void *state;
    void *state1;

    pa_log_debug("%s", __func__);

    pa_assert(dbus_protocol);
    pa_assert(effects);
    pa_assert(card);

    if (module_intialized) {
        pa_log_info("%s: already intialized", __func__);
        return NULL;
    } else {
        effect_mdata = pa_xnew0(pa_qahw_effect_module_data, 1);
    }

    effect_mdata->dbus_obj_path = dbus_obj_path;
    effect_mdata->dbus_protocol = dbus_protocol;
    effect_mdata->max_supported_effects = pa_hashmap_size(effects);
    effect_mdata->sessions = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                 pa_idxset_string_compare_func, NULL,
                                                 (pa_free_cb_t) pa_qahw_free_session);
    effect_mdata->effects = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                pa_idxset_string_compare_func, NULL,
                                                (pa_free_cb_t) pa_qahw_effect_free);

    /* copy pa_qahw_config_effect to local data structure */
    PA_HASHMAP_FOREACH(effect_config, effects, state) {
       effect_info = pa_xnew(pa_qahw_effect_info, 1);
       effect_info->name = pa_xstrdup(effect_config->name);
       effect_info->type = pa_xstrdup(effect_config->type);
       effect_info->index = i++;
       state1 = NULL;

       if (pa_streq(effect_info->type, "sink")) {
           effect_info->sinks = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                    pa_idxset_string_compare_func, NULL,
                                                    (pa_free_cb_t) pa_qahw_effect_free_endpoint);
           PA_HASHMAP_FOREACH(sink_config, effect_config->sinks, state1) {
               sink = pa_xnew(pa_qahw_effect_endpoint_info, 1);
               sink->client_count = 0;
               sink->effect_handle = NULL;
               sink->lib_handle = NULL;
               sink->dbus_obj_path = NULL;
               name =  pa_xstrdup(sink_config->name);
               sink->type = pa_xstrdup(effect_info->type);
               pa_hashmap_put(effect_info->sinks, name, sink);
               pa_log_info("%s: Adding effect %s of type %s to sink %s", __func__,
                           effect_info->name, effect_info->type, name);
           }
       } else if  (pa_streq(effect_info->type, "port")) {
           effect_info->ports = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                    pa_idxset_string_compare_func, NULL,
                                                    (pa_free_cb_t) pa_qahw_effect_free_endpoint);
           PA_HASHMAP_FOREACH(port_config, effect_config->ports, state1) {
               port = pa_xnew(pa_qahw_effect_endpoint_info, 1);
               port->client_count = 0;
               port->effect_handle = NULL;
               port->lib_handle = NULL;
               port->dbus_obj_path = NULL;
               name =  pa_xstrdup(port_config->name);
               port->type = pa_xstrdup(effect_info->type);
               pa_hashmap_put(effect_info->ports, name, port);
               pa_log_info("%s: Adding effect %s of type %s to port %s", __func__,
                           effect_info->name, effect_info->type, name);
           }
       }

       /* fill uuid */
       effect_info->uuid = pa_xnew0(qahw_effect_uuid_t, 1);

       pa_qahw_get_effect_uuid(effect_config->name, effect_info->uuid);

       /* get lib name */
       effect_info->lib_name = pa_xstrdup(pa_qahw_get_effect_lib_name(effect_config->name));

       pa_hashmap_put(effect_mdata->effects, effect_info->name, effect_info);
    }

    effect_mdata->card = card;

    pa_assert_se(pa_dbus_protocol_add_interface(effect_mdata->dbus_protocol,
                 effect_mdata->dbus_obj_path, &module_interface_info, effect_mdata) >= 0);

    pa_log_info("Interface is added for object %s\n", effect_mdata->dbus_obj_path);

    module_intialized = true;

    return (pa_qahw_effect_handle_t)effect_mdata;
}

void pa_qahw_deinit_effect(pa_qahw_effect_handle_t effect_handle) {
    pa_qahw_effect_module_data *effect_mdata = (pa_qahw_effect_module_data *)effect_handle;

    pa_log_debug("%s\n", __func__);

    pa_assert(effect_mdata);

    pa_hashmap_free(effect_mdata->effects);
    effect_mdata->effects = NULL;
    pa_hashmap_free(effect_mdata->sessions);
    effect_mdata->sessions = NULL;

    if (effect_mdata->dbus_obj_path && effect_mdata->dbus_protocol) {
        pa_assert_se(pa_dbus_protocol_remove_interface(effect_mdata->dbus_protocol,
                        effect_mdata->dbus_obj_path, module_interface_info.name) >= 0);
        pa_dbus_protocol_unref(effect_mdata->dbus_protocol);
        pa_xfree(effect_mdata->dbus_obj_path);
    }

    module_intialized = false;
    pa_xfree(effect_mdata);
    effect_mdata = NULL;

    return;
}
