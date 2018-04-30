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
#include <errno.h>
#include <math.h>

#include "qahw-loopback.h"
#include "qahw-utils.h"

#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>

#define PA_QAHW_LOOPBACK_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qahw"
#define PA_QAHW_LOOPBACK_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Loopback"
#define PA_QAHW_LOOPBACK_DBUS_SESSION_IFACE "org.PulseAudio.Ext.Loopback.Session"

#define PA_QAHW_LOOPBACK_PORT_CONFIG_AUTO 0x10
#define E_OK 0
#define MILLIBELS_CONSTANT 2000

static struct pa_qahw_loopback_module_data {
    pa_card *card;
    qahw_module_handle_t *module_handle;
    pa_dbus_protocol *dbus_protocol;
    char *dbus_path;
} *pa_qahw_loopback_mdata;

static void pa_qahw_loopback_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_loopback_stop(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum pa_qahw_module_handler_index {
    MODULE_HANDLER_CREATE_LOOPBACK,
    MODULE_HANDLER_MAX
};

enum pa_qahw_session_handler_index {
    SESSION_HANDLER_STOP_LOOPBACK,
    SESSION_HANDLER_MAX
};

struct pa_qahw_loopback_session_data {
    struct pa_qahw_loopback_module_data *common;
    audio_patch_handle_t ses_handle;
    char *obj_path;
};

pa_dbus_arg_info pa_qahw_loopback_create_args[] = {
    {"create_loopback", "(iiuuuiids)a(iiuuuiids)", "in"},
    {"object_path", "o", "out"},
};

pa_dbus_arg_info pa_qahw_loopback_stop_args[] = {
};

static pa_dbus_method_handler pa_qahw_loopback_module_handlers[MODULE_HANDLER_MAX] = {
    [MODULE_HANDLER_CREATE_LOOPBACK] = {
        .method_name = "Create",
        .arguments = pa_qahw_loopback_create_args,
        .n_arguments = sizeof(pa_qahw_loopback_create_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_loopback_create},
};

static pa_dbus_method_handler pa_qahw_loopback_session_handlers[SESSION_HANDLER_MAX] = {
    [SESSION_HANDLER_STOP_LOOPBACK] = {
        .method_name = "Stop",
        .arguments = pa_qahw_loopback_stop_args,
        .n_arguments = sizeof(pa_qahw_loopback_stop_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_loopback_stop},
};

pa_dbus_interface_info pa_qahw_loopback_module_interface_info = {
    .name = PA_QAHW_LOOPBACK_DBUS_MODULE_IFACE,
    .method_handlers = pa_qahw_loopback_module_handlers,
    .n_method_handlers = MODULE_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

pa_dbus_interface_info pa_qahw_loopback_session_interface_info = {
    .name = PA_QAHW_LOOPBACK_DBUS_SESSION_IFACE,
    .method_handlers = pa_qahw_loopback_session_handlers,
    .n_method_handlers = SESSION_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

/******* Helper functions ********/
void pa_qahw_loopback_init(qahw_module_handle_t *module_handle, pa_core *core, pa_card *card) {
    pa_log_info("%s enter\n", __func__);

    pa_qahw_loopback_mdata = pa_xnew0(struct pa_qahw_loopback_module_data, 1);

    pa_qahw_loopback_mdata->dbus_path = pa_sprintf_malloc("%s/%s", PA_QAHW_LOOPBACK_DBUS_OBJECT_PATH_PREFIX,
                                                                                                 "loopback");

    pa_qahw_loopback_mdata->dbus_protocol = pa_dbus_protocol_get(core);

    pa_qahw_loopback_mdata->module_handle = module_handle;
    pa_qahw_loopback_mdata->card = card;

    pa_assert_se(pa_dbus_protocol_add_interface(pa_qahw_loopback_mdata->dbus_protocol, pa_qahw_loopback_mdata->dbus_path,
                                                    &pa_qahw_loopback_module_interface_info, pa_qahw_loopback_mdata) >= 0);

    pa_log_info("%s exit\n", __func__);
}

void pa_qahw_loopback_deinit(void) {
    pa_log_info("%s enter\n", __func__);

    if (pa_qahw_loopback_mdata) {
        if (pa_qahw_loopback_mdata->dbus_path && pa_qahw_loopback_mdata->dbus_protocol)
            pa_assert_se(pa_dbus_protocol_remove_interface(pa_qahw_loopback_mdata->dbus_protocol,pa_qahw_loopback_mdata->dbus_path,
                                                                                 pa_qahw_loopback_module_interface_info.name) >= 0);

        if (pa_qahw_loopback_mdata->dbus_path)
            pa_dbus_protocol_unref(pa_qahw_loopback_mdata->dbus_protocol);

        if (pa_qahw_loopback_mdata->dbus_protocol)
            pa_xfree(pa_qahw_loopback_mdata->dbus_path);

        pa_xfree(pa_qahw_loopback_mdata);
    }

    pa_log_info("%s exit\n", __func__);
}

static void pa_qahw_loopback_unmarshal_port_config(DBusMessageIter *arg, struct audio_port_config *cfg,
                                                                      double *port_gain, pa_card *card) {
    DBusMessageIter struct_i;
    pa_device_port *p;
    audio_devices_t *audio_device;
    pa_encoding_t format;
    pa_sample_format_t bitwidth;
    unsigned int num_channels;
    char *port_name = NULL;

    dbus_message_iter_recurse(arg, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &(cfg->id));
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &(cfg->role));
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &(cfg->config_mask));
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &(cfg->sample_rate));
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &num_channels);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &format);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &bitwidth);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, port_gain);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &port_name);

    p = pa_hashmap_get(card->ports, port_name);
    audio_device = PA_DEVICE_PORT_DATA(p);
    pa_assert(audio_device);
    cfg->ext.device.type = *audio_device;

    cfg->channel_mask = pa_qahw_util_get_channel_mask_from_num_channels(num_channels);

    if ((cfg->ext.device.type == AUDIO_DEVICE_OUT_SPEAKER) ||
        (cfg->ext.device.type == AUDIO_DEVICE_OUT_WIRED_HEADSET) ||
        (cfg->ext.device.type == AUDIO_DEVICE_OUT_WIRED_HEADPHONE) ||
        (cfg->ext.device.type == AUDIO_DEVICE_OUT_LINE)) {
        cfg->format = get_qahw_audio_format(bitwidth);
    } else {
        if (format == PA_ENCODING_PCM)
            cfg->format = get_qahw_audio_format(bitwidth);
        else
            cfg->format = pa_qahw_util_get_qahw_format_from_pa_encoding(format);
    }

    memset(&(cfg->gain), 0, sizeof(struct audio_gain_config));

    cfg->type = AUDIO_PORT_TYPE_DEVICE;
}

static dbus_uint32_t pa_qahw_loopback_get_array_size(DBusMessageIter array) {
    dbus_uint32_t cnt = 0;
    int arg_type;

    while ((arg_type = dbus_message_iter_get_arg_type(&array)) != DBUS_TYPE_INVALID) {
        cnt++;
        dbus_message_iter_next(&array);
    }

    return cnt;
}

/******* Module specific function ********/
static void pa_qahw_loopback_create(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int status = 0;
    dbus_uint32_t i;
    pa_card *card;
    struct pa_qahw_loopback_module_data *u;
    qahw_module_handle_t *module_handle;
    audio_patch_handle_t handle = AUDIO_PATCH_HANDLE_NONE;
    struct pa_qahw_loopback_session_data *ses_data = NULL;

    dbus_uint32_t num_srcs = 1;
    double src_port_gain;
    struct audio_port_config src_cfg;

    dbus_uint32_t num_sinks;
    double *sink_port_gain = NULL;
    struct audio_port_config *sink_cfg = NULL;

    struct audio_port_config sink_gain_config;
    int loopback_gain_in_millibels;

    DBusMessage *reply = NULL;
    DBusError error;
    DBusMessageIter arg_i, array_i;

    pa_log_debug("%s enter\n", __func__);

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    u = (struct pa_qahw_loopback_module_data *)userdata;
    module_handle = u->module_handle;
    card = u->card;

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("Create has no arguments\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Create has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(iiuuuiids)a(iiuuuiids)")) {
        pa_log_error("Invalid signature for Create\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for Create");
        dbus_error_free(&error);
        return;
    }

    pa_log_info("Unmarshalling create_qahw_loopback message\n");
    pa_qahw_loopback_unmarshal_port_config(&arg_i, &src_cfg, &src_port_gain, card);

    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    num_sinks = pa_qahw_loopback_get_array_size(array_i);

    sink_cfg = pa_xnew0(struct audio_port_config, num_sinks);
    sink_port_gain = pa_xnew0(double, num_sinks);

    for(i = 0; i < num_sinks; i++) {
        pa_qahw_loopback_unmarshal_port_config(&array_i, &sink_cfg[i], &sink_port_gain[i], card);
        dbus_message_iter_next(&array_i);
    }

    pa_log_debug("Source port config: id %d, role %d, config_mask %u, sample_rate %u, "
                 "channel_mask %u, format %d, gain %lf, device %u\n",
                 src_cfg.id, src_cfg.role, src_cfg.config_mask,
                 src_cfg.sample_rate, src_cfg.channel_mask, src_cfg.format,
                 src_port_gain, src_cfg.ext.device.type);

    for (i = 0; i < num_sinks; i++) {
        pa_log_debug("Sink port config: id %d, role %d, config_mask %u, sample_rate %u, "
                 "channel_mask %u, format %d, gain %lf, device %u\n", sink_cfg[i].id,
                 sink_cfg[i].role, sink_cfg[i].config_mask, sink_cfg[i].sample_rate,
                 sink_cfg[i].channel_mask, sink_cfg[i].format, sink_port_gain[i], sink_cfg[i].ext.device.type);
    }

    if (src_cfg.config_mask == PA_QAHW_LOOPBACK_PORT_CONFIG_AUTO)
        src_cfg.config_mask = AUDIO_PORT_CONFIG_ALL ^ AUDIO_PORT_CONFIG_GAIN;

    pa_log_info("Creating audio loopback patch\n");
    status = qahw_create_audio_patch(module_handle, num_srcs, &src_cfg,
                                                num_sinks, sink_cfg, &handle);
    pa_log_debug("Create audio loopback patch returned status: %d, handle %d\n",
                                                                status, handle);

    if (!status) {
        for (i = 0; i < num_sinks; i++) {
            loopback_gain_in_millibels = MILLIBELS_CONSTANT * log10(sink_port_gain[i]);
            sink_gain_config.gain.index = 0;
            sink_gain_config.gain.mode = AUDIO_GAIN_MODE_JOINT;
            sink_gain_config.gain.channel_mask = 1;
            sink_gain_config.gain.values[0] = loopback_gain_in_millibels;
            sink_gain_config.id = sink_cfg[i].id;
            sink_gain_config.role = sink_cfg[i].role;
            sink_gain_config.type = sink_cfg[i].type;
            sink_gain_config.config_mask = AUDIO_PORT_CONFIG_GAIN;
            (void)qahw_set_audio_port_config(module_handle, &sink_gain_config);
        }
    } else {
        pa_log_error("QAHW create audio patch failed\n");
        pa_xfree(sink_cfg);
        pa_xfree(sink_port_gain);

        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Create failed");
        dbus_error_free(&error);
        return;
    }

    /* Create session data */
    ses_data = pa_xnew0(struct pa_qahw_loopback_session_data, 1);
    ses_data->common = u;
    ses_data->ses_handle = handle;
    ses_data->obj_path = pa_sprintf_malloc("%s/ses_%d", u->dbus_path, handle);

    pa_log_info("session obj path %s \n", ses_data->obj_path);

    pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol, ses_data->obj_path,
                                            &pa_qahw_loopback_session_interface_info, ses_data) >= 0);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &ses_data->obj_path);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    pa_xfree(sink_cfg);
    pa_xfree(sink_port_gain);

    pa_log_debug("%s exit\n", __func__);
}

/******* Session specific function ********/
static void pa_qahw_loopback_stop(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pa_qahw_loopback_session_data *ses_data = (struct pa_qahw_loopback_session_data *)userdata;
    int status = -1;
    dbus_int32_t handle = ses_data->ses_handle;

    DBusError error;

    pa_log_debug("%s enter\n", __func__);

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    status = qahw_release_audio_patch(ses_data->common->module_handle, handle);
    if (status != E_OK) {
        pa_log_error("QAHW release audio patch failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "QAHW release audio patch failed");
        dbus_error_free(&error);

        pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol, ses_data->obj_path,
                                                        pa_qahw_loopback_session_interface_info.name) >= 0);

        pa_xfree(ses_data->obj_path);
        pa_xfree(ses_data);

        return;
    }

    pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol, ses_data->obj_path,
                                                    pa_qahw_loopback_session_interface_info.name) >= 0);

    pa_xfree(ses_data->obj_path);
    pa_xfree(ses_data);

    pa_dbus_send_empty_reply(conn, msg);

    pa_log_debug("%s exit\n", __func__);
}
