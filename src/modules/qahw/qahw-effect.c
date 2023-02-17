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

#include "qahw-sink.h"
#include "qahw-utils.h"
#include "qahw-loopback.h"
#include <qahw_effect_api.h>
#include <qahw_effect_bassboost.h>
#include <qahw_effect_virtualizer.h>
#include <qahw_effect_equalizer.h>
#include <qahw_effect_presetreverb.h>
#include <qahw_effect_environmentalreverb.h>
#include <qahw_effect_audiosphere.h>
#if !defined(__DISABLE_EFFECT_TRUMPET)
#include <qahw_effect_trumpet.h>
#endif
typedef enum {
    PA_QAHW_EFFECT_TYPE_SINK = 0,
    PA_QAHW_EFFECT_TYPE_PORT,
    PA_QAHW_EFFECT_TYPE_LOOPBACK
} pa_qahw_effect_endpoint_type_t;

static bool module_intialized = false;

/* Info related to an effect */
typedef struct {
    char *name;                 /* Name of the effect */
    char *description;          /* Description of the effect */
    int index;                  /* Effect index */
    qahw_effect_uuid_t *uuid;   /* Effect UUID */
    char* lib_name;             /* Effect library name */
    pa_hashmap *sinks;          /* All the sinks on which this effect is supported */
    pa_hashmap *ports;          /* All the ports on which this effect is supported */
    pa_hashmap *loopbacks;      /* All the loopbacks on which this effect is supported */
} pa_qahw_effect_info;

/* Info related to endpoint */
typedef struct {
    char *name;                         /* Name of the endpoint */
    int client_count;                   /* Number of clients registered for a particular effect on an endpoint */
    pa_qahw_effect_endpoint_type_t type;  /* Endpoint type. Sink/Port/Loopback */
} pa_qahw_effect_endpoint_info;

/* Maps effect names to effect library names */
typedef struct {
    const char *effect_name;
    const char *lib_name;
} pa_qahw_effect_name_to_lib_mapping;

/* Used to know if an effect is enabled/disabled */
typedef struct {
    char *effect_name;
    bool is_enabled;
} pa_qahw_effect_status;

/* Module Level data */
typedef struct {
    char *dbus_obj_path;               /* Dbus path where effect module listens for connections */
    pa_dbus_protocol *dbus_protocol;   /* Dbus protocol */
    pa_card *card;
    uint32_t max_supported_effects;    /* Count of all the effects supported by module */

    pa_hashmap *effects;               /* Hashmap containing all effects info */
    pa_hashmap *sessions;              /* Hashmap containing all active sessions across all endpoints */
    pa_hashmap *callbacks;             /* Hashmap containing all the registered callbacks */
} pa_qahw_effect_module_data;

/* Session specific data */
typedef struct {
    char *endpoint_name;                    /* Endpoint name a session is currently active on */
    char *effect_name;                      /* Effect corresponding to a session */
    char *dbus_obj_path;                    /* Dbus path for session specific APIs */

    qahw_effect_lib_handle_t lib_handle;    /* Effect lib handle */
    pa_qahw_effect_handle_t effect_handle;  /* Effect handle */
    uint64_t handle;                        /* Handle specific to an endpoint. Ex: IO handle for sink, Patch handle for loopback */

    pa_qahw_effect_endpoint_info *endpoint; /* Info of endpoint corresponding to a session */
    pa_qahw_effect_module_data *common;     /* Pointer to module data. */
} pa_qahw_effect_session_data;

static void pa_qahw_module_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_port_get_supported_effects(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_effect_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_port_effect_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_loopback_effect_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_sink_effect_release(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_port_effect_release(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_loopback_effect_release(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_get_descriptor(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_get_version(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_effect_command(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum module_handler_index {
    MODULE_HANDLER_GET_MODULE_SUPPORTED_EFFECTS,
    MODULE_HANDLER_SINK_SUPPORTED_EFFECTS,
    MODULE_HANDLER_PORT_SUPPORTED_EFFECTS,
    MODULE_HANDLER_SINK_EFFECT_CREATE,
    MODULE_HANDLER_PORT_EFFECT_CREATE,
    MODULE_HANDLER_LOOPBACK_EFFECT_CREATE,
    MODULE_HANDLER_EFFECT_GET_VERSION,
    MODULE_HANDLER_MAX
};

enum session_handler_index {
    SESSION_HANDLER_SINK_EFFECT_RELEASE,
    SESSION_HANDLER_PORT_EFFECT_RELEASE,
    SESSION_HANDLER_LOOPBACK_EFFECT_RELEASE,
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

pa_dbus_arg_info loopback_effect_create_args[] = {
    {"uuid", "(uqqqay)", "in"},
    {"patch_handle", "t", "in"},
    {"obj_path", "o", "out"},
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

pa_dbus_arg_info loopback_effect_release_args[] = {
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
    {"reply_size", "u", "in"},
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
    [MODULE_HANDLER_LOOPBACK_EFFECT_CREATE] = {
        .method_name = "LoopbackEffectCreate",
        .arguments = loopback_effect_create_args,
        .n_arguments = sizeof(loopback_effect_create_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_loopback_effect_create},
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
    [SESSION_HANDLER_LOOPBACK_EFFECT_RELEASE] = {
        .method_name = "LoopbackEffectRelease",
        .arguments = loopback_effect_release_args,
        .n_arguments = sizeof(loopback_effect_release_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_loopback_effect_release},
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

static int pa_qahw_effect_string_to_uuid(char *str, qahw_effect_uuid_t *uuid) {
    int tmp[10];

    if (str == NULL || uuid == NULL) {
        pa_log_debug("str or uuid is null\n");
        return -1;
    }

    if (sscanf(str, "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
            tmp, tmp+1, tmp+2, tmp+3, tmp+4, tmp+5, tmp+6, tmp+7, tmp+8, tmp+9) < 10) {
        pa_log_debug("%s: Unable to parse uuid. Invalid uuid format\n", __func__);
        return -1;
    }

    uuid->timeLow = (uint32_t)tmp[0];
    uuid->timeMid = (uint16_t)tmp[1];
    uuid->timeHiAndVersion = (uint16_t)tmp[2];
    uuid->clockSeq = (uint16_t)tmp[3];
    uuid->node[0] = (uint8_t)tmp[4];
    uuid->node[1] = (uint8_t)tmp[5];
    uuid->node[2] = (uint8_t)tmp[6];
    uuid->node[3] = (uint8_t)tmp[7];
    uuid->node[4] = (uint8_t)tmp[8];
    uuid->node[5] = (uint8_t)tmp[9];

    return 0;
}

static int pa_qahw_get_latency_cmd_code(char *effect_name) {
    if (pa_streq(effect_name, "bassboost"))
        return BASSBOOST_PARAM_LATENCY;
    else if (pa_streq(effect_name, "virtualizer"))
        return VIRTUALIZER_PARAM_LATENCY;
    else if (pa_streq(effect_name, "equalizer"))
        return EQ_PARAM_LATENCY;
    else if (pa_streq(effect_name, "preset_reverb"))
        return REVERB_PARAM_LATENCY;
    else
        return -1;
}

static void pa_qahw_effect_free_status(pa_qahw_effect_status *effect_status) {
    pa_assert(effect_status);

    if (effect_status != NULL) {
        if (effect_status->effect_name != NULL) {
            pa_xfree(effect_status->effect_name);
            effect_status->effect_name = NULL;
        }

        pa_xfree(effect_status);
    }
}

int pa_qahw_effect_register_callback(pa_qahw_effect_handle_t effect_handle, pa_qahw_effect_callback_config *cb) {
    pa_qahw_effect_callback_config *cb_conf = NULL;
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)effect_handle;

    pa_assert(m_data->callbacks);

    pa_log_debug("%s\n", __func__);

    cb_conf = pa_xnew0(pa_qahw_effect_callback_config, 1);
    cb_conf->endpoint_name = pa_xstrdup(cb->endpoint_name);
    cb_conf->effects = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, (pa_free_cb_t)pa_qahw_effect_free_status);
    cb_conf->cb_func = cb->cb_func;
    cb_conf->prv_data = cb->prv_data;

    pa_hashmap_put(m_data->callbacks, cb_conf->endpoint_name, cb_conf);

    return 0;
}

int pa_qahw_effect_deregister_callback(pa_qahw_effect_handle_t effect_handle, char *endpoint_name) {
    pa_qahw_effect_callback_config *cb_conf = NULL;
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)effect_handle;

    pa_log_debug("%s\n", __func__);

    if (m_data != NULL) {
        if (m_data->callbacks != NULL) {
            if (endpoint_name != NULL) {
                cb_conf = pa_hashmap_get(m_data->callbacks, endpoint_name);
            } else {
                pa_log_error("%s: Invalid endpoint name\n", __func__);
                return -1;
            }

            if (cb_conf != NULL) {
                pa_hashmap_remove(m_data->callbacks, endpoint_name);
                if (cb_conf->endpoint_name != NULL) {
                    pa_xfree(cb_conf->endpoint_name);
                    cb_conf->endpoint_name = NULL;
                }

                if (cb_conf->effects != NULL) {
                    pa_hashmap_free(cb_conf->effects);
                    cb_conf->effects = NULL;
                }

                if (cb_conf->prv_data != NULL) {
                    pa_xfree(cb_conf->prv_data);
                    cb_conf->prv_data = NULL;
                }

                cb_conf->cb_func = NULL;
                pa_xfree(cb_conf);
            } else {
                pa_log_error("%s: failed\n", __func__);
                return -1;
            }
        }
    } else {
        pa_log_error("%s: failed\n", __func__);
        return -1;
    }

    return 0;
}

static pa_qahw_effect_callback_config *pa_qahw_get_callback_data(pa_hashmap *callbacks, char *endpoint_name) {
    pa_qahw_effect_callback_config *cb = NULL;
    void *state;

    PA_HASHMAP_FOREACH(cb, callbacks, state) {
        if (pa_streq(cb->endpoint_name, endpoint_name))
            return cb;
    }

    return NULL;
}

static void pa_qahw_update_effect_status(pa_qahw_effect_callback_config *cb, char *effect_name, bool value) {
    pa_qahw_effect_status *status = NULL;

    status = pa_hashmap_get(cb->effects, effect_name);
    if (status != NULL)
        status->is_enabled = value;
}

static bool pa_qahw_is_effect_enabled(pa_qahw_effect_callback_config *cb, char *effect_name) {
    pa_qahw_effect_status *status = NULL;

    pa_log_debug("%s\n", __func__);

    status = pa_hashmap_get(cb->effects, effect_name);
    if (status != NULL) {
        if (status->is_enabled)
            return true;
        else
            return false;
    }

    return false;
}

static int pa_qahw_effect_cmd_offload(pa_qahw_effect_handle_t effect_handle, int handle) {
    uint32_t cmd_size = sizeof(qahw_effect_offload_param_t);
    uint32_t reply_size = sizeof(uint32_t);
    void *reply_data = (void *)malloc(reply_size);
    int rc = 0;

    qahw_effect_offload_param_t *cmd_data = (qahw_effect_offload_param_t *)malloc(cmd_size);

    pa_log_debug("%s\n", __func__);
    cmd_data->isOffload = true;
    cmd_data->ioHandle = handle;

    rc = qahw_effect_command(effect_handle, QAHW_EFFECT_CMD_OFFLOAD, cmd_size, (void *)cmd_data, &reply_size, reply_data);

    free(cmd_data);
    free(reply_data);
    return rc;
}

static int pa_qahw_effect_get_latency(pa_qahw_effect_handle_t effect_handle, int handle, char *effect_name) {
    uint32_t cmd_size = sizeof(qahw_effect_param_t) + sizeof(uint32_t);
    uint32_t reply_size = sizeof(qahw_effect_param_t) + 2*sizeof(uint32_t);
    uint32_t arr2[reply_size];
    int rc = 0;
    int cmd_code;
    void *value;
    int offset;

    qahw_effect_param_t *cmd_data = (qahw_effect_param_t *)malloc(cmd_size);
    qahw_effect_param_t *reply_data;

    pa_log_debug("%s\n", __func__);

    rc = pa_qahw_effect_cmd_offload(effect_handle, handle);

    if (rc < 0)
        pa_log_error("qahw_effect_command failed\n");

    cmd_code = pa_qahw_get_latency_cmd_code(effect_name);
    cmd_data->psize = sizeof(int32_t);
    cmd_data->vsize = sizeof(int32_t);
    memcpy(cmd_data->data, &cmd_code, sizeof(int32_t));
    reply_data = (qahw_effect_param_t *)arr2;
    reply_data->psize = sizeof(int32_t);
    reply_data->vsize = sizeof(int32_t);

    rc = qahw_effect_command(effect_handle, QAHW_EFFECT_CMD_GET_PARAM, cmd_size, (void *)cmd_data, &reply_size, (void *)reply_data);

    free(cmd_data);

    if (rc < 0) {
        pa_log_error("qahw_effect_command failed\n");
        return -1;
    }

    offset = ((reply_data->psize - 1) / sizeof(int32_t) + 1) * sizeof(int32_t);
    value = reply_data->data + offset;

    return (*(uint32_t *)value);
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
            if (effect->sinks != NULL)
                sink = pa_hashmap_get(effect->sinks, sink_name);

            if (sink != NULL) {
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
    pa_qahw_effect_callback_config *cb = NULL;
    pa_qahw_effect_callback_enable_event_data enable_data;
    pa_qahw_effect_callback_disable_event_data disable_data;

    DBusError error;
    DBusMessageIter arg_i, array_i;
    DBusMessage *reply = NULL;
    DBusMessageIter r_arg, r_array_i;

    int n_elements = 0;
    uint32_t cmd_code;
    uint32_t cmd_size;
    void *cmd_data = NULL;
    uint32_t reply_size;
    void *reply_data = NULL;
    int rc = -1;
    int latency;
    qahw_effect_offload_param_t *offload_cmd = NULL;
    qahw_effect_offload_param_t *temp_data = NULL;

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

    if (!pa_streq(dbus_message_get_signature(msg), "uuuay")) {
        pa_log_error("Invalid signature for effect_command.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for effect_command.");
        dbus_error_free(&error);
        return;
    }

    endpoint = ses_data->endpoint;

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
    dbus_message_iter_get_basic(&arg_i, &reply_size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, &cmd_data, &n_elements);

    switch (cmd_code) {
        case QAHW_EFFECT_CMD_INIT:
        case QAHW_EFFECT_CMD_SET_CONFIG:
        case QAHW_EFFECT_CMD_SET_PARAM:
        case QAHW_EFFECT_CMD_RESET:
        case QAHW_EFFECT_CMD_SET_DEVICE:
        case QAHW_EFFECT_CMD_SET_AUDIO_MODE:
        case QAHW_EFFECT_CMD_SET_VOLUME:
        case QAHW_EFFECT_CMD_GET_CONFIG:
        case QAHW_EFFECT_CMD_GET_PARAM:
            break;
        case QAHW_EFFECT_CMD_OFFLOAD:
            reply_size = sizeof(uint32_t);
            reply_data = (void *)malloc(reply_size);
            temp_data = (qahw_effect_offload_param_t *)cmd_data;
            offload_cmd = (qahw_effect_offload_param_t *)malloc(cmd_size);

            offload_cmd->isOffload = temp_data->isOffload;
            offload_cmd->ioHandle = ses_data->handle;

            rc = qahw_effect_command(ses_data->effect_handle, QAHW_EFFECT_CMD_OFFLOAD, cmd_size, (void *)offload_cmd, &reply_size, reply_data);
            free(offload_cmd);
            goto done;
        case QAHW_EFFECT_CMD_ENABLE:
            if (endpoint->type == PA_QAHW_EFFECT_TYPE_LOOPBACK) {
                cb = pa_qahw_get_callback_data(ses_data->common->callbacks, ses_data->endpoint_name);

                if (cb != NULL) {

                    if (!pa_qahw_is_effect_enabled(cb, ses_data->effect_name)) {
                        latency = pa_qahw_effect_get_latency(ses_data->effect_handle, ses_data->handle, ses_data->effect_name);

                        if (latency < 0) {
                            pa_log_error("%s: pa_qahw_effect_get_latency failed\n", __func__);
                            break;
                        }

                        enable_data.handle = ses_data->handle;
                        enable_data.latency = latency;
                        enable_data.effect_name = pa_xstrdup(ses_data->effect_name);

                        cb->cb_func(PA_QAHW_EFFECT_EVENT_ENABLE, (void *)(&enable_data), cb->prv_data);

                        pa_xfree(enable_data.effect_name);
                        pa_qahw_update_effect_status(cb, ses_data->effect_name, true);
                    }
                }
            }
            break;
        case QAHW_EFFECT_CMD_DISABLE:
            if (endpoint->type == PA_QAHW_EFFECT_TYPE_LOOPBACK) {
                cb = pa_qahw_get_callback_data(ses_data->common->callbacks, ses_data->endpoint_name);

                if (cb != NULL) {
                    if (pa_qahw_is_effect_enabled(cb, ses_data->effect_name)) {
                        latency = pa_qahw_effect_get_latency(ses_data->effect_handle, ses_data->handle, ses_data->effect_name);

                        if (latency < 0) {
                            pa_log_error("%s: pa_qahw_effect_get_latency failed\n", __func__);
                            break;
                        }

                        disable_data.handle = ses_data->handle;
                        disable_data.latency = (latency * (-1));
                        disable_data.effect_name = pa_xstrdup(ses_data->effect_name);

                        cb->cb_func(PA_QAHW_EFFECT_EVENT_DISABLE, (void *)(&disable_data), cb->prv_data);
                        pa_xfree(disable_data.effect_name);
                        pa_qahw_update_effect_status(cb, ses_data->effect_name, false);
                    }
                }
            }
            break;
        default:
            pa_log_error("Invalid command \n");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid effect command.");
            dbus_error_free(&error);
            return;
    }

    reply_data = (void *)malloc(reply_size);

    rc = qahw_effect_command(ses_data->effect_handle, cmd_code, cmd_size, cmd_data, &reply_size, reply_data);

done:
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
    qahw_effect_uuid_t uuid;
    qahw_effect_descriptor_t effect_desc;

    DBusError error;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, struct_i, array_i;
    DBusMessageIter r_arg;
    DBusMessageIter r_array_i, r_array_ii;
    DBusMessageIter r_struct_i, r_struct_ii, r_struct_iii;

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

    endpoint = ses_data->endpoint;

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
        rc = qahw_effect_get_descriptor(ses_data->lib_handle, &uuid, &effect_desc);
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

static void pa_qahw_loopback_effect_release(DBusConnection *conn,
                                            DBusMessage *msg,
                                            void *userdata) {
    pa_qahw_effect_endpoint_info *loopback = NULL;
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;
    pa_qahw_effect_callback_config *cb = NULL;

    int rc = -1;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    loopback = ses_data->endpoint;

    if (loopback) {
        loopback->client_count--;
    } else {
        pa_log_error("%s: Error in getting endpoint\n", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Error in getting endpoint.");
        return;
    }

    if (loopback->client_count == 0) {
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

        cb = pa_qahw_get_callback_data(ses_data->common->callbacks, ses_data->endpoint_name);
        if (cb != NULL) {
            pa_hashmap_remove(ses_data->common->callbacks, ses_data->endpoint_name);
            if (cb->endpoint_name != NULL) {
                pa_xfree(cb->endpoint_name);
                cb->endpoint_name = NULL;
            }

            if (cb->effects != NULL)
                pa_hashmap_free(cb->effects);
            pa_xfree(cb);
        }

        pa_hashmap_remove(ses_data->common->sessions, ses_data->dbus_obj_path);
        ses_data->effect_handle = NULL;
        ses_data->lib_handle = NULL;

        if (ses_data->endpoint_name != NULL) {
            pa_xfree(ses_data->endpoint_name);
            ses_data->endpoint_name = NULL;
        }

        if (ses_data->effect_name != NULL) {
            pa_xfree(ses_data->effect_name);
            ses_data->effect_name = NULL;
        }

        if (ses_data->dbus_obj_path != NULL) {
            pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol,
                         ses_data->dbus_obj_path, session_interface_info.name) >= 0);
            pa_dbus_send_empty_reply(conn, msg);
            pa_xfree(ses_data->dbus_obj_path);
            ses_data->dbus_obj_path = NULL;
        }

        pa_xfree(ses_data);

        ses_data = NULL;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_port_effect_release(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    pa_qahw_effect_endpoint_info *port = NULL;
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;

    int rc = -1;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    port = ses_data->endpoint;

    if (port) {
        port->client_count--;
    } else {
        pa_log_error("%s: Error in getting endpoint\n", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Error in getting endpoint.");
        return;
    }

    if (port->client_count == 0) {
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

        pa_hashmap_remove(ses_data->common->sessions, ses_data->dbus_obj_path);
        ses_data->effect_handle = NULL;
        ses_data->lib_handle = NULL;

        if (ses_data->endpoint_name != NULL) {
            pa_xfree(ses_data->endpoint_name);
            ses_data->endpoint_name = NULL;
        }

        if (ses_data->effect_name != NULL) {
            pa_xfree(ses_data->effect_name);
            ses_data->effect_name = NULL;
        }

        if (ses_data->dbus_obj_path != NULL) {
            pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol,
                         ses_data->dbus_obj_path, session_interface_info.name) >= 0);
            pa_dbus_send_empty_reply(conn, msg);
            pa_xfree(ses_data->dbus_obj_path);
            ses_data->dbus_obj_path = NULL;
        }

        pa_xfree(ses_data);
        ses_data = NULL;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_sink_effect_release(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    pa_qahw_effect_endpoint_info *sink = NULL;
    pa_qahw_effect_session_data *ses_data = (pa_qahw_effect_session_data *)userdata;

    int rc = -1;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    sink = ses_data->endpoint;

    if (sink) {
        sink->client_count--;
    } else {
        pa_log_error("%s: Error in getting endpoint\n", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Error in getting endpoint.");
        return;
    }

    if (sink->client_count == 0) {
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

        pa_hashmap_remove(ses_data->common->sessions, ses_data->dbus_obj_path);
        ses_data->effect_handle = NULL;
        ses_data->lib_handle = NULL;

        if (ses_data->endpoint_name != NULL) {
            pa_xfree(ses_data->endpoint_name);
            ses_data->endpoint_name = NULL;
        }

        if (ses_data->effect_name != NULL) {
            pa_xfree(ses_data->effect_name);
            ses_data->effect_name = NULL;
        }

        if (ses_data->dbus_obj_path != NULL) {
            pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol,
                         ses_data->dbus_obj_path, session_interface_info.name) >= 0);
            pa_dbus_send_empty_reply(conn, msg);
            pa_xfree(ses_data->dbus_obj_path);
            ses_data->dbus_obj_path = NULL;
        }

        pa_xfree(ses_data);
        ses_data = NULL;
    }
    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_loopback_effect_create(DBusConnection *conn,
                                           DBusMessage *msg,
                                           void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    pa_qahw_effect_session_data *ses_data = NULL;
    pa_qahw_effect_info *effect = NULL;
    pa_qahw_effect_endpoint_info *loopback = NULL;
    pa_qahw_effect_callback_config *cb_config = NULL;
    pa_qahw_effect_status *status = NULL;

    DBusError error;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, struct_i;

    qahw_effect_uuid_t uuid;
    audio_patch_handle_t patch_handle;
    int rc = -1;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    char *loopback_name = NULL;
    char *obj_path;
    void *state;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("%s\n", __func__);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("loopback_effect_create has no arguments.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "loopback_effect_create has no arguments.");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(uqqqay)t")) {
        pa_log_error("Invalid signature for loopback_effect_create.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for loopback_effect_create.");
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
    dbus_message_iter_get_basic(&arg_i, &patch_handle);

    PA_HASHMAP_FOREACH(effect, m_data->effects, state)
        if (memcmp(&uuid, effect->uuid, sizeof(qahw_effect_uuid_t)) == 0)
            break;

    if (!effect) {
        pa_log_error("Unsupported UUID.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported UUID.");
        dbus_error_free(&error);
        return;
    }

    loopback_name = pa_qahw_loopback_get_name_from_handle(patch_handle);
    if (loopback_name == NULL) {
        pa_log_error("Invalid patch handle.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid patch handle.");
        dbus_error_free(&error);
        return;
    }

    if (effect->loopbacks != NULL)
        loopback = pa_hashmap_get(effect->loopbacks, loopback_name);

    if (loopback == NULL) {
        pa_log_error("Invalid patch handle.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid patch handle.");
        dbus_error_free(&error);
        return;
    }

    if (loopback->client_count == 0) {
        ses_data = pa_xnew0(pa_qahw_effect_session_data, 1);
        ses_data->handle = patch_handle;
        ses_data->lib_handle = qahw_effect_load_library(effect->lib_name);
        rc = qahw_effect_create(ses_data->lib_handle, &uuid, patch_handle, &ses_data->effect_handle);

        if (rc != 0) {
            pa_log_error("LoopbackEffectCreate failed\n");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_create failed.");
            dbus_error_free(&error);
            pa_xfree(ses_data);
            return;
        }

        cb_config = pa_hashmap_get(m_data->callbacks, loopback_name);
        if (cb_config != NULL) {
            status = pa_xnew0(pa_qahw_effect_status, 1);
            status->is_enabled = false;
            status->effect_name = pa_xstrdup(effect->name);
            if (cb_config->effects != NULL)
                pa_hashmap_put(cb_config->effects, effect->name, status);
        }

        ses_data->endpoint_name = pa_xstrdup(loopback_name);
        ses_data->effect_name = pa_xstrdup(effect->name);
        ses_data->endpoint = loopback;
        ses_data->common = m_data;
        ses_data->dbus_obj_path = pa_sprintf_malloc("%s/loopback_%u/effect_%d", m_data->dbus_obj_path, patch_handle, effect->index);

        loopback->client_count++;

        pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol,
                     ses_data->dbus_obj_path, &session_interface_info, ses_data) >= 0);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &ses_data->dbus_obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        dbus_message_unref(reply);
    } else {
        loopback->client_count++;
        obj_path = pa_sprintf_malloc("%s/loopback_%d/effect_%d", m_data->dbus_obj_path, patch_handle, effect->index);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        pa_xfree(obj_path);
        dbus_error_free(&error);
        dbus_message_unref(reply);
    }
}

static void pa_qahw_port_effect_create(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    pa_qahw_effect_session_data *ses_data = NULL;
    pa_qahw_effect_info *effect = NULL;
    pa_qahw_effect_endpoint_info *port = NULL;
    qahw_effect_uuid_t uuid;

    DBusError error;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, struct_i;

    int rc = -1;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    char *port_name = NULL;
    char *obj_path;
    void *state;

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

    if (effect->ports != NULL)
        port = pa_hashmap_get(effect->ports, port_name);

    if (!port) {
        pa_log_error("Invalid port name.\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid port name.");
        dbus_error_free(&error);
        return;
    }

    if (port->client_count == 0) {
        ses_data = pa_xnew0(pa_qahw_effect_session_data, 1);
        ses_data->handle = -1;
        ses_data->lib_handle = qahw_effect_load_library(effect->lib_name);
        rc = qahw_effect_create(ses_data->lib_handle, &uuid, ses_data->handle, &ses_data->effect_handle);

        if (rc != 0) {
            pa_log_error("PortEffectCreate failed\n");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_create failed.");
            dbus_error_free(&error);
            pa_xfree(ses_data);
            return;
        }

        ses_data->endpoint_name = pa_xstrdup(port_name);
        ses_data->effect_name = pa_xstrdup(effect->name);
        ses_data->endpoint = port;
        ses_data->common = m_data;
        ses_data->dbus_obj_path = pa_sprintf_malloc("%s/port_%s/effect_%d", m_data->dbus_obj_path, port_name, effect->index);
        pa_hashmap_put(m_data->sessions, ses_data->dbus_obj_path, ses_data);

        port->client_count++;

        pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol,
                     ses_data->dbus_obj_path, &session_interface_info, ses_data) >= 0);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &ses_data->dbus_obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        dbus_message_unref(reply);
    } else {
        port->client_count++;
        obj_path = pa_sprintf_malloc("%s/port_%s/effect_%d", m_data->dbus_obj_path, port_name, effect->index);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        pa_xfree(obj_path);
        dbus_error_free(&error);
        dbus_message_unref(reply);
    }
}

static void pa_qahw_sink_effect_create(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    pa_qahw_effect_module_data *m_data = (pa_qahw_effect_module_data *)userdata;
    pa_qahw_effect_session_data *ses_data;
    pa_qahw_effect_info *effect = NULL;
    pa_qahw_effect_endpoint_info *sink = NULL;
    qahw_effect_uuid_t uuid;

    DBusError error;
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
    audio_io_handle_t handle;

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
        if (effect->sinks != NULL)
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
        handle = pa_qahw_sink_get_io_handle(sink_id);
        if (handle < 0) {
            pa_log_error("%s: Unable to retrieve sink io handle for sink id: %d\n", __func__, sink_id);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unable to retrieve sink io handle.");
            dbus_error_free(&error);
            return;
        }
        ses_data = pa_xnew0(pa_qahw_effect_session_data, 1);
        ses_data->handle = handle;

        ses_data->lib_handle = qahw_effect_load_library(effect->lib_name);
        rc = qahw_effect_create(ses_data->lib_handle, &uuid, ses_data->handle, &ses_data->effect_handle);

        if (rc != 0) {
            pa_log_error("SinkEffectCreate failed\n");
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "qahw_effect_create failed.");
            dbus_error_free(&error);
            pa_xfree(ses_data);
            return;
        }

        ses_data->endpoint_name = pa_xstrdup(sink_name);
        ses_data->effect_name = pa_xstrdup(effect->name);
        ses_data->endpoint = sink;
        ses_data->common = m_data;
        ses_data->dbus_obj_path = pa_sprintf_malloc("%s/sink_%d/effect_%d", m_data->dbus_obj_path, sink_id, effect->index);
        pa_hashmap_put(m_data->sessions, ses_data->dbus_obj_path, ses_data);

        sink->client_count++;

        pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol,
                     ses_data->dbus_obj_path, &session_interface_info, ses_data) >= 0);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &ses_data->dbus_obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        dbus_message_unref(reply);
    } else {
        sink->client_count++;
        obj_path = pa_sprintf_malloc("%s/sink_%d/effect_%d", m_data->dbus_obj_path, sink_id, effect->index);
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &arg_i);
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &obj_path);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        pa_xfree(obj_path);
        dbus_error_free(&error);
        dbus_message_unref(reply);
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

    PA_HASHMAP_FOREACH(effect, m_data->effects, state) {
        if ((effect->ports != NULL) && (m_data->card->ports != NULL))
                if (pa_hashmap_get(effect->ports, port_name)
                    && pa_hashmap_get(m_data->card->ports, port_name))
                    port_effects++;
    }

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
    PA_HASHMAP_FOREACH(effect, m_data->effects, state) {
        if (effect->sinks != NULL)
            if (pa_hashmap_get(effect->sinks, sink_name))
                sink_effects++;
    }

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

static void pa_qahw_effect_free_callback(pa_qahw_effect_callback_config *cb) {
    pa_assert(cb);

    pa_log_info("Freeing callback info\n");

    if (cb != NULL) {
        if (cb->endpoint_name != NULL)
            pa_xfree(cb->endpoint_name);

        if (cb->effects != NULL)
            pa_hashmap_free(cb->effects);

        if (cb->prv_data != NULL) {
            pa_xfree(cb->prv_data);
            cb->prv_data = NULL;
        }

        cb->cb_func = NULL;
        pa_xfree(cb);
    }
}

static void pa_qahw_free_session(pa_qahw_effect_session_data *session) {
    pa_assert(session);

    pa_log_info("%s: freeing session", __func__);

    if (session != NULL) {
        if ((session->lib_handle != NULL) && (session->effect_handle != NULL)) {
            qahw_effect_release(session->lib_handle, session->effect_handle);
            qahw_effect_unload_library(session->lib_handle);
            session->lib_handle = NULL;
            session->effect_handle = NULL;
        }

        if (session->dbus_obj_path != NULL) {
            pa_assert_se(pa_dbus_protocol_remove_interface(session->common->dbus_protocol,
                         session->dbus_obj_path, session_interface_info.name) >= 0);
            pa_xfree(session->dbus_obj_path);
            session->dbus_obj_path = NULL;
        }

        if (session->endpoint_name != NULL) {
            pa_xfree(session->endpoint_name);
            session->endpoint_name = NULL;
        }

        if (session->effect_name != NULL) {
            pa_xfree(session->effect_name);
            session->effect_name = NULL;
        }

        pa_xfree(session);
        session = NULL;
    }
}

static void pa_qahw_effect_free_endpoint(pa_qahw_effect_endpoint_info *endpoint) {
    pa_assert(endpoint);

    pa_log_info("%s: freeing endpoint of type %d", __func__, endpoint->type);

    if (endpoint != NULL) {
        if (endpoint->name != NULL) {
            pa_xfree(endpoint->name);
            endpoint->name = NULL;
        }

        pa_xfree(endpoint);
        endpoint = NULL;
    }
};

static void pa_qahw_effect_free(pa_qahw_effect_info *effect) {
    pa_assert(effect);

    pa_log_info("%s: freeing effect %s", __func__, effect->name);

    if (effect->sinks) {
        pa_hashmap_free(effect->sinks);
        effect->sinks = NULL;
    }

    if (effect->ports) {
        pa_hashmap_free(effect->ports);
        effect->ports = NULL;
    }

    if (effect->loopbacks) {
        pa_hashmap_free(effect->loopbacks);
        effect->loopbacks = NULL;
    }

    if (effect->name != NULL) {
        pa_xfree(effect->name);
        effect->name = NULL;
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
    pa_qahw_loopback_config *loopback_config;

    pa_qahw_effect_info *effect_info;
    pa_qahw_effect_endpoint_info *sink = NULL;
    pa_qahw_effect_endpoint_info *port = NULL;
    pa_qahw_effect_endpoint_info *loopback = NULL;

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
        effect_info = pa_xnew0(pa_qahw_effect_info, 1);
        effect_info->name = pa_xstrdup(effect_config->name);
        effect_info->index = i++;
        effect_info->sinks = NULL;
        effect_info->ports = NULL;
        effect_info->loopbacks = NULL;
        state1 = NULL;

        if (!pa_hashmap_isempty(effect_config->sinks)) {
            effect_info->sinks = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                     pa_idxset_string_compare_func, NULL,
                                                     (pa_free_cb_t) pa_qahw_effect_free_endpoint);
            PA_HASHMAP_FOREACH(sink_config, effect_config->sinks, state1) {
                sink = pa_xnew0(pa_qahw_effect_endpoint_info, 1);
                sink->name =  pa_xstrdup(sink_config->name);
                sink->type = PA_QAHW_EFFECT_TYPE_SINK;
                pa_hashmap_put(effect_info->sinks, sink->name, sink);
                pa_log_info("%s: Adding effect %s of type %d to sink %s", __func__,
                            effect_info->name, sink->type, sink->name);
            }
            state1 = NULL;
        }

        if (!pa_hashmap_isempty(effect_config->ports)) {
            effect_info->ports = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                     pa_idxset_string_compare_func, NULL,
                                                     (pa_free_cb_t) pa_qahw_effect_free_endpoint);
            PA_HASHMAP_FOREACH(port_config, effect_config->ports, state1) {
                port = pa_xnew0(pa_qahw_effect_endpoint_info, 1);
                port->name =  pa_xstrdup(port_config->name);
                port->type = PA_QAHW_EFFECT_TYPE_PORT;
                pa_hashmap_put(effect_info->ports, port->name, port);
                pa_log_info("%s: Adding effect %s of type %d to port %s", __func__,
                            effect_info->name, port->type, port->name);
            }
            state1 = NULL;
        }

        if (!pa_hashmap_isempty(effect_config->loopbacks)) {
            effect_info->loopbacks = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                         pa_idxset_string_compare_func, NULL,
                                                         (pa_free_cb_t) pa_qahw_effect_free_endpoint);
            PA_HASHMAP_FOREACH(loopback_config, effect_config->loopbacks, state1) {
                loopback = pa_xnew0(pa_qahw_effect_endpoint_info, 1);
                loopback->name = pa_xstrdup(loopback_config->name);
                loopback->type = PA_QAHW_EFFECT_TYPE_LOOPBACK;
                pa_hashmap_put(effect_info->loopbacks, loopback->name, loopback);
                pa_log_info("%s: Adding effect %s of type %d to loopback %s", __func__,
                            effect_info->name, loopback->type, loopback->name);
            }
            state1 = NULL;
        }

        effect_info->uuid = pa_xnew0(qahw_effect_uuid_t, 1);

        if (pa_qahw_effect_string_to_uuid(effect_config->uuid, effect_info->uuid) != 0)
            pa_log_error("%s: Error parsing uuid for %s\n", __func__, effect_config->name);

        effect_info->lib_name = pa_xstrdup(effect_config->lib_name);

        pa_hashmap_put(effect_mdata->effects, effect_info->name, effect_info);
    }

    effect_mdata->card = card;
    effect_mdata->callbacks = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                  pa_idxset_string_compare_func, NULL,
                                                  (pa_free_cb_t)pa_qahw_effect_free_callback);

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

    if (effect_mdata->effects) {
        pa_hashmap_free(effect_mdata->effects);
        effect_mdata->effects = NULL;
    }

    if (effect_mdata->sessions) {
        pa_hashmap_free(effect_mdata->sessions);
        effect_mdata->sessions = NULL;
    }

    if (effect_mdata->callbacks) {
        pa_hashmap_free(effect_mdata->callbacks);
        effect_mdata->callbacks = NULL;
    }

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
