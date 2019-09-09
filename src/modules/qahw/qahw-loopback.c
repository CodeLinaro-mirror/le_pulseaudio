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

#include <pulsecore/core-util.h>
#include <errno.h>
#include <math.h>

#include "qahw-loopback.h"
#include "qahw-utils.h"
#include "qahw-card.h"
#include "qahw-jack.h"
#include "qahw-jack-format.h"

#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>

#define PA_QAHW_LOOPBACK_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qahw"
#define PA_QAHW_LOOPBACK_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Loopback"
#define PA_QAHW_LOOPBACK_DBUS_SESSION_IFACE "org.PulseAudio.Ext.Loopback.Session"

#define PA_QAHW_LOOPBACK_PORT_CONFIG_AUTO 0x10
#define PA_QAHW_LOOPBACK_MAX_SESSIONS 1
#define E_OK 0
#define MILLIBELS_CONSTANT 2000
#define NUM_CHANNEL_POSITIONS 8

#define PA_DEFAULT_PORT_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_PORT_RATE 48000
#define PA_DEFAULT_PORT_CHANNELS 2

static struct pa_qahw_loopback_module_data {
    pa_card *card;
    pa_module *m;
    qahw_module_handle_t *module_handle;
    pa_dbus_protocol *dbus_protocol;
    char *dbus_path;
    pa_hashmap *loopbacks;
    pa_hashmap *loopback_mappings;              /* Stores name & patch handle corresponding to a loopback */
    pa_hashmap *loopback_sessions;
    pa_qahw_loopback_callback_t callback;
    void *prv_data;
    pa_qahw_effect_handle_t effect_handle;
} *pa_qahw_loopback_mdata;

struct pa_qahw_loopback_session_data {
    struct pa_qahw_loopback_module_data *common;
    audio_patch_handle_t ses_handle;
    char *obj_path;
    int latency;
    const char *src_port;
    dbus_uint32_t num_sinks;
    struct audio_port_config src_cfg;
    struct audio_port_config *sink_cfg;
    uint32_t src_config_mask;
    pa_qahw_jack_type_t src_jack_type;
    pa_qahw_jack_handle_t *jack_handle;
};

typedef struct {
    audio_patch_handle_t handle;
    char *name;
} pa_qahw_loopback_handle_to_name;

typedef struct {
    struct pa_qahw_loopback_module_data *mdata;
    audio_patch_handle_t handle;
} pa_qahw_loopback_callback_data;

static pa_qahw_jack_out_config curr_port_config;

static void pa_qahw_loopback_create(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_loopback_stop(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_loopback_get_port_config(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pa_qahw_loopback_set_port_config(DBusConnection *conn, DBusMessage *msg, void *userdata);
void pa_qahw_loopback_cb(pa_qahw_effect_event event_id, void *event_data, void *prv_data);
static int pa_set_metadata_av_window_mat(qahw_module_handle_t *hw_module, audio_patch_handle_t handle);

enum pa_qahw_module_handler_index {
    MODULE_HANDLER_CREATE_LOOPBACK,
    MODULE_HANDLER_SET_PORT_CONFIG,
    MODULE_HANDLER_GET_PORT_CONFIG,
    MODULE_HANDLER_MAX
};

enum pa_qahw_session_handler_index {
    SESSION_HANDLER_STOP_LOOPBACK,
    SESSION_HANDLER_MAX
};

enum signal_index {
    SIGNAL_DETECTION_EVENT,
    SIGNAL_MAX
};

pa_dbus_arg_info pa_qahw_loopback_create_args[] = {
    {"config", "(iiuuuiids)a(iiuuuiids)", "in"},
    {"object_path", "o", "out"},
};

pa_dbus_arg_info pa_qahw_loopback_stop_args[] = {
};

pa_dbus_arg_info pa_qahw_loopback_set_port_config_args[] = {
    {"config", "(iiuuuiids)", "in"},
};

pa_dbus_arg_info pa_qahw_loopback_get_port_config_args[] = {
    {"id", "ii", "in"},
    {"config", "(iiuuuiids)", "out"},
};

pa_dbus_arg_info detection_event_args[] = {
    {"handle", "i", NULL},
};

static pa_dbus_method_handler pa_qahw_loopback_module_handlers[MODULE_HANDLER_MAX] = {
    [MODULE_HANDLER_CREATE_LOOPBACK] = {
        .method_name = "Create",
        .arguments = pa_qahw_loopback_create_args,
        .n_arguments = sizeof(pa_qahw_loopback_create_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_loopback_create},
    [MODULE_HANDLER_GET_PORT_CONFIG] = {
        .method_name = "GetPortConfig",
        .arguments = pa_qahw_loopback_get_port_config_args,
        .n_arguments = sizeof(pa_qahw_loopback_get_port_config_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_loopback_get_port_config},
    [MODULE_HANDLER_SET_PORT_CONFIG] = {
        .method_name = "SetPortConfig",
        .arguments = pa_qahw_loopback_set_port_config_args,
        .n_arguments = sizeof(pa_qahw_loopback_set_port_config_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_loopback_set_port_config},
};

static pa_dbus_method_handler pa_qahw_loopback_session_handlers[SESSION_HANDLER_MAX] = {
    [SESSION_HANDLER_STOP_LOOPBACK] = {
        .method_name = "Stop",
        .arguments = pa_qahw_loopback_stop_args,
        .n_arguments = sizeof(pa_qahw_loopback_stop_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_loopback_stop},
};

static pa_dbus_signal_info det_event_signals[SIGNAL_MAX] = {
    [SIGNAL_DETECTION_EVENT] = {
        .name = "DetectionEvent",
        .arguments = detection_event_args,
        .n_arguments = sizeof(detection_event_args)/sizeof(pa_dbus_arg_info)},
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
    .signals = det_event_signals,
    .n_signals = SIGNAL_MAX
};

static int pa_qahw_loopback_set_format_update_callback(struct pa_qahw_loopback_session_data *);

char *pa_qahw_loopback_get_name_from_handle(audio_patch_handle_t handle)
{
    pa_qahw_loopback_handle_to_name *mapping = NULL;
    void *state;

    pa_assert(pa_qahw_loopback_mdata->loopback_mappings);

    PA_HASHMAP_FOREACH(mapping, pa_qahw_loopback_mdata->loopback_mappings, state) {
        if (mapping->handle == handle)
            return mapping->name;
    }
    return NULL;
}

void pa_qahw_loopback_cb(pa_qahw_effect_event event_id, void *event_data, void *prv_data)
{
    struct qahw_out_render_window_param render_window;
    pa_qahw_effect_callback_enable_event_data *enable_data = NULL;
    pa_qahw_effect_callback_disable_event_data *disable_data = NULL;
    pa_qahw_loopback_callback_data *pdata = NULL;
    struct pa_qahw_loopback_module_data *mdata = NULL;
    struct pa_qahw_loopback_session_data *ses_data = NULL;

    pa_assert(event_data);
    pa_assert(prv_data);

    pa_log_debug("%s\n", __func__);

    pdata = (pa_qahw_loopback_callback_data *)prv_data;
    if (pdata != NULL)
        mdata = pdata->mdata;

    if (mdata != NULL) {
        if (mdata->loopback_sessions != NULL)
            ses_data = pa_hashmap_get(mdata->loopback_sessions, &pdata->handle);
    }

    pa_log_debug("Handle is %u\n", pdata->handle);

    switch (event_id) {
        case PA_QAHW_EFFECT_EVENT_ENABLE:
            enable_data = (pa_qahw_effect_callback_enable_event_data *)event_data;
            ses_data->latency += enable_data->latency;
            render_window.render_ws = UINT64_MAX + 1 - (ses_data->latency * 1000);
            render_window.render_we = (ses_data->latency * 1000);
            qahw_loopback_set_param_data(mdata->module_handle, pdata->handle, QAHW_PARAM_LOOPBACK_RENDER_WINDOW, (qahw_loopback_param_payload *)(&render_window));
            break;
        case PA_QAHW_EFFECT_EVENT_DISABLE:
            disable_data = (pa_qahw_effect_callback_disable_event_data *)event_data;
            ses_data->latency += disable_data->latency;
            render_window.render_ws = UINT64_MAX + 1 - (ses_data->latency * 1000);
            render_window.render_we = (ses_data->latency * 1000);
            qahw_loopback_set_param_data(mdata->module_handle, pdata->handle, QAHW_PARAM_LOOPBACK_RENDER_WINDOW, (qahw_loopback_param_payload *)(&render_window));
            break;
        default:
            pa_log_debug("Invalid effect event\n");
            break;
    }
}

static void pa_qahw_loopback_free_resources(struct pa_qahw_loopback_session_data *ses_data) {
    pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol, ses_data->obj_path,
                                                    pa_qahw_loopback_session_interface_info.name) >= 0);

    pa_xfree(ses_data->obj_path);
    pa_xfree(ses_data->sink_cfg);

    if (ses_data->jack_handle)
        pa_qahw_jack_deregister_event_callback(ses_data->jack_handle, pa_qahw_loopback_mdata->m);

    pa_xfree(ses_data);
}

static audio_format_t pa_qahw_loopback_check_audio_format(uint32_t audio_format) {
    if ((audio_format == AUDIO_FORMAT_AC3) ||
        (audio_format == AUDIO_FORMAT_E_AC3) ||
        (audio_format == AUDIO_FORMAT_DOLBY_TRUEHD) ||
        (audio_format == AUDIO_FORMAT_MAT))
        return audio_format;

    pa_log_debug("%s: unsupported audio format: 0x%0x, using AC3\n", __func__, audio_format);

    return AUDIO_FORMAT_AC3;
}

static pa_encoding_t pa_qahw_loopback_get_valid_dsp_encoding(pa_encoding_t encoding) {
    switch (encoding) {
        case PA_ENCODING_UNKNOWN_4X_IEC61937:
            return PA_ENCODING_EAC3_IEC61937;
        case PA_ENCODING_UNKNOWN_HBR_IEC61937:
            return PA_ENCODING_MAT_IEC61937;
        case PA_ENCODING_UNKNOWN_IEC61937:
            return PA_ENCODING_AC3_IEC61937;
        default:
            pa_log_info("%s: returning default encoding %d", __func__, encoding);
            return encoding;
    }
}

static pa_qahw_jack_out_config *pa_qahw_loopback_read_port_configuration(char *port_name, pa_qahw_card_port_config *config_port,
                                                             pa_direction_t direction, pa_qahw_loopback_config *loopback_config) {
    pa_qahw_jack_type_t jack_type;
    pa_qahw_jack_in_config *jack_in_config = NULL;
    pa_qahw_jack_out_config *jack_config = NULL;
    const char *linked_port_name = NULL;
    pa_qahw_card_port_config *secondary_config_port = NULL;
    int i = 0;

    pa_assert(config_port);

    jack_type = pa_qahw_util_get_jack_type_from_port_name(port_name);

    jack_in_config = pa_xnew0(pa_qahw_jack_in_config, 1);
    jack_config = pa_xnew0(pa_qahw_jack_out_config, 1);

    pa_qahw_util_get_jack_sys_path(config_port, jack_in_config);

    if (config_port->linked_ports) {
        while ((linked_port_name = config_port->linked_ports[i++])) {
            if (direction == PA_DIRECTION_INPUT)
                secondary_config_port = pa_hashmap_get(loopback_config->in_ports, linked_port_name);
            else
                secondary_config_port = pa_hashmap_get(loopback_config->out_ports, linked_port_name);

            pa_qahw_util_get_jack_sys_path(secondary_config_port, jack_in_config);
            secondary_config_port = NULL;
        }
    }

    switch (jack_type) {
        case PA_QAHW_JACK_TYPE_HDMI_IN:
        case PA_QAHW_JACK_TYPE_HDMI_ARC:
            if (pa_qahw_hdmi_jack_get_config(jack_type, jack_in_config->jack_sys_path, jack_config)) {
                pa_log_error("%s: error in reading hdmi port config", __func__);
                pa_xfree(jack_config);
                jack_config = NULL;
            }

            break;
        case PA_QAHW_JACK_TYPE_SPDIF:
            if (pa_qahw_spdif_jack_get_config(jack_type, jack_in_config->jack_sys_path, jack_config)) {
                pa_log_error("%s: error in reading spdif port config", __func__);
                pa_xfree(jack_config);
                jack_config = NULL;
            }

            break;
        default:
            pa_log_error("Unsupported jack type");
            pa_xfree(jack_config);
            jack_config = NULL;
            break;
    }

    /* FIXME: ensure whether this check is needed */
    /* suppose loopback is requested for hdmi-in but active jack is hdmi-arc */
    /* this case occurs only when secondary ports are supported */
    if ((jack_type != jack_config->active_jack) && (config_port->linked_ports)) {
        pa_log_error("%s: Active jack is not the requested jack", __func__);
        pa_xfree(jack_config);
        jack_config = NULL;
    }

    pa_xfree(jack_in_config);

    return jack_config;
}

static int pa_qahw_loopback_unmarshal_port_config(DBusMessageIter *arg, struct audio_port_config *cfg,
                                      double *port_gain, pa_card *card, pa_hashmap *loopbacks) {

    DBusMessageIter struct_i;
    pa_device_port *p;
    pa_qahw_card_port_device_data *port_device_data;
    pa_encoding_t encoding;
    pa_encoding_t valid_encoding;
    pa_sample_format_t sample_format;
    unsigned int num_channels;
    char *port_name = NULL;
    pa_qahw_jack_out_config *jack_config = NULL;

    pa_qahw_card_port_config *config_port;
    pa_qahw_loopback_config *loopback_config = NULL;

    pa_sample_spec ss;
    pa_channel_map map;
    pa_format_info *format;
    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];
    static pa_sample_spec default_ss = {PA_DEFAULT_PORT_FORMAT, PA_DEFAULT_PORT_RATE, PA_DEFAULT_PORT_CHANNELS};
    pa_channel_map default_map;
    char *kvpair;
    int rc;

    pa_assert(loopbacks);

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
    dbus_message_iter_get_basic(&struct_i, &encoding);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &sample_format);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, port_gain);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &port_name);

    p = pa_hashmap_get(card->ports, port_name);
    if (!p)
        return -1;

    port_device_data = PA_DEVICE_PORT_DATA(p);
    pa_assert(port_device_data);
    cfg->ext.device.type = port_device_data->device;

    if (p->available == PA_AVAILABLE_NO) {
        pa_log_error("%s: port not available", __func__);
        return -1;
    }

    loopback_config = pa_hashmap_first(loopbacks);
    if (p->direction == PA_DIRECTION_INPUT)
        config_port = pa_hashmap_get(loopback_config->in_ports, port_name);
    else
        config_port = pa_hashmap_get(loopback_config->out_ports, port_name);

    if (!(config_port)) {
        pa_log_error("%s: unsupported port %s", __func__, port_name);
        return -1;
    }

    if (cfg->config_mask == PA_QAHW_LOOPBACK_PORT_CONFIG_AUTO) {
        /* if port supports format detection then get it from jack else read the default config from conf */
        if (config_port->format_detection) {
            jack_config = pa_qahw_loopback_read_port_configuration(port_name, config_port, p->direction, loopback_config);

            if (jack_config) {
                num_channels = (jack_config->map).channels;
                sample_format =  (jack_config->ss).format;
                encoding = jack_config->encoding;
                cfg->sample_rate = (jack_config->ss).rate;

                pa_xfree(jack_config);
            } else {
                pa_log_error("%s: %s port read failure", __func__, port_name);
                return -1;
            }
        } else {
            /* intialize default map */
            pa_channel_map_init_auto(&default_map, PA_DEFAULT_PORT_CHANNELS, PA_CHANNEL_MAP_DEFAULT);

            format = pa_idxset_first(config_port->formats, NULL);

            if (pa_qahw_utils_format_to_sample_spec(format, &ss, &map, &default_ss, &default_map)) {
                pa_log_error("%s: No default port config for port %s", __func__, port_name);
                return -1;
            }

            pa_log_info("%s: port %s using config %s", __func__, port_name,
                       pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &ss));

            num_channels = map.channels;
            sample_format =  ss.format;
            encoding = format->encoding;
        }
    }

    cfg->channel_mask = pa_qahw_util_get_channel_mask_from_num_channels(num_channels);

    if (encoding == PA_ENCODING_PCM) {
        cfg->format = pa_qahw_util_get_qahw_format_from_pa_sample(sample_format);
    } else {
        valid_encoding = pa_qahw_loopback_get_valid_dsp_encoding(encoding);
        cfg->format = pa_qahw_util_get_qahw_format_from_pa_encoding(valid_encoding);
    }

    /* Cache the current source config */
    if ((p->direction == PA_DIRECTION_INPUT) && (cfg->config_mask == PA_QAHW_LOOPBACK_PORT_CONFIG_AUTO) &&
                                                                          (config_port->format_detection)) {
        memcpy(&curr_port_config, jack_config, sizeof(pa_qahw_jack_out_config));

        if (encoding == PA_ENCODING_UNKNOWN_4X_IEC61937)
            cfg->sample_rate = 4 * cfg->sample_rate;
    }

    memset(&(cfg->gain), 0, sizeof(struct audio_gain_config));

    cfg->type = AUDIO_PORT_TYPE_DEVICE;

    if (port_device_data->device & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP) {
        kvpair = pa_sprintf_malloc("%s=%d", QAHW_PARAMETER_DEVICE_CONNECT, port_device_data->device);

        rc = qahw_set_parameters(pa_qahw_loopback_mdata->module_handle, kvpair);
        if (rc)
            pa_log_error("qahw routing failed %d",rc);

        pa_log_info("%s: port name: %s kvpair %s device %x", __func__, port_name, kvpair, port_device_data->device);

        pa_xfree(kvpair);
    }

    return 0;
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

static const char *pa_qahw_loopback_get_loopback_event_string(pa_qahw_loopback_event_t event) {
    switch (event) {
        case PA_QAHW_LOOPBACK_EVENT_INVALID:
            return "qahw loopback error event";
        case PA_QAHW_LOOPBACK_EVENT_FORMAT_UPDATE:
            return "qahw loopback format update event";
        case PA_QAHW_LOOPBACK_EVENT_RECREATED:
            return "qahw loopback session recreated event";
        default:
            return NULL;
    }
}

static pa_qahw_jack_type_t pa_qahw_loopback_get_valid_jack_type(pa_qahw_jack_type_t src_jack_type, const char *port_name,
                                                                                    pa_card *card, pa_hashmap *loopbacks) {
    pa_qahw_card_port_config *config_port = NULL;
    pa_qahw_loopback_config *loopback_config = NULL;
    pa_device_port *p = NULL;
    pa_qahw_jack_type_t jack_type = PA_QAHW_JACK_TYPE_INVALID;

    pa_assert(card);
    pa_assert(loopbacks);

    p = pa_hashmap_get(card->ports, port_name);
    if (!p)
        goto exit;

    loopback_config = pa_hashmap_first(loopbacks);
    if (p->direction == PA_DIRECTION_INPUT)
        config_port = pa_hashmap_get(loopback_config->in_ports, port_name);

    if (!config_port) {
        pa_log_error("%s: unsupported port %s", __func__, port_name);
        goto exit;
    }

    if (config_port->port_type) {
        if (pa_streq(config_port->port_type, "secondary")) {
            /* Get primary jack */
            pa_log_info("%s: primary port for %s is %s", __func__, port_name, config_port->primary_port_name);
            jack_type = pa_qahw_util_get_jack_type_from_port_name(config_port->primary_port_name);
        } else {
            jack_type = src_jack_type;
        }
    } else {
        /* If port-type not mentioned, means port is primary */
        jack_type = src_jack_type;
    }

exit:
    return jack_type;
}

static bool pa_qahw_loopback_config_change_valid(pa_qahw_jack_out_config *port_config) {
    bool status = false;

    /* port_config will be NULL during ADSP event. For ADSP event return true */
    if (!port_config)
        return true;

    /* Check whether there is any difference in configs */
    if ((curr_port_config.encoding != port_config->encoding) ||
         memcmp(&(curr_port_config.ss), &(port_config->ss), sizeof(pa_sample_spec)) ||
         memcmp(&(curr_port_config.map), &(port_config->map), sizeof(pa_channel_map))) {
        status = true;
        memcpy(&curr_port_config, port_config, sizeof(pa_qahw_jack_out_config));
    }

    return status;
}

static pa_qahw_loopback_event_t pa_qahw_loopback_restart_loopback_session(struct pa_qahw_loopback_session_data *ses_data,
                                                                                    pa_qahw_jack_out_config *port_config) {
    int status = 0;
    int num_srcs = 1;
    qahw_source_port_config_t source_port_config;
    qahw_sink_port_config_t sink_port_config;

    pa_qahw_loopback_event_t loopback_event = PA_QAHW_LOOPBACK_EVENT_INVALID;
    audio_patch_handle_t handle = AUDIO_PATCH_HANDLE_NONE;

    pa_assert(ses_data);

    /* If session already closed, restart with new config */
    if (ses_data->ses_handle == AUDIO_PATCH_HANDLE_NONE) {
        pa_log_info("%s: session already closed, restarting with new configs", __func__);

        /* Notify PA_QAHW_LOOPBACK_EVENT_STARTED to QAHW card module */
        ses_data->common->callback(ses_data->src_port, PA_QAHW_LOOPBACK_EVENT_STARTED, ses_data->common->prv_data);

        source_port_config.source_config = &(ses_data->src_cfg);
        sink_port_config.sink_config = ses_data->sink_cfg;

        source_port_config.flags = QAHW_INPUT_FLAG_PASSTHROUGH | QAHW_INPUT_FLAG_COMPRESS;
        sink_port_config.flags = 0;

        source_port_config.num_sources = num_srcs;
        sink_port_config.num_sinks = ses_data->num_sinks;

        status = qahw_create_audio_patch_v2(ses_data->common->module_handle,
                                    &source_port_config,
                                    &sink_port_config,
                                    &handle);

        if (status) {
            pa_log_error("Create audio patch failed with status %d", status);
            loopback_event = PA_QAHW_LOOPBACK_EVENT_INVALID;
        } else {
            ses_data->ses_handle = handle;
            loopback_event = PA_QAHW_LOOPBACK_EVENT_RECREATED;

            /* Set format update event callback */
            pa_qahw_loopback_set_format_update_callback(ses_data);

            memcpy(&(curr_port_config), port_config, sizeof(pa_qahw_jack_out_config));
        }

        return loopback_event;
    }

    /* Check for change in config */
    if (!pa_qahw_loopback_config_change_valid(port_config)) {
        pa_log_error("%s: no valid config change with current session", __func__);
        return loopback_event;
    }

    pa_log_info("%s: restarting loopback with updated port configs", __func__);

    status = qahw_release_audio_patch(ses_data->common->module_handle, ses_data->ses_handle);
    if (status) {
        pa_log_error("Release audio patch failed with status %d", status);
        loopback_event = PA_QAHW_LOOPBACK_EVENT_INVALID;
    } else {
        ses_data->ses_handle = AUDIO_PATCH_HANDLE_NONE;

        /* Create new loopback session */

    source_port_config.source_config = &(ses_data->src_cfg);
    sink_port_config.sink_config = ses_data->sink_cfg;

    source_port_config.flags = QAHW_INPUT_FLAG_PASSTHROUGH | QAHW_INPUT_FLAG_COMPRESS;
    sink_port_config.flags = 0;

    source_port_config.num_sources = num_srcs;
    sink_port_config.num_sinks = ses_data->num_sinks;

    status = qahw_create_audio_patch_v2(ses_data->common->module_handle,
                                    &source_port_config,
                                    &sink_port_config,
                                    &handle);

        if (status) {
            pa_log_error("Create audio patch failed with status %d", status);
            loopback_event = PA_QAHW_LOOPBACK_EVENT_INVALID;
        } else {
            ses_data->ses_handle = handle;
            loopback_event = PA_QAHW_LOOPBACK_EVENT_RECREATED;

            /* Set format update event callback */
            pa_qahw_loopback_set_format_update_callback(ses_data);
        }
    }

    return loopback_event;
}

static void pa_qahw_loopback_raise_signal_to_client(const char *event_string, pa_qahw_jack_out_config *port_config,
                                                                    struct pa_qahw_loopback_session_data *ses_data) {
    const char *encoding_string = NULL;
    const char *channel_map_string = NULL;
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, array_i;
    int i = 0;

    pa_assert(event_string);
    pa_assert(port_config);
    pa_assert(ses_data);

    /* Generate encoding string */
    encoding_string = pa_encoding_to_string(port_config->encoding);

    /* Raising signal to client on the event of format change detection */
    pa_assert_se(message = dbus_message_new_signal(ses_data->obj_path, pa_qahw_loopback_session_interface_info.name,
                                                                    det_event_signals[SIGNAL_DETECTION_EVENT].name));

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_STRING, &(event_string));
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_STRING, &(ses_data->src_port));
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &((port_config->ss).rate));
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &((port_config->ss).channels));
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_STRING, &(encoding_string));

    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "s", &array_i);
    for (i = 0; i < NUM_CHANNEL_POSITIONS; i++) {
        channel_map_string = pa_channel_position_to_string((port_config->map).map[i]);
        if (!channel_map_string)
            channel_map_string = "nil";

        dbus_message_iter_append_basic(&array_i, DBUS_TYPE_STRING, &channel_map_string);
    }
    dbus_message_iter_close_container(&arg_i, &array_i);

    pa_dbus_protocol_send_signal(ses_data->common->dbus_protocol, message);

    dbus_message_unref(message);
}

static pa_hook_result_t pa_qahw_loopback_jack_callback(void *dummy __attribute__((unused)), pa_qahw_jack_event_data_t *event_data, void *prv_data) {
    pa_qahw_jack_event_t jack_event;
    pa_qahw_loopback_event_t loopback_event;
    pa_qahw_jack_out_config *port_config;
    struct pa_qahw_loopback_session_data *ses_data;
    const char *loopback_event_string = NULL;
    pa_encoding_t valid_encoding;
    int status = 0;
    bool close_session = false;

    pa_assert(prv_data);
    pa_assert(event_data);

    pa_log_info("%s: jack event received, jack_type %d event %d\n", __func__, event_data->jack_type, event_data->event);

    jack_event = event_data->event;
    ses_data = (struct pa_qahw_loopback_session_data *)prv_data;

    if ((jack_event != PA_QAHW_JACK_CONFIG_UPDATE) && (jack_event != PA_QAHW_JACK_NO_VALID_STREAM)) {
        pa_log_error("%s: Unsupported qahw jack event", __func__);
        return PA_HOOK_CANCEL;
    }

    if (jack_event == PA_QAHW_JACK_NO_VALID_STREAM) {
        pa_log_error("%s: No valid stream event, closing session", __func__);
        close_session = true;
    } else if (jack_event == PA_QAHW_JACK_CONFIG_UPDATE) {
        pa_assert(event_data->pa_qahw_jack_info);
        port_config = (pa_qahw_jack_out_config *)event_data->pa_qahw_jack_info;

        if (ses_data->src_jack_type != port_config->active_jack) {
            pa_log_error("%s: unsupported active jack, closing session", __func__);
            close_session = true;
        }
    }

    if (close_session) {
        status = qahw_release_audio_patch(ses_data->common->module_handle, ses_data->ses_handle);
        if (status) {
            pa_log_error("Release audio patch failed with status %d", status);
            loopback_event = PA_QAHW_LOOPBACK_EVENT_INVALID;
            return PA_HOOK_CANCEL;
        }

        /* Notify PA_QAHW_LOOPBACK_EVENT_STOPPED to QAHW card module */
        ses_data->common->callback(ses_data->src_port, PA_QAHW_LOOPBACK_EVENT_STOPPED, ses_data->common->prv_data);
        ses_data->ses_handle = AUDIO_PATCH_HANDLE_NONE;
        memset(&(curr_port_config), 0, sizeof(pa_qahw_jack_out_config));
        return PA_HOOK_CANCEL;
    }

    loopback_event = PA_QAHW_LOOPBACK_EVENT_FORMAT_UPDATE;

    ses_data->src_cfg.sample_rate = (port_config->ss).rate;
    ses_data->src_cfg.channel_mask = pa_qahw_util_get_channel_mask_from_num_channels((port_config->ss).channels);

    if (port_config->encoding == PA_ENCODING_PCM) {
        ses_data->src_cfg.format = pa_qahw_util_get_qahw_format_from_pa_sample((port_config->ss).format);
    } else {
        valid_encoding = pa_qahw_loopback_get_valid_dsp_encoding(port_config->encoding);
        ses_data->src_cfg.format = pa_qahw_util_get_qahw_format_from_pa_encoding(valid_encoding);
    }

    /* Cache the current source config */
    if (port_config->encoding == PA_ENCODING_UNKNOWN_4X_IEC61937)
        ses_data->src_cfg.sample_rate = 4 * ses_data->src_cfg.sample_rate;

    if (ses_data->src_config_mask == PA_QAHW_LOOPBACK_PORT_CONFIG_AUTO)
        loopback_event = pa_qahw_loopback_restart_loopback_session(ses_data, port_config);

    /* Generate event string based on event type */
    loopback_event_string = pa_qahw_loopback_get_loopback_event_string(loopback_event);

    /* Raise signal to client */
    pa_qahw_loopback_raise_signal_to_client(loopback_event_string, port_config, ses_data);

    return PA_HOOK_OK;
}

static int pa_qahw_loopback_format_update_callback(qahw_stream_callback_event_t event, void *param, void *cookie) {
    uint32_t *payload = NULL;
    struct pa_qahw_loopback_session_data *ses_data = NULL;
    pa_qahw_loopback_event_t loopback_event = PA_QAHW_LOOPBACK_EVENT_INVALID;
    audio_format_t new_codec_format;
    const char *loopback_event_string = NULL;
    int i = 0;

    pa_assert(param);
    pa_assert(cookie);

    payload = (uint32_t *)param;
    ses_data = (struct pa_qahw_loopback_session_data *)cookie;

    pa_assert(ses_data->common->loopbacks);

    switch (event) {
        case QAHW_STREAM_CBK_EVENT_ADSP:
            if ((payload[0] == QAHW_STREAM_IEC_61937_FMT_UPDATE_EVENT) && (payload[1] == sizeof(int))) {
                pa_log_info("%s: received IEC61937 format update event with qahw audio format 0x%x", __func__, payload[2]);
                new_codec_format = pa_qahw_loopback_check_audio_format(payload[2]);

                loopback_event = PA_QAHW_LOOPBACK_EVENT_FORMAT_UPDATE;

                /* Restart loopback with new configs */
                if (ses_data->src_config_mask == PA_QAHW_LOOPBACK_PORT_CONFIG_AUTO) {
                    if (ses_data->src_cfg.format != new_codec_format) {
                        ses_data->src_cfg.format = new_codec_format;

                        /* For ADSP event, pass jack_config as NULL */
                        loopback_event = pa_qahw_loopback_restart_loopback_session(ses_data, NULL);
                    } else {
                        pa_log_debug("%s: No change in reported format", __func__);
                        return -1;
                    }
                }

                /* Generate event string based on event type */
                loopback_event_string = pa_qahw_loopback_get_loopback_event_string(loopback_event);

                /* Raise signal to client */
                pa_qahw_loopback_raise_signal_to_client(loopback_event_string, &curr_port_config, ses_data);
            } else {
                pa_log_error("%s: unsupported event %d, param length %d", __func__, payload[0], payload[1]);

                for (i = 2; i * sizeof(uint32_t) <= payload[1]; i++)
                    pa_log_error("%s: param[%d] 0x%x", __func__, i, payload[i]);
            }

            break;
        default:
            break;
    }

    return 0;
}

static int pa_qahw_loopback_set_format_update_callback(struct pa_qahw_loopback_session_data *session_data) {
    qahw_loopback_param_payload payload;
    int ret = 0;

    pa_log_info("%s: set format update event callback", __func__);

    payload.stream_callback_params.cb = pa_qahw_loopback_format_update_callback;
    payload.stream_callback_params.cookie = (void *)session_data;

    ret = qahw_loopback_set_param_data(session_data->common->module_handle, session_data->ses_handle,
                                                          QAHW_PARAM_LOOPBACK_SET_CALLBACK, &payload);
    if (ret < 0)
        pa_log_error("%s: set callback failed with error %d", __func__, ret);

    return ret;
}

int pa_set_metadata_av_window_mat(qahw_module_handle_t *hw_module,
                                  audio_patch_handle_t handle)
{
    qahw_loopback_param_payload payload;
    int ret = 0;

    pa_log_error("Set the AV sync meta data params using qahw_loopback_set_param_data\n");

    payload.render_window_params.render_ws = 0xFFFFFFFFFFFE7960;
    payload.render_window_params.render_we = 0x00000000000186A0;

    ret = qahw_loopback_set_param_data(hw_module, handle,
        QAHW_PARAM_LOOPBACK_RENDER_WINDOW, &payload);

    if (ret < 0) {
        pa_log_error("qahw_loopback_set_param_data av render failed with err %d\n", ret);
        goto done;
    }
done:
    return ret;
}

/******* Module specific function ********/
static void pa_qahw_loopback_create(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int status = 0;
    dbus_uint32_t i;
    struct pa_qahw_loopback_module_data *u;
    qahw_module_handle_t *module_handle;
    audio_patch_handle_t handle = AUDIO_PATCH_HANDLE_NONE;
    struct pa_qahw_loopback_session_data *ses_data = NULL;
    pa_qahw_loopback_config *loopback_config = NULL;
    pa_qahw_loopback_handle_to_name *mapping = NULL;

    dbus_uint32_t num_srcs = 1;
    double src_port_gain;
    struct audio_port_config src_cfg;

    dbus_uint32_t num_sinks;
    double *sink_port_gain = NULL;
    struct audio_port_config *sink_cfg = NULL;

    struct audio_port_config sink_gain_config;
    int loopback_gain_in_millibels;
    pa_qahw_jack_type_t jack_type = PA_QAHW_JACK_TYPE_INVALID;

    pa_qahw_effect_callback_config cb;
    pa_qahw_loopback_callback_data *pdata;

    qahw_source_port_config_t source_port_config;
    qahw_sink_port_config_t sink_port_config;

    DBusMessage *reply = NULL;
    DBusError error;
    DBusMessageIter arg_i, array_i;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    u = (struct pa_qahw_loopback_module_data *)userdata;
    module_handle = u->module_handle;
    loopback_config = pa_hashmap_first(u->loopbacks);

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
    if (pa_qahw_loopback_unmarshal_port_config(&arg_i, &src_cfg, &src_port_gain, u->card, u->loopbacks)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Create failed");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    /* FIXME: Use dbus_message_iter_get_element_count() when we move to newer Yocto */
    num_sinks = pa_qahw_loopback_get_array_size(array_i);

    sink_cfg = pa_xnew0(struct audio_port_config, num_sinks);
    sink_port_gain = pa_xnew0(double, num_sinks);

    for(i = 0; i < num_sinks; i++) {
        if (pa_qahw_loopback_unmarshal_port_config(&array_i, &sink_cfg[i], &sink_port_gain[i], u->card, u->loopbacks)) {
            pa_xfree(sink_cfg);
            pa_xfree(sink_port_gain);

            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Create failed");
            dbus_error_free(&error);
            return;
        }

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

    /* Create session data */
    ses_data = pa_xnew0(struct pa_qahw_loopback_session_data, 1);
    ses_data->common = u;
    ses_data->src_port = pa_qahw_util_audio_device_to_port_name(src_cfg.ext.device.type, u->card->ports);
    ses_data->src_config_mask = src_cfg.config_mask;
    memcpy(&(ses_data->src_cfg), &src_cfg, sizeof(struct audio_port_config) * num_srcs);

    ses_data->sink_cfg = pa_xnew0(struct audio_port_config, num_sinks);
    memcpy(ses_data->sink_cfg, sink_cfg, sizeof(struct audio_port_config) * num_sinks);
    ses_data->num_sinks = num_sinks;
    ses_data->src_jack_type = pa_qahw_util_get_jack_type_from_port_name(ses_data->src_port);

    if (src_cfg.config_mask == PA_QAHW_LOOPBACK_PORT_CONFIG_AUTO) {
        src_cfg.config_mask = AUDIO_PORT_CONFIG_ALL ^ AUDIO_PORT_CONFIG_GAIN;
        ses_data->src_cfg.config_mask = AUDIO_PORT_CONFIG_ALL ^ AUDIO_PORT_CONFIG_GAIN;
    }

    /* Notify PA_QAHW_LOOPBACK_EVENT_STARTED to QAHW card module */
    u->callback(ses_data->src_port, PA_QAHW_LOOPBACK_EVENT_STARTED, u->prv_data);

    pa_log_info("Creating audio loopback patch\n");

    source_port_config.source_config = &(src_cfg);
    sink_port_config.sink_config = sink_cfg;

    source_port_config.flags = QAHW_INPUT_FLAG_PASSTHROUGH | QAHW_INPUT_FLAG_COMPRESS;
    sink_port_config.flags = 0;

    source_port_config.num_sources = num_srcs;
    sink_port_config.num_sinks = num_sinks;

    status = qahw_create_audio_patch_v2(module_handle,
                                    &source_port_config,
                                    &sink_port_config,
                                    &handle);
    pa_log_debug("Create audio loopback patch returned status: %d, handle %u\n", status, handle);

    if (ses_data->src_cfg.format == AUDIO_FORMAT_MAT) {
        pa_set_metadata_av_window_mat(module_handle, handle);
        pa_log_info("Setting AV sync for MAT loopback case\n");
    }
    ses_data->ses_handle = handle;

    if (!status) {
        /* Set format update event callback */
        pa_qahw_loopback_set_format_update_callback(ses_data);

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
        /* Unsuspend source if loopback fails to create */
        u->callback(ses_data->src_port, PA_QAHW_LOOPBACK_EVENT_STOPPED, u->prv_data);
        pa_xfree(ses_data->sink_cfg);
        pa_xfree(ses_data);
        pa_xfree(sink_cfg);
        pa_xfree(sink_port_gain);

        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Create failed");
        dbus_error_free(&error);
        return;
    }

    ses_data->obj_path = pa_sprintf_malloc("%s/ses_%u", u->dbus_path, handle);
    ses_data->latency = 5;
    pa_hashmap_put(ses_data->common->loopback_sessions, &ses_data->ses_handle, ses_data);

    mapping = pa_xnew0(pa_qahw_loopback_handle_to_name, 1);
    mapping->handle = handle;
    mapping->name = pa_xstrdup(loopback_config->name);
    pa_hashmap_put(u->loopback_mappings, &handle, mapping);

    pa_log_info("session obj path %s \n", ses_data->obj_path);

    pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol, ses_data->obj_path,
                                                &pa_qahw_loopback_session_interface_info, ses_data) >= 0);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH, &ses_data->obj_path);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    pdata = pa_xnew0(pa_qahw_loopback_callback_data, 1);
    pdata->mdata = ses_data->common;
    pdata->handle = ses_data->ses_handle;

    cb.endpoint_name = pa_xstrdup(loopback_config->name);
    cb.cb_func = pa_qahw_loopback_cb;
    cb.prv_data = pdata;
    status = pa_qahw_effect_register_callback(ses_data->common->effect_handle, &cb);

    if (status == 0)
        pa_log_debug("Callback registered successfully\n");

    /* Check whether the jack is secondary jack before registering for event */
    /* If secondary jack, then register event for its primary jack */
    jack_type = pa_qahw_loopback_get_valid_jack_type(ses_data->src_jack_type, ses_data->src_port,
                                                                           u->card, u->loopbacks);

    if (jack_type != PA_QAHW_JACK_TYPE_INVALID)
        ses_data->jack_handle = pa_qahw_jack_register_event_callback(jack_type, pa_qahw_loopback_jack_callback,
                                                             pa_qahw_loopback_mdata->m, NULL, (void *)ses_data);

    if (ses_data->jack_handle)
        pa_log_info("Register jack event callback successful\n");
    else
        pa_log_error("Register jack event callback failed\n");

    pa_xfree(cb.endpoint_name);

    pa_xfree(sink_cfg);
    pa_xfree(sink_port_gain);
}

static void pa_qahw_loopback_get_port_config(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pa_qahw_loopback_module_data *u;
    int status;
    struct audio_port port;
    struct audio_port_config port_cfg;
    const char *port_name = NULL;
    double gain;
    pa_encoding_t pa_format;
    pa_sample_format_t pa_sample_format;
    unsigned int num_channels;

    DBusError error;
    DBusMessageIter arg_i, struct_i;
    DBusMessage *reply = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    u = (struct pa_qahw_loopback_module_data *) userdata;

    dbus_error_init(&error);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_INT32, &port.id, DBUS_TYPE_INT32, &port.role, DBUS_TYPE_INVALID)) {
        pa_log_error("Invalid signature for GetPortConfig - %s\n", error.message);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for GetPortConfig");
        dbus_error_free(&error);
        return;
    }

    port.type = AUDIO_PORT_TYPE_DEVICE;

    pa_log_info("Calling QAHW get audio port\n");
    status = qahw_get_audio_port(u->module_handle, &port);
    if (status != E_OK) {
        pa_log_error("QAHW get audio port failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "GetPortConfig failed");
        dbus_error_free(&error);
        return;
    }

    port_cfg = port.active_config;
    port_name = pa_qahw_util_audio_device_to_port_name(port_cfg.ext.device.type, u->card->ports);

    /* Converting back from millibels */
    gain = pow(10, ((double)port_cfg.gain.values[0] / MILLIBELS_CONSTANT));
    pa_format = pa_qahw_util_get_pa_encoding_from_qahw_format(port_cfg.format);
    pa_sample_format = pa_qahw_util_get_pa_sample_from_qahw_format(port_cfg.format);
    num_channels = pa_qahw_util_get_num_channels_from_channel_mask(port_cfg.channel_mask);

    pa_log_debug("pa format %d\n", pa_format);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &port_cfg.id);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &port_cfg.role);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &port_cfg.config_mask);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &port_cfg.sample_rate);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32, &num_channels);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &pa_format);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &pa_sample_format);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_DOUBLE, &gain);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_STRING, &port_name);
    dbus_message_iter_close_container(&arg_i, &struct_i);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

static void pa_qahw_loopback_set_port_config(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pa_qahw_loopback_module_data *u;
    double gain;
    struct audio_port_config cfg;
    int status;
    double gain_in_millibels;

    DBusError error;
    DBusMessageIter arg_i;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    u = (struct pa_qahw_loopback_module_data *)userdata;

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_log_error("SetPortConfig has no arguments\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "SetPortConfig has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(iiuuuiids)")) {
        pa_log_error("Invalid signature for SetPortConfig\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Invalid signature for SetPortConfig");
        dbus_error_free(&error);
        return;
    }

    pa_log_info("Unmarshalling SetPortConfig message\n");
    if (pa_qahw_loopback_unmarshal_port_config(&arg_i, &cfg, &gain, u->card,
                                                      u->loopbacks)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "SetPortConfig failed");
        dbus_error_free(&error);
        return;
    }

    gain_in_millibels = MILLIBELS_CONSTANT * log10(gain);

    cfg.gain.index = 0;
    cfg.gain.mode = AUDIO_GAIN_MODE_JOINT;
    cfg.gain.channel_mask = 1;
    cfg.gain.values[0] = gain_in_millibels;

    status = qahw_set_audio_port_config(u->module_handle, &cfg);

    if (status != E_OK)
        pa_log_error("QAHW set port config failed\n");

    pa_dbus_send_empty_reply(conn, msg);
}

static void pa_qahw_loopback_free_session(struct pa_qahw_loopback_session_data *ses_data) {
    pa_assert(ses_data);

    if (ses_data->obj_path != NULL) {
        pa_xfree(ses_data->obj_path);
        ses_data->obj_path = NULL;
    }

    pa_xfree(ses_data);
}

static void pa_qahw_loopback_free_info(pa_qahw_loopback_handle_to_name *loopback) {
    pa_assert(loopback);

    if (loopback->name != NULL)
        pa_xfree(loopback->name);

    pa_xfree(loopback);
}

/******* Session specific function ********/
static void pa_qahw_loopback_stop(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pa_qahw_loopback_session_data *ses_data = (struct pa_qahw_loopback_session_data *)userdata;
    int status = -1;
    dbus_int32_t handle = ses_data->ses_handle;
    char *loopback_name = NULL;

    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    loopback_name = pa_qahw_loopback_get_name_from_handle(ses_data->ses_handle);
    status = pa_qahw_effect_deregister_callback(ses_data->common->effect_handle, loopback_name);

    if (status < 0)
        pa_log_error("%s: pa_qahw_effect_deregister_callback failed\n", __func__);

    if (ses_data->ses_handle == AUDIO_PATCH_HANDLE_NONE) {
        pa_log_error("%s: requested session handle is invalid", __func__);

        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "session ID is not valid");
        dbus_error_free(&error);

        /* Notify PA_QAHW_LOOPBACK_EVENT_STOPPED to QAHW card module */
        ses_data->common->callback(ses_data->src_port, PA_QAHW_LOOPBACK_EVENT_STOPPED, ses_data->common->prv_data);

        pa_hashmap_remove(ses_data->common->loopback_sessions, &ses_data->ses_handle);
        pa_hashmap_remove(ses_data->common->loopback_mappings, &ses_data->ses_handle);

        pa_qahw_loopback_free_resources(ses_data);

        return;
    }

    status = qahw_release_audio_patch(ses_data->common->module_handle, handle);
    if (status != E_OK) {
        pa_log_error("QAHW release audio patch failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "QAHW release audio patch failed");
        dbus_error_free(&error);

        pa_hashmap_remove(ses_data->common->loopback_sessions, &ses_data->ses_handle);
        pa_hashmap_remove(ses_data->common->loopback_mappings, &ses_data->ses_handle);

        pa_qahw_loopback_free_resources(ses_data);

        return;
    }

    /* Notify PA_QAHW_LOOPBACK_EVENT_STOPPED to QAHW card module */
    ses_data->common->callback(ses_data->src_port, PA_QAHW_LOOPBACK_EVENT_STOPPED, ses_data->common->prv_data);

    pa_hashmap_remove(ses_data->common->loopback_sessions, &ses_data->ses_handle);
    pa_hashmap_remove(ses_data->common->loopback_mappings, &ses_data->ses_handle);

    pa_qahw_loopback_free_resources(ses_data);

    pa_dbus_send_empty_reply(conn, msg);
}

/******* public functions ********/
void pa_qahw_loopback_init(qahw_module_handle_t *module_handle, pa_core *core, pa_card *card,
                           pa_hashmap *loopbacks, pa_qahw_loopback_callback_t callback,
                           void *prv_data, pa_qahw_effect_handle_t effect_handle, pa_module *m) {
    pa_assert(module_handle);
    pa_assert(core);
    pa_assert(card);
    pa_assert(m);
    pa_assert(loopbacks);

    if (pa_hashmap_size(loopbacks) != PA_QAHW_LOOPBACK_MAX_SESSIONS) {
        pa_log_error("%s: invalid loopback session count %d", __func__, pa_hashmap_size(loopbacks));
        return;
    }

    memset(&curr_port_config, 0, sizeof(pa_qahw_jack_out_config));

    pa_qahw_loopback_mdata = pa_xnew0(struct pa_qahw_loopback_module_data, 1);

    pa_qahw_loopback_mdata->dbus_path = pa_sprintf_malloc("%s/%s", PA_QAHW_LOOPBACK_DBUS_OBJECT_PATH_PREFIX, "loopback");

    pa_qahw_loopback_mdata->dbus_protocol = pa_dbus_protocol_get(core);

    pa_qahw_loopback_mdata->module_handle = module_handle;
    pa_qahw_loopback_mdata->card = card;
    pa_qahw_loopback_mdata->m = m;
    pa_qahw_loopback_mdata->callback = callback;
    pa_qahw_loopback_mdata->prv_data = prv_data;
    pa_qahw_loopback_mdata->loopbacks = loopbacks;
    pa_qahw_loopback_mdata->effect_handle = effect_handle;

    pa_qahw_loopback_mdata->loopback_mappings = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                                    pa_idxset_string_compare_func, NULL,
                                                                    (pa_free_cb_t)pa_qahw_loopback_free_info);

    pa_qahw_loopback_mdata->loopback_sessions = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                                                    pa_idxset_string_compare_func, NULL,
                                                                    (pa_free_cb_t)pa_qahw_loopback_free_session);

    pa_assert_se(pa_dbus_protocol_add_interface(pa_qahw_loopback_mdata->dbus_protocol,
                                                pa_qahw_loopback_mdata->dbus_path,
                                                &pa_qahw_loopback_module_interface_info,
                                                pa_qahw_loopback_mdata) >= 0);
}

void pa_qahw_loopback_deinit(void) {
    if (pa_qahw_loopback_mdata) {
        if (pa_qahw_loopback_mdata->dbus_path && pa_qahw_loopback_mdata->dbus_protocol)
            pa_assert_se(pa_dbus_protocol_remove_interface(pa_qahw_loopback_mdata->dbus_protocol,
                                                           pa_qahw_loopback_mdata->dbus_path,
                                                           pa_qahw_loopback_module_interface_info.name) >= 0);

        if (pa_qahw_loopback_mdata->dbus_path)
            pa_xfree(pa_qahw_loopback_mdata->dbus_path);

        if (pa_qahw_loopback_mdata->dbus_protocol)
            pa_dbus_protocol_unref(pa_qahw_loopback_mdata->dbus_protocol);

        if (pa_qahw_loopback_mdata->loopback_mappings)
            pa_hashmap_free(pa_qahw_loopback_mdata->loopback_mappings);

        if (pa_qahw_loopback_mdata->loopback_sessions)
            pa_hashmap_free(pa_qahw_loopback_mdata->loopback_sessions);

        pa_xfree(pa_qahw_loopback_mdata);
    }
}
