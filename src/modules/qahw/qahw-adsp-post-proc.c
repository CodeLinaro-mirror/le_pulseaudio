/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 *
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

#include "qahw-adsp-post-proc.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <pulsecore/thread.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/poll.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/core-error.h>

#ifdef QAHW_AUDIO_ADSP_PP_ENABLED

#include <qahw_post_proc_api.h>
#include <qahw_effect_api.h>

#include "qahw-utils.h"
#include "qahw-effect.h"

#define QAHW_POST_PROC_MODULE_IFACE "org.PulseAudio.Ext.QAHW.PostProc"
#define QAHW_POST_PROC_MODULE_OBJECT_PATH "/org/pulseaudio/ext/qahw/postproc"

#define QAHW_POST_PROC_SESSION_IFACE "org.PulseAudio.Ext.QAHW.PostProc.Session"

#define POST_PROC_LIB_NAME "libadsppostproc.so"
#define MAX_BUFFER_SIZE_BYTES 65496

//#define POST_PROC_DEBUG

typedef struct {
    char *name;
    qahw_effect_uuid_t *uuid;
    char *uuid_str;
    char *lib_name;
} pa_qahw_post_proc_effect_info_t;

typedef struct {
    char *name;
    uint32_t topology_id;
    char *topology_id_str;
    uint32_t app_type;
    pa_hashmap *effect_infos;
    uint32_t latency_us;
} pa_qahw_post_proc_topology_info_t;

typedef struct {
    DBusConnection *conn;
    DBusMessage *msg;
    void *userdata;
} pa_qahw_dbus_conn_data_t;

/* module level data */
typedef struct {
    pa_msgobject parent;

    pa_dbus_protocol *dbus_protocol;    /* Dbus protocol */
    pa_core *core;

    pa_hashmap *topology_infos;             /* Hashmap containing all topologies */
    pa_hashmap *sessions;               /* Hashmap containing all active post proc sessions */

    qahw_post_proc_module_handle_t *module_handle;

    pa_thread *dbus_hdlr_thread;
    pa_rtpoll *dbus_hdlr_rtpoll;
    pa_thread_mq dbus_hdlr_thread_mq;

    uint32_t counter;
} pa_qahw_post_proc_module_data_t;

/* post proc session specific data */
typedef struct {
    pa_qahw_post_proc_module_data_t *mdata;
    uint32_t session_id;

    pa_qahw_topology_config topology_config;

    uint32_t topology_id;
    uint32_t app_type;

    uint32_t latency_us;

    qahw_post_proc_stream_handle_t *stream_handle;
    uint32_t popp_id;

    qahw_post_proc_buffer_config_t input_buf_config;
    qahw_post_proc_buffer_config_t output_buf_config;

    char *session_object_path;

    char* input_file_name;
    char* output_file_name;

    int input_fd;
    int output_fd;

    pa_thread *io_thread;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_item;
    pa_thread_mq thread_mq;

    pa_hashmap *effect_sessions;
} pa_qahw_post_proc_session_data_t;

typedef struct {
    qahw_effect_lib_handle_t lib_handle;    /* Effect lib handle */
    qahw_effect_handle_t effect_handle;     /* Effect handle */
} pa_qahw_post_proc_effect_session_data_t;

PA_DEFINE_PRIVATE_CLASS(pa_qahw_post_proc_module_data_t, pa_msgobject);

static void pa_qahw_fill_topology_infos (pa_qahw_post_proc_module_data_t *mdata, pa_hashmap *topology_configs);
static char* pa_qahw_post_proc_get_string_from_uuid(qahw_effect_uuid_t *uuid);
static void pa_qahw_post_proc_dbus_hdlr_thread_func (void *userdata);
static int pa_qahw_dbus_hdlr_process_msg (pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk);
static pa_qahw_dbus_conn_data_t *pa_qahw_create_dbus_conn_data (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_free_dbus_conn_data (pa_qahw_dbus_conn_data_t *dbus_conn_data);
static void pa_qahw_get_supported_topologies_async (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_get_supported_topologies (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_fill_effect_uuid(DBusMessageIter *struct_iter, qahw_effect_uuid_t *uuid);
static void pa_qahw_create_post_proc_session_async (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_create_post_proc_session (DBusConnection *conn, DBusMessage *msg, void *userdata);
static pa_qahw_post_proc_session_data_t *pa_qahw_open_session (pa_qahw_post_proc_module_data_t *mdata, uint32_t topology_id, uint32_t app_type,
                                            qahw_post_proc_buffer_config_t input_buf_config, qahw_post_proc_buffer_config_t output_buf_config);
static int pa_qahw_create_and_open_io_files(pa_qahw_post_proc_session_data_t *sdata);
static void pa_qahw_post_proc_io_thread_func(void *userdata);
static int pa_qahw_write_output (pa_qahw_post_proc_session_data_t *sdata, pa_memchunk *memchunk, int *write_type);
static void pa_qahw_post_proc_effect_command (DBusConnection *conn, DBusMessage *msg, void *userdata, uint32_t command_code);
static void pa_qahw_post_proc_get_param_async (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_post_proc_get_param (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_post_proc_set_param_async (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_post_proc_set_param (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_post_proc_set_persist_async (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_post_proc_set_persist (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_post_proc_release_session_async (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_post_proc_release_session (DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_close_session (pa_qahw_post_proc_session_data_t *sdata);
static void pa_qahw_post_proc_free_topology_info(pa_qahw_post_proc_topology_info_t *topology_info);
static void pa_qahw_post_proc_free_effect_info(pa_qahw_post_proc_effect_info_t *effect_info);
static void pa_qahw_post_proc_free_effect_session(pa_qahw_post_proc_effect_session_data_t *effect_sdata);

enum module_handler_index {
    MODULE_HANDLER_GET_SUPPORTED_TOPOLOGIES,
    MODULE_HANDLER_CREATE_SESSION,
    MODULE_HANDLER_MAX
};

pa_dbus_arg_info create_session_args[] = {
    {"topology_id", "u", "in"},
    {"app_type", "u", "in"},
    {"in_config", "uus", "in"},
    {"out_config", "uus", "in"},
    {"in_file", "s", "out"},
    {"out_file", "s", "out"},
    {"session_object_path", "o", "out"},
    {"latency_us", "u", "out"}
};

pa_dbus_arg_info get_supported_topologies_args[] = {
    {"n_topologies", "u", "out"},
    {"topology_infos", "a(suua(s(uqqqay)))", "out"}
};

static pa_dbus_method_handler post_proc_module_handlers[MODULE_HANDLER_MAX] = {
    [MODULE_HANDLER_GET_SUPPORTED_TOPOLOGIES] = {
        .method_name = "GetSupportedTopologies",
        .arguments = get_supported_topologies_args,
        .n_arguments = sizeof(get_supported_topologies_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_get_supported_topologies_async},
    [MODULE_HANDLER_CREATE_SESSION] = {
        .method_name = "CreateSession",
        .arguments = create_session_args,
        .n_arguments = sizeof(create_session_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_create_post_proc_session_async}
};

static pa_dbus_interface_info module_interface_info = {
    .name = QAHW_POST_PROC_MODULE_IFACE,
    .method_handlers = post_proc_module_handlers,
    .n_method_handlers = MODULE_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

enum session_handler_index {
    SESSION_GET_PARAM,
    SESSION_SET_PARAM,
    SESSION_SET_PERSIST,
    SESSION_HANDLER_RELEASE,
    SESSION_HANDLER_MAX
};

pa_dbus_arg_info release_session_args[] = {
};

pa_dbus_arg_info get_param_args[] = {
    {"uuid", "(uqqqay)", "in"},
    {"command_size", "u", "in"},
    {"reply_size", "u", "in"},
    {"command_data", "ay", "in"},
    {"reply_data", "ay", "out"}
};

pa_dbus_arg_info set_param_args[] = {
    {"uuid", "(uqqqay)", "in"},
    {"command_size", "u", "in"},
    {"reply_size", "u", "in"},
    {"command_data", "ay", "in"},
    {"reply_data", "ay", "out"}
};

pa_dbus_arg_info set_persist_args[] = {
    {"uuid", "(uqqqay)", "in"},
    {"persist", "b", "in"}
};

static pa_dbus_method_handler post_proc_session_handlers[SESSION_HANDLER_MAX] = {
    [SESSION_GET_PARAM] = {
        .method_name = "GetParam",
        .arguments = get_param_args,
        .n_arguments = sizeof(get_param_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_post_proc_get_param_async},
    [SESSION_SET_PARAM] = {
        .method_name = "SetParam",
        .arguments = set_param_args,
        .n_arguments = sizeof(set_param_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_post_proc_set_param_async},
    [SESSION_SET_PERSIST] = {
        .method_name = "SetPersist",
        .arguments = set_persist_args,
        .n_arguments = sizeof(set_persist_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_post_proc_set_persist_async},
    [SESSION_HANDLER_RELEASE] = {
        .method_name = "ReleaseSession",
        .arguments = release_session_args,
        .n_arguments = sizeof(release_session_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_post_proc_release_session_async}
};

static pa_dbus_interface_info session_interface_info = {
    .name = QAHW_POST_PROC_SESSION_IFACE,
    .method_handlers = post_proc_session_handlers,
    .n_method_handlers = SESSION_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

typedef enum {
    PA_QAHW_POST_PROC_MSG_GET_SUPPORTED_TOPOLOGIES,
    PA_QAHW_POST_PROC_MSG_CREATE_SESSION,
    PA_QAHW_POST_PROC_MSG_GET_PARAM,
    PA_QAHW_POST_PROC_MSG_SET_PARAM,
    PA_QAHW_POST_PROC_MSG_SET_PERSIST,
    PA_QAHW_POST_PROC_MSG_RELEASE_SESSION
} pa_qahw_dbus_hdlr_msg_t;

pa_qahw_post_proc_handle_t pa_qahw_post_proc_module_init (pa_core *core, pa_dbus_protocol *dbus_protocol,
                                                            pa_hashmap *topology_configs) {
    pa_qahw_post_proc_module_data_t *mdata;

    char* thread_name = NULL;

    pa_assert(topology_configs);

    mdata = pa_msgobject_new(pa_qahw_post_proc_module_data_t);

    mdata->dbus_protocol = dbus_protocol;
    mdata->core = core;
    mdata->counter = 0;

    pa_qahw_fill_topology_infos(mdata, topology_configs);

    if (mdata->topology_infos == NULL || pa_hashmap_isempty(mdata->topology_infos)) {
        pa_log_error("%s: No valid topology found from conf file", __func__);
        goto fail;
    }

    mdata->module_handle = qahw_post_proc_load_module(POST_PROC_LIB_NAME);
    if (mdata->module_handle == NULL) {
        pa_log_error("%s: Failed to load HAL module", __func__);
        goto fail;
    }

    mdata->dbus_hdlr_rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&mdata->dbus_hdlr_thread_mq, mdata->core->mainloop, mdata->dbus_hdlr_rtpoll);

    thread_name = pa_sprintf_malloc("post_proc_dbus_hdlr");
    if ((mdata->dbus_hdlr_thread = pa_thread_new(thread_name,
            pa_qahw_post_proc_dbus_hdlr_thread_func, mdata)) == NULL) {
        pa_log_error("%s: Could not spawn DBus Hanlder Thread", __func__);
        goto fail;
    }
    pa_xfree(thread_name);

    mdata->parent.process_msg = pa_qahw_dbus_hdlr_process_msg;

    pa_assert_se(pa_dbus_protocol_add_interface(mdata->dbus_protocol,
             QAHW_POST_PROC_MODULE_OBJECT_PATH, &module_interface_info, mdata) >= 0);

    return mdata;

fail:
    if (thread_name) pa_xfree(thread_name);
    if (mdata->topology_infos) pa_hashmap_free(mdata->topology_infos);
    if (mdata->dbus_hdlr_thread) pa_thread_free(mdata->dbus_hdlr_thread);
    if (mdata->dbus_hdlr_rtpoll) pa_rtpoll_free(mdata->dbus_hdlr_rtpoll);
    pa_thread_mq_done(&mdata->dbus_hdlr_thread_mq);
    if (mdata->module_handle) qahw_post_proc_unload_module(mdata->module_handle);
    pa_xfree(mdata);

    return NULL;
}

static void pa_qahw_fill_topology_infos (pa_qahw_post_proc_module_data_t *mdata, pa_hashmap *topology_configs) {
    pa_qahw_topology_config *topology_config;
    pa_qahw_effect_config *effect_config;

    pa_qahw_post_proc_topology_info_t *topology_info;
    pa_qahw_post_proc_effect_info_t *effect_info;

    void *state = NULL;
    void *state1;

    if (!pa_hashmap_isempty(topology_configs)) {
        mdata->topology_infos = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                         pa_idxset_string_compare_func, NULL,
                                                         (pa_free_cb_t) pa_qahw_post_proc_free_topology_info);

        PA_HASHMAP_FOREACH(topology_config, topology_configs, state) {
            topology_info = pa_xnew0(pa_qahw_post_proc_topology_info_t, 1);
            topology_info->name = pa_xstrdup(topology_config->name);
            topology_info->topology_id = topology_config->topology_id;
            topology_info->app_type = topology_config->app_type;
            topology_info->latency_us = topology_config->latency_us;
            topology_info->effect_infos = NULL;

            if (!pa_hashmap_isempty(topology_config->effect_configs)) {
                topology_info->effect_infos = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                         pa_idxset_string_compare_func, NULL,
                                                         (pa_free_cb_t) pa_qahw_post_proc_free_effect_info);

                state1 = NULL;
                PA_HASHMAP_FOREACH(effect_config, topology_config->effect_configs, state1) {
                    effect_info = pa_xnew0(pa_qahw_post_proc_effect_info_t, 1);
                    effect_info->name =  pa_xstrdup(effect_config->name);
                    effect_info->lib_name = pa_xstrdup(effect_config->lib_name);
                    effect_info->uuid_str = pa_xstrdup(effect_config->uuid);

                    effect_info->uuid = pa_xnew0(qahw_effect_uuid_t, 1);
                    if (pa_qahw_effect_string_to_uuid(effect_config->uuid, effect_info->uuid) != 0) {
                        pa_log_error("%s: Failed to parse uuid for effect %s", __func__, effect_config->name);
                        pa_qahw_post_proc_free_effect_info(effect_info);
                        continue;
                    }

                    pa_log_debug("%s: Adding effect %s (%s) to topology %s", __func__, effect_info->name,
                                    effect_info->uuid_str, topology_info->name);
                    pa_hashmap_put(topology_info->effect_infos, effect_info->uuid_str, effect_info);
                }
            } else {
                pa_log_error("%s: No effect found for topology %s", __func__, topology_config->name);
                pa_qahw_post_proc_free_topology_info(topology_info);
                continue;
            }

            topology_info->topology_id_str = pa_sprintf_malloc("%u", topology_info->topology_id);

            pa_log_debug("%s: Adding topology %s (%u) to topology_info list", __func__,
                            topology_info->name, topology_info->topology_id);
            pa_hashmap_put(mdata->topology_infos, topology_info->topology_id_str, topology_info);
        }
    } else {
        pa_log_error("%s: No topology found", __func__);
    }
}

static char* pa_qahw_post_proc_get_string_from_uuid (qahw_effect_uuid_t *uuid) {
    char *uuid_str = NULL;

    uuid_str = pa_sprintf_malloc("%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x", uuid->timeLow, uuid->timeMid,
                        uuid->timeHiAndVersion, uuid->clockSeq,
                        uuid->node[0], uuid->node[1], uuid->node[2], uuid->node[3], uuid->node[4], uuid->node[5]);
    return uuid_str;
}

static void pa_qahw_post_proc_dbus_hdlr_thread_func (void *userdata) {
    pa_qahw_post_proc_module_data_t *mdata = (pa_qahw_post_proc_module_data_t *) userdata;

    if ((mdata->core->realtime_scheduling)) {
        pa_log_info("%s:: Making dbus handler thread as realtime with prio %d", __func__,
                        mdata->core->realtime_priority);
        pa_make_realtime(mdata->core->realtime_priority);
    }

    pa_thread_mq_install(&mdata->dbus_hdlr_thread_mq);

    for (;;) {
        int ret = 0;

        /* nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(mdata->dbus_hdlr_rtpoll, true)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_wait_for(mdata->dbus_hdlr_thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Dbus Hanlder Thread shutting down");
}


static int pa_qahw_dbus_hdlr_process_msg (pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    pa_qahw_dbus_conn_data_t *dbus_conn_data;

    pa_log_debug ("%s: msg = %d", __func__, code);

    dbus_conn_data = (pa_qahw_dbus_conn_data_t *) data;

    switch(code) {
        case PA_QAHW_POST_PROC_MSG_GET_SUPPORTED_TOPOLOGIES:
            pa_qahw_get_supported_topologies(dbus_conn_data->conn, dbus_conn_data->msg, dbus_conn_data->userdata);
            break;
        case PA_QAHW_POST_PROC_MSG_CREATE_SESSION:
            pa_qahw_create_post_proc_session(dbus_conn_data->conn, dbus_conn_data->msg, dbus_conn_data->userdata);
            break;
        case PA_QAHW_POST_PROC_MSG_GET_PARAM:
            pa_qahw_post_proc_get_param(dbus_conn_data->conn, dbus_conn_data->msg, dbus_conn_data->userdata);
            break;
        case PA_QAHW_POST_PROC_MSG_SET_PARAM:
            pa_qahw_post_proc_set_param(dbus_conn_data->conn, dbus_conn_data->msg, dbus_conn_data->userdata);
            break;
        case PA_QAHW_POST_PROC_MSG_SET_PERSIST:
            pa_qahw_post_proc_set_persist(dbus_conn_data->conn, dbus_conn_data->msg, dbus_conn_data->userdata);
            break;
        case PA_QAHW_POST_PROC_MSG_RELEASE_SESSION:
            pa_qahw_post_proc_release_session(dbus_conn_data->conn, dbus_conn_data->msg, dbus_conn_data->userdata);
            break;
    }

    pa_qahw_free_dbus_conn_data(dbus_conn_data);
    return 0;
}

static pa_qahw_dbus_conn_data_t *pa_qahw_create_dbus_conn_data (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_dbus_conn_data_t *dbus_conn_data;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_connection_ref(conn);
    dbus_message_ref(msg);

    dbus_conn_data = pa_xnew0(pa_qahw_dbus_conn_data_t, 1);
    dbus_conn_data->conn = conn;
    dbus_conn_data->msg = msg;
    dbus_conn_data->userdata = userdata;

    return dbus_conn_data;
}

static void pa_qahw_free_dbus_conn_data (pa_qahw_dbus_conn_data_t *dbus_conn_data) {
    dbus_connection_unref(dbus_conn_data->conn);
    dbus_message_unref(dbus_conn_data->msg);
    pa_xfree(dbus_conn_data);
}

static void pa_qahw_get_supported_topologies_async (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_module_data_t *mdata;
    pa_qahw_dbus_conn_data_t *dbus_conn_data;

    pa_assert(userdata);

    mdata = (pa_qahw_post_proc_module_data_t *) userdata;
    dbus_conn_data = pa_qahw_create_dbus_conn_data(conn, msg, userdata);

    pa_asyncmsgq_post(mdata->dbus_hdlr_thread_mq.inq, PA_MSGOBJECT(mdata),
                        PA_QAHW_POST_PROC_MSG_GET_SUPPORTED_TOPOLOGIES, dbus_conn_data, 0, NULL, NULL);
}

static void pa_qahw_get_supported_topologies (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_module_data_t *mdata;
    pa_qahw_post_proc_topology_info_t *topology_info;
    pa_qahw_post_proc_effect_info_t *effect_info;

    uint32_t n_topologies;

    DBusMessageIter iter, topology_array_iter, topology_struct_iter;
    DBusMessageIter effect_array_iter, effect_struct_iter, uuid_struct_iter;
    DBusError error;
    DBusMessage *reply = NULL;
    void *state = NULL;
    void *state1;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    mdata = (pa_qahw_post_proc_module_data_t *) userdata;

    pa_log_debug("%s", __func__);

    dbus_error_init(&error);

    if (!pa_streq(dbus_message_get_signature(msg), "")) {
        pa_log_error("%s: Invalid signature for GetSupportedTopologies", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for sink_get_supported_efffects.");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &iter);

    n_topologies = pa_hashmap_size(mdata->topology_infos);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &n_topologies);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(suua(s(uqqqay)))", &topology_array_iter);

    PA_HASHMAP_FOREACH(topology_info, mdata->topology_infos, state) {
        dbus_message_iter_open_container(&topology_array_iter, DBUS_TYPE_STRUCT, NULL, &topology_struct_iter);

        dbus_message_iter_append_basic(&topology_struct_iter, DBUS_TYPE_STRING, &topology_info->name);
        dbus_message_iter_append_basic(&topology_struct_iter, DBUS_TYPE_UINT32, &topology_info->topology_id);
        dbus_message_iter_append_basic(&topology_struct_iter, DBUS_TYPE_UINT32, &topology_info->app_type);

        dbus_message_iter_open_container(&topology_struct_iter, DBUS_TYPE_ARRAY, "(s(uqqqay))", &effect_array_iter);

        state1 = NULL;
        PA_HASHMAP_FOREACH(effect_info, topology_info->effect_infos, state1) {
            dbus_message_iter_open_container(&effect_array_iter, DBUS_TYPE_STRUCT, NULL, &effect_struct_iter);
            dbus_message_iter_append_basic(&effect_struct_iter, DBUS_TYPE_STRING, &effect_info->name);
            dbus_message_iter_open_container(&effect_struct_iter, DBUS_TYPE_STRUCT, NULL, &uuid_struct_iter);
            pa_qahw_fill_effect_uuid(&uuid_struct_iter, effect_info->uuid);
            dbus_message_iter_close_container(&effect_struct_iter, &uuid_struct_iter);
            dbus_message_iter_close_container(&effect_array_iter, &effect_struct_iter);
        }

        dbus_message_iter_close_container(&topology_struct_iter, &effect_array_iter);
        dbus_message_iter_close_container(&topology_array_iter, &topology_struct_iter);
    }

    dbus_message_iter_close_container(&iter, &topology_array_iter);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_error_free(&error);
    dbus_message_unref(reply);
}

static void pa_qahw_fill_effect_uuid(DBusMessageIter *struct_iter, qahw_effect_uuid_t *uuid) {
    DBusMessageIter array_iter;
    int i = 0;

    dbus_message_iter_append_basic(struct_iter, DBUS_TYPE_UINT32, &uuid->timeLow);
    dbus_message_iter_append_basic(struct_iter, DBUS_TYPE_UINT16, &uuid->timeMid);
    dbus_message_iter_append_basic(struct_iter, DBUS_TYPE_UINT16, &uuid->timeHiAndVersion);
    dbus_message_iter_append_basic(struct_iter, DBUS_TYPE_UINT16, &uuid->clockSeq);
    dbus_message_iter_open_container(struct_iter, DBUS_TYPE_ARRAY, "y", &array_iter);

    for (i = 0; i < 6; i++)
        dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, &uuid->node[i]);

    dbus_message_iter_close_container(struct_iter, &array_iter);
}

static void pa_qahw_create_post_proc_session_async (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_module_data_t *mdata;
    pa_qahw_dbus_conn_data_t *dbus_conn_data;

    pa_assert(userdata);

    mdata = (pa_qahw_post_proc_module_data_t *) userdata;
    dbus_conn_data = pa_qahw_create_dbus_conn_data(conn, msg, userdata);

    pa_asyncmsgq_post(mdata->dbus_hdlr_thread_mq.inq, PA_MSGOBJECT(mdata),
                        PA_QAHW_POST_PROC_MSG_CREATE_SESSION, dbus_conn_data, 0, NULL, NULL);
}

static void pa_qahw_create_post_proc_session (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_module_data_t *mdata;
    pa_qahw_post_proc_session_data_t *sdata;

    DBusMessageIter iter;
    DBusError error;
    DBusMessage *reply = NULL;

    char *format_str = NULL;
    char *topology_id_str;

    pa_qahw_post_proc_topology_info_t *topology_info;

    uint32_t topology_id;
    uint32_t app_type;
    qahw_post_proc_buffer_config_t input_buf_config;
    qahw_post_proc_buffer_config_t output_buf_config;

    pa_sample_format_t pa_format;;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    mdata = (pa_qahw_post_proc_module_data_t *) userdata;

    pa_log_debug("%s", __func__);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &iter)) {
        pa_log_error("%s: No arguments for CreateSession", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "No arguments for CreateSession");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "uuuusuus")) {
        pa_log_error("%s: Invalid signature for CreateSession", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for CreateSession");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_get_basic(&iter, &topology_id);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &app_type);

    topology_id_str = pa_sprintf_malloc("%u", topology_id);
    topology_info = pa_hashmap_get(mdata->topology_infos, topology_id_str);
    pa_xfree(topology_id_str);

    if (topology_info == NULL) {
        pa_log_error("%s: Topology with id %s not found", __func__, topology_id_str);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Topology info not found");
        return;
    }

    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &input_buf_config.sampling_rate);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &input_buf_config.channels);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &format_str);

    pa_format = pa_parse_sample_format(format_str);
    input_buf_config.format = pa_qahw_util_get_qahw_format_from_pa_sample(pa_format);
    if(!pa_sample_format_valid(input_buf_config.format)) {
        pa_log_error("%s: Unsupported input format", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported input format");
        dbus_error_free(&error);
        return;
    }

    input_buf_config.max_frame_count = MAX_BUFFER_SIZE_BYTES / (input_buf_config.channels *
                                                    pa_sample_size_of_format(pa_format));

    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &output_buf_config.sampling_rate);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &output_buf_config.channels);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &format_str);

    pa_format = pa_parse_sample_format(format_str);
    output_buf_config.format = pa_qahw_util_get_qahw_format_from_pa_sample(pa_format);
    if(!pa_sample_format_valid(output_buf_config.format)) {
        pa_log_error("%s: Unsupported output format", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Unsupported output format");
        dbus_error_free(&error);
        return;
    }

    output_buf_config.max_frame_count = MAX_BUFFER_SIZE_BYTES / (output_buf_config.channels *
                                                    pa_sample_size_of_format(pa_format))    ;

    sdata = pa_qahw_open_session(mdata, topology_id, app_type, input_buf_config, output_buf_config);

    if (sdata) {
        sdata->session_object_path = pa_sprintf_malloc("%s/session_%u", QAHW_POST_PROC_MODULE_OBJECT_PATH, sdata->session_id);

        pa_assert_se(pa_dbus_protocol_add_interface(mdata->dbus_protocol,
                     sdata->session_object_path, &session_interface_info, sdata) >= 0);

        sdata->latency_us = topology_info->latency_us;

        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &sdata->input_file_name);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &sdata->output_file_name);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sdata->session_object_path);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &sdata->latency_us);

        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        dbus_error_free(&error);
        dbus_message_unref(reply);
    } else {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Opening PP HAL session failed");
        dbus_error_free(&error);
    }
}

pa_qahw_post_proc_session_data_t *pa_qahw_open_session (pa_qahw_post_proc_module_data_t *mdata,
                                                        uint32_t topology_id, uint32_t app_type,
                                                        qahw_post_proc_buffer_config_t input_buf_config,
                                                        qahw_post_proc_buffer_config_t output_buf_config) {
    pa_qahw_post_proc_session_data_t *sdata;
    struct pollfd *pollfd;
    char *thread_name;

    pa_log_debug("%s: app_type %u, topology_id %u", __func__, app_type, topology_id);

    sdata = pa_xnew0(pa_qahw_post_proc_session_data_t, 1);
    sdata->mdata = mdata;
    sdata->session_id = mdata->counter++;
    sdata->topology_id = topology_id;
    sdata->app_type = app_type;
    sdata->input_buf_config = input_buf_config;
    sdata->output_buf_config = output_buf_config;

    if (pa_qahw_create_and_open_io_files(sdata) < 0) {
        pa_log_error("%s: Could not create IO files", __func__);
        goto fail;
    }

    sdata->rtpoll = pa_rtpoll_new();
    sdata->rtpoll_item = pa_rtpoll_item_new(sdata->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(sdata->rtpoll_item, NULL);
    pollfd->fd = sdata->input_fd;
    pollfd->events = pollfd->revents = 0;

    pa_thread_mq_init(&sdata->thread_mq, mdata->core->mainloop, sdata->rtpoll);

    thread_name = pa_sprintf_malloc("post_proc_%u", sdata->session_id);
    if ((sdata->io_thread = pa_thread_new(thread_name, pa_qahw_post_proc_io_thread_func, sdata)) == NULL) {
        pa_log_error("%s: Could not spawn I/O thread", __func__);
        goto fail;
    }
    pa_xfree(thread_name);

    if (qahw_open_post_proc_stream(mdata->module_handle, sdata->topology_id, sdata->app_type,
        &sdata->input_buf_config, &sdata->output_buf_config, &sdata->stream_handle)) {
        pa_log_error("Could not open post proc session in HAL");
        goto fail;
    }

    sdata->popp_id = qahw_get_post_proc_session_id(sdata->stream_handle);

    return sdata;

fail:
    pa_qahw_close_session(sdata);

    return NULL;
}

static int pa_qahw_create_and_open_io_files(pa_qahw_post_proc_session_data_t *sdata) {

    sdata->input_file_name = pa_sprintf_malloc("/tmp/post_proc_input_%u.fifo", sdata->session_id);
    sdata->output_file_name = pa_sprintf_malloc("/tmp/post_proc_output_%u.fifo", sdata->session_id);

    pa_log_debug("%s: input file: %s, output file: %s", __func__, sdata->input_file_name, sdata->output_file_name);

    if (mkfifo(sdata->input_file_name, 0666) < 0) {
        if (errno != EEXIST) {
            pa_log_error("%s: mkfifo('%s'): %s", __func__, sdata->input_file_name, pa_cstrerror(errno));
            goto fail;
        }
    } else {
        /* Our umask is 077, so the pipe won't be created with the requested
         * permissions. Let's fix the permissions with chmod(). */
        if (chmod(sdata->input_file_name, 0666) < 0)
            pa_log_warn("%s: chomd('%s'): %s", __func__, sdata->input_file_name, pa_cstrerror(errno));
    }

    if ((sdata->input_fd = pa_open_cloexec(sdata->input_file_name, O_RDWR, 0)) < 0) {
        pa_log_error("%s: open('%s'): %s", __func__, sdata->input_file_name, pa_cstrerror(errno));
        goto fail;
    }

    if (mkfifo(sdata->output_file_name, 0666) < 0) {
        if (errno != EEXIST) {
            pa_log_error("%s: mkfifo('%s'): %s", __func__, sdata->output_file_name, pa_cstrerror(errno));
            goto fail;
        }
    } else {
        /* Our umask is 077, so the pipe won't be created with the requested
         * permissions. Let's fix the permissions with chmod(). */
        if (chmod(sdata->output_file_name, 0666) < 0)
            pa_log_warn("%s: chomd('%s'): %s", __func__, sdata->output_file_name, pa_cstrerror(errno));
    }

    if ((sdata->output_fd = pa_open_cloexec(sdata->output_file_name, O_RDWR, 0)) < 0) {
        pa_log_error("%s: open('%s'): %s", __func__, sdata->output_file_name, pa_cstrerror(errno));
        goto fail;
    }

    return 0;

fail:
    return -1;
}

static void pa_qahw_post_proc_io_thread_func(void *userdata) {
    pa_qahw_post_proc_session_data_t *sdata;

    pa_memchunk in_chunk;
    pa_memchunk out_chunk;

    qahw_post_proc_buffer_t in_buf;
    qahw_post_proc_buffer_t out_buf;

    int read_type = 0;
    int write_type = 0;

    pa_sample_format_t pa_format;

    pa_assert(userdata);

    sdata = (pa_qahw_post_proc_session_data_t *) userdata;

    if ((sdata->mdata->core->realtime_scheduling)) {
        pa_log_info("%s:: Making IO thread as realtime with prio %d", __func__,
                        sdata->mdata->core->realtime_priority);
        pa_make_realtime(sdata->mdata->core->realtime_priority);
    }

    pa_log_debug("%s: Starting IO thread", __func__);

    pa_thread_mq_install(&sdata->thread_mq);

    pa_memchunk_reset(&in_chunk);
    pa_memchunk_reset(&out_chunk);

    memset(&in_buf, 0, sizeof(qahw_post_proc_buffer_t));
    memset(&out_buf, 0, sizeof(qahw_post_proc_buffer_t));

    for(;;) {
        int ret;
        struct pollfd *pollfd;
        size_t out_bytes = 0;

        pollfd = pa_rtpoll_item_get_pollfd(sdata->rtpoll_item, NULL);

        if (pollfd->revents) {
            ssize_t l;
            void *data;

            if (!in_chunk.memblock) {
                in_chunk.memblock = pa_memblock_new(sdata->mdata->core->mempool, -1);
                in_chunk.index = in_chunk.length = 0;
            }

#ifdef POST_PROC_DEBUG
            pa_log_debug("%s: trying to read %u bytes from fifo", __func__, pa_memblock_get_length(in_chunk.memblock));
#endif

            data = pa_memblock_acquire(in_chunk.memblock);
            l = pa_read(sdata->input_fd, (uint8_t*) data, pa_memblock_get_length(in_chunk.memblock), &read_type);

            pa_assert(l != 0); /* EOF cannot happen, since we opened the fifo for both reading and writing */

            if (l < 0) {
                if (errno == EINTR)
                    continue;
                else if (errno != EAGAIN) {
                    pa_log_error("%s: Failed to read data from FIFO: %s", __func__, pa_cstrerror(errno));
                    goto fail;
                }
            } else {
                in_chunk.length = (size_t) l;
                in_buf.raw = (char*)data + in_chunk.index;

                pa_format = pa_qahw_util_get_pa_sample_from_qahw_format(sdata->input_buf_config.format);
                in_buf.frame_count = in_chunk.length / (sdata->input_buf_config.channels *
                                                    pa_sample_size_of_format(pa_format));

#ifdef POST_PROC_DEBUG
                pa_log_debug("%s: read %u bytes, frame count %u", __func__, in_chunk.length, in_buf.frame_count);
#endif

                out_buf.frame_count = in_buf.frame_count * sdata->output_buf_config.sampling_rate /
                                        sdata->input_buf_config.sampling_rate;
                pa_format = pa_qahw_util_get_pa_sample_from_qahw_format(sdata->output_buf_config.format);
                out_bytes = out_buf.frame_count * (sdata->output_buf_config.channels *
                                                    pa_sample_size_of_format(pa_format));

                out_chunk.memblock = pa_memblock_new(sdata->mdata->core->mempool, out_bytes);
                data = pa_memblock_acquire(out_chunk.memblock);
                out_chunk.length = pa_memblock_get_length(out_chunk.memblock);

                out_buf.raw = data;

                qahw_process(sdata->stream_handle, &in_buf, &out_buf);

                if (pa_qahw_write_output(sdata, &out_chunk, &write_type) < 0)
                    goto fail;

                pa_memblock_release(out_chunk.memblock);
                pa_memblock_unref(out_chunk.memblock);
                pa_memchunk_reset(&out_chunk);

                pollfd->revents = 0;
            }

            pa_memblock_release(in_chunk.memblock);
            pa_memblock_unref(in_chunk.memblock);
            pa_memchunk_reset(&in_chunk);
        }

        /* Hmm, nothing to do. Let's sleep */
        pollfd->events = (short) POLLIN;

        if ((ret = pa_rtpoll_run(sdata->rtpoll, true)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        pollfd = pa_rtpoll_item_get_pollfd(sdata->rtpoll_item, NULL);

        if (pollfd->revents & ~POLLIN) {
            pa_log_error("%s: FIFO shutdown", __func__);
            goto fail;
        }
    }

fail:
    pa_log_error("%s: Something went wrong in IO thread", __func__);
    if (in_chunk.memblock) pa_memblock_unref(in_chunk.memblock);
    if (out_chunk.memblock) pa_memblock_unref(out_chunk.memblock);
    pa_asyncmsgq_wait_for(sdata->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("%s: IO thread is shutting down", __func__);
}

static int pa_qahw_write_output (pa_qahw_post_proc_session_data_t *sdata, pa_memchunk *out_chunk, int *write_type) {

    for (;;) {
        ssize_t l;
        void *data;

#ifdef POST_PROC_DEBUG
        pa_log_debug("%s: trying to write out_chunk index %u length %u", __func__, out_chunk->index, out_chunk->length);
#endif

        data = pa_memblock_acquire(out_chunk->memblock);
        l = pa_write(sdata->output_fd, (uint8_t*) data + out_chunk->index, out_chunk->length, write_type);
        pa_memblock_release(out_chunk->memblock);

#ifdef POST_PROC_DEBUG
        pa_log_debug("%s: wrote %u bytes", __func__, l);
#endif

        pa_assert(l != 0);

        if (l < 0) {
            if (errno == EINTR)
                continue;
            else if (errno == EAGAIN)
                return 0;
            else {
                pa_log_error("%s: Failed to write data to FIFO: %s", __func__, pa_cstrerror(errno));
                return -1;
            }
        } else {
            out_chunk->index += (size_t) l;
            out_chunk->length -= (size_t) l;

            if(out_chunk->length <= 0)
                return 0;
        }
    }

    return 0;
}

static void pa_qahw_post_proc_get_param_async (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_session_data_t *sdata;
    pa_qahw_dbus_conn_data_t *dbus_conn_data;

    pa_assert(userdata);

    sdata = (pa_qahw_post_proc_session_data_t *) userdata;
    dbus_conn_data = pa_qahw_create_dbus_conn_data(conn, msg, userdata);

    pa_asyncmsgq_post(sdata->mdata->dbus_hdlr_thread_mq.inq, PA_MSGOBJECT(sdata->mdata),
                        PA_QAHW_POST_PROC_MSG_GET_PARAM, dbus_conn_data, 0, NULL, NULL);
}

static void pa_qahw_post_proc_get_param (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_effect_command(conn, msg, userdata, QAHW_EFFECT_CMD_GET_PARAM);
}

static void pa_qahw_post_proc_set_param_async (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_session_data_t *sdata;
    pa_qahw_dbus_conn_data_t *dbus_conn_data;

    pa_assert(userdata);

    sdata = (pa_qahw_post_proc_session_data_t *) userdata;
    dbus_conn_data = pa_qahw_create_dbus_conn_data(conn, msg, userdata);

    pa_asyncmsgq_post(sdata->mdata->dbus_hdlr_thread_mq.inq, PA_MSGOBJECT(sdata->mdata),
                        PA_QAHW_POST_PROC_MSG_SET_PARAM, dbus_conn_data, 0, NULL, NULL);
}

static void pa_qahw_post_proc_set_param (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_effect_command(conn, msg, userdata, QAHW_EFFECT_CMD_SET_PARAM);
}

static void pa_qahw_post_proc_set_persist_async (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_session_data_t *sdata;
    pa_qahw_dbus_conn_data_t *dbus_conn_data;

    pa_assert(userdata);

    sdata = (pa_qahw_post_proc_session_data_t *) userdata;
    dbus_conn_data = pa_qahw_create_dbus_conn_data(conn, msg, userdata);

    pa_asyncmsgq_post(sdata->mdata->dbus_hdlr_thread_mq.inq, PA_MSGOBJECT(sdata->mdata),
                        PA_QAHW_POST_PROC_MSG_SET_PERSIST, dbus_conn_data, 0, NULL, NULL);
}

static void pa_qahw_post_proc_set_persist (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_effect_command(conn, msg, userdata, QAHW_EFFECT_CMD_SET_PERSIST);
}

static void pa_qahw_post_proc_effect_command (DBusConnection *conn, DBusMessage *msg, void *userdata, uint32_t command_code) {
    pa_qahw_post_proc_session_data_t *sdata;
    pa_qahw_post_proc_effect_session_data_t *effect_sdata = NULL;

    pa_qahw_post_proc_topology_info_t *topology_info;
    pa_qahw_post_proc_effect_info_t *effect_info;

    DBusError error;
    DBusMessage *reply = NULL;
    DBusMessageIter iter, uuid_struct_iter, uuid_array_iter, data_array_iter;

    int uuid_node_length, data_length;
    char *uuid_node = NULL;

    qahw_effect_uuid_t *uuid = NULL;
    char *uuid_str = NULL;
    char *topology_id_str = NULL;

    uint32_t command_size, reply_size;
    uint32_t enable_reply_size;
    uint32_t offload_command_size, offload_reply_size;

    int32_t *enable_reply_data = NULL, *offload_reply_data = NULL;
    void *command_data = NULL, *reply_data = NULL;
    qahw_effect_offload_v2_param_t *offload_command_data = NULL;

    bool persist;

    int rc;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    sdata = (pa_qahw_post_proc_session_data_t *) userdata;

    pa_log_debug("%s", __func__);

    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &iter)) {
        pa_log_error("%s: No arguments", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "No arguments.");
        dbus_error_free(&error);
        return;
    }

    if ((command_code == QAHW_EFFECT_CMD_SET_PERSIST && !pa_streq(dbus_message_get_signature(msg), "(uqqqay)b")) ||
        (command_code != QAHW_EFFECT_CMD_SET_PERSIST && !pa_streq(dbus_message_get_signature(msg), "(uqqqay)uuay"))) {
        pa_log_error("%s: Invalid signature", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature");
        dbus_error_free(&error);
        return;
    }

    uuid = pa_xnew0(qahw_effect_uuid_t, 1);

    dbus_message_iter_recurse(&iter, &uuid_struct_iter);
    dbus_message_iter_get_basic(&uuid_struct_iter, &uuid->timeLow);
    dbus_message_iter_next(&uuid_struct_iter);
    dbus_message_iter_get_basic(&uuid_struct_iter, &uuid->timeMid);
    dbus_message_iter_next(&uuid_struct_iter);
    dbus_message_iter_get_basic(&uuid_struct_iter, &uuid->timeHiAndVersion);
    dbus_message_iter_next(&uuid_struct_iter);
    dbus_message_iter_get_basic(&uuid_struct_iter, &uuid->clockSeq);
    dbus_message_iter_next(&uuid_struct_iter);
    dbus_message_iter_recurse(&uuid_struct_iter, &uuid_array_iter);
    dbus_message_iter_get_fixed_array(&uuid_array_iter, &uuid_node, &uuid_node_length);
    memcpy(&uuid->node, uuid_node, uuid_node_length);

    uuid_str = pa_qahw_post_proc_get_string_from_uuid(uuid);

    if (sdata->effect_sessions == NULL) {
        sdata->effect_sessions = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                         pa_idxset_string_compare_func, NULL,
                                                         (pa_free_cb_t) pa_qahw_post_proc_free_effect_session);
    } else {
        effect_sdata = pa_hashmap_get(sdata->effect_sessions, uuid_str);
    }

    if(effect_sdata == NULL) {
        topology_id_str = pa_sprintf_malloc("%u", sdata->topology_id);
        topology_info = pa_hashmap_get(sdata->mdata->topology_infos, topology_id_str);
        if (topology_info == NULL) {
            pa_log_error("%s: Topology with id %s not found", __func__, topology_id_str);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Topology info not found");
            goto fail;
        }

        effect_info = pa_hashmap_get(topology_info->effect_infos, uuid_str);
        if (effect_info == NULL) {
            pa_log_error("%s: Effect with uuid %s not found", __func__, uuid_str);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Effect info not found");
            goto fail;
        }

        effect_sdata = pa_xnew0(pa_qahw_post_proc_effect_session_data_t, 1);
        effect_sdata->lib_handle = qahw_effect_load_library(effect_info->lib_name);

        rc = qahw_effect_create(effect_sdata->lib_handle, uuid, 0, &effect_sdata->effect_handle);
        if (rc != 0) {
            pa_log_error("%s: Failed to create effect session", __func__);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Failed to create effect session");
            goto fail;
        }

        offload_reply_size = sizeof(int32_t);
        offload_reply_data = (int32_t *) malloc(sizeof(int32_t));
        offload_command_size = sizeof(qahw_effect_offload_v2_param_t);

        offload_command_data = (qahw_effect_offload_v2_param_t *)malloc(offload_command_size);
        offload_command_data->isOffload = true;
        offload_command_data->isNonTunnel = true;
        offload_command_data->sessionId = sdata->popp_id;
        offload_command_data->topoId = sdata->topology_id;
        offload_command_data->appType = sdata->app_type;

        rc = qahw_effect_command(effect_sdata->effect_handle, QAHW_EFFECT_CMD_OFFLOAD_V2,
                                    offload_command_size, (void *)offload_command_data,
                                    &offload_reply_size, (void *)offload_reply_data);
        if (rc != 0) {
            pa_log_error("%s: Failed to set offload param", __func__);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Failed to set offload param");
            goto fail;
        }

        enable_reply_size = sizeof(int32_t);
        enable_reply_data = (int32_t *) malloc(sizeof(int32_t));

        rc = qahw_effect_command(effect_sdata->effect_handle, QAHW_EFFECT_CMD_ENABLE, 0, NULL,
                                    &enable_reply_size, (void *)enable_reply_data);
        if (rc != 0) {
            pa_log_error("%s: Failed to enable GEF", __func__);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Failed to enable GEF");
            goto fail;
        }

        pa_log_debug("%s: effect session for uuid %s is created and enabled", __func__, effect_info->uuid_str);

        pa_hashmap_put(sdata->effect_sessions, effect_info->uuid_str, effect_sdata);
    }

    switch (command_code) {
        case QAHW_EFFECT_CMD_GET_PARAM:
        case QAHW_EFFECT_CMD_SET_PARAM:
            dbus_message_iter_next(&iter);
            dbus_message_iter_get_basic(&iter, &command_size);
            dbus_message_iter_next(&iter);
            dbus_message_iter_get_basic(&iter, &reply_size);

            dbus_message_iter_next(&iter);
            dbus_message_iter_recurse(&iter, &data_array_iter);
            dbus_message_iter_get_fixed_array(&data_array_iter, &command_data, &data_length);
            reply_data = (void *)malloc(reply_size);
            break;
        case QAHW_EFFECT_CMD_SET_PERSIST:
            dbus_message_iter_next(&iter);
            dbus_message_iter_get_basic(&iter, &persist);
            command_data = (void *) (&persist);
            reply_size = 0;
            break;
        default:
            pa_log_error("%s: Invalid effect command", __func__);
            break;
    }

    rc = qahw_effect_command(effect_sdata->effect_handle, command_code, command_size,
                                command_data, &reply_size, reply_data);
    if (rc != 0) {
        pa_log_error("%s: Command Failed", __func__);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Command Failed");
        goto fail;
    }

    if (reply_size > 0) {
        pa_assert_se((reply = dbus_message_new_method_return(msg)));
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "y", &data_array_iter);
        dbus_message_iter_append_fixed_array(&data_array_iter, DBUS_TYPE_BYTE, &reply_data, reply_size);
        dbus_message_iter_close_container(&iter, &data_array_iter);
        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        dbus_message_unref(reply);
    } else
        pa_dbus_send_empty_reply(conn, msg);

    goto done;

fail:
    if (effect_sdata) pa_qahw_post_proc_free_effect_session(effect_sdata);

done:
    if (uuid_str) pa_xfree(uuid_str);
    if (topology_id_str) pa_xfree(topology_id_str);
    if (uuid) pa_xfree(uuid);
    if (offload_command_data) pa_xfree(offload_command_data);
    if (offload_reply_data) pa_xfree(offload_reply_data);
    if (enable_reply_data) pa_xfree(enable_reply_data);

    dbus_error_free(&error);
}

static void pa_qahw_post_proc_release_session_async (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_session_data_t *sdata;
    pa_qahw_dbus_conn_data_t *dbus_conn_data;

    pa_assert(userdata);

    sdata = (pa_qahw_post_proc_session_data_t *) userdata;
    dbus_conn_data = pa_qahw_create_dbus_conn_data(conn, msg, userdata);

    pa_asyncmsgq_post(sdata->mdata->dbus_hdlr_thread_mq.inq, PA_MSGOBJECT(sdata->mdata),
                        PA_QAHW_POST_PROC_MSG_RELEASE_SESSION, dbus_conn_data, 0, NULL, NULL);
}

static void pa_qahw_post_proc_release_session (DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_post_proc_session_data_t *sdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    sdata = (pa_qahw_post_proc_session_data_t *) userdata;

    pa_log_debug("%s Enter", __func__);

    if (sdata->mdata->dbus_protocol && sdata->session_object_path) {
        pa_assert_se(pa_dbus_protocol_remove_interface(sdata->mdata->dbus_protocol,
                        sdata->session_object_path, session_interface_info.name) >= 0);
    }
    pa_qahw_close_session(sdata);

    pa_dbus_send_empty_reply(conn, msg);
    pa_log_debug("%s Exit", __func__);
}

static void pa_qahw_close_session (pa_qahw_post_proc_session_data_t *sdata) {
    pa_assert(sdata);

    pa_log_debug("%s Enter", __func__);

    if(sdata->session_object_path) pa_xfree(sdata->session_object_path);

    if (sdata->stream_handle) {
        pa_log_debug("%s: Closing HAL session", __func__);
        qahw_close_post_proc_stream(sdata->stream_handle);
    }

    if (sdata->io_thread) {
        pa_asyncmsgq_send(sdata->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(sdata->io_thread);
    }
    pa_thread_mq_done(&sdata->thread_mq);
    if (sdata->rtpoll_item) pa_rtpoll_item_free(sdata->rtpoll_item);
    if (sdata->rtpoll) pa_rtpoll_free(sdata->rtpoll);

    //closing file descriptors
    if (sdata->input_fd) pa_assert_se(pa_close(sdata->input_fd) == 0);
    if (sdata->output_fd) pa_assert_se(pa_close(sdata->output_fd) == 0);

    if (sdata->input_file_name) pa_xfree(sdata->input_file_name);
    if (sdata->output_file_name) pa_xfree(sdata->output_file_name);

    if (sdata->effect_sessions) pa_hashmap_free(sdata->effect_sessions);

    pa_xfree(sdata);
    pa_log_debug("%s Exit", __func__);
}

void pa_qahw_post_proc_module_deinit (pa_qahw_post_proc_handle_t post_proc_handle) {

    pa_qahw_post_proc_module_data_t *mdata = (pa_qahw_post_proc_module_data_t *) post_proc_handle;

    if (mdata->topology_infos) pa_hashmap_free(mdata->topology_infos);
    if (mdata->sessions) pa_hashmap_free(mdata->sessions);

    if (mdata->dbus_protocol) {
        pa_assert_se(pa_dbus_protocol_remove_interface(mdata->dbus_protocol,
                        QAHW_POST_PROC_MODULE_OBJECT_PATH, module_interface_info.name) >= 0);
    }

    if (mdata->dbus_hdlr_thread) {
        pa_asyncmsgq_send(mdata->dbus_hdlr_thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(mdata->dbus_hdlr_thread);
    }
    if (mdata->dbus_hdlr_rtpoll) pa_rtpoll_free(mdata->dbus_hdlr_rtpoll);
    pa_thread_mq_done(&mdata->dbus_hdlr_thread_mq);

    qahw_post_proc_unload_module(mdata->module_handle);
    pa_xfree(mdata);
}

static void pa_qahw_post_proc_free_topology_info(pa_qahw_post_proc_topology_info_t *topology_info) {
    pa_assert(topology_info);

    pa_log_info("%s: freeing topology %s", __func__, topology_info->name);

    if (topology_info->name != NULL) pa_xfree(topology_info->name);
    if (topology_info->topology_id_str) pa_xfree(topology_info->topology_id_str);
    if (topology_info->effect_infos) pa_hashmap_free(topology_info->effect_infos);

    pa_xfree(topology_info);
}

static void pa_qahw_post_proc_free_effect_info(pa_qahw_post_proc_effect_info_t *effect_info) {
    pa_assert(effect_info);

    pa_log_info("%s: freeing effect %s", __func__, effect_info->name);

    if (effect_info->name != NULL) pa_xfree(effect_info->name);
    if (effect_info->uuid) pa_xfree(effect_info->uuid);
    if (effect_info->uuid_str) pa_xfree(effect_info->uuid_str);
    if (effect_info->lib_name) pa_xfree(effect_info->lib_name);

    pa_xfree(effect_info);
}

static void pa_qahw_post_proc_free_effect_session(pa_qahw_post_proc_effect_session_data_t *effect_sdata) {
    int rc;

    pa_assert(effect_sdata);

    pa_log_info("%s", __func__);

    if (effect_sdata->effect_handle) {
        rc = qahw_effect_release(effect_sdata->lib_handle, effect_sdata->effect_handle);
        if (rc != 0) {
            pa_log_error("%s: qahw_effect_release returns :%d", __func__, rc);
            return;
        }
    }

    rc = qahw_effect_unload_library(effect_sdata->lib_handle);
    if (rc != 0) {
        pa_log_error("%s: qahw_effect_unload_library returns :%d", __func__, rc);
        return;
    }

    pa_xfree(effect_sdata);
}

#endif //QAHW_AUDIO_ADSP_PP_ENABLED
