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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>

#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/core-util.h>

#include "qahw-jack-common.h"
#include "qahw-jack-format.h"

#define SOCKET_BUFFER_SIZE 64 * 1024
#define SPDIF_UEVENT_NAME "PRI_SPDIF_TX=MEDIA_CONFIG_CHANGE"
#define SPDIF_CH_STATUS_UEVENT_NAME "PRI_SPDIF_TX=CHANNEL_STATUS_CHANGE"

#define QAHW_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qahw/port"
#define QAHW_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Qahw.Module"

enum module_method_handler_index {
    METHOD_HANDLER_SET_CHANNEL_BIT_MASK = 0,
    METHOD_HANDLER_MODULE_LAST = METHOD_HANDLER_SET_CHANNEL_BIT_MASK,
    METHOD_HANDLER_MODULE_MAX = METHOD_HANDLER_MODULE_LAST + 1,
};

enum signal_index {
    SIGNAL_CHANNEL_STATUS_UPDATE,
    SIGNAL_MAX
};

typedef struct {
    int fd;
    pa_io_event *io;
    pa_hook event_hook;
    pa_qahw_jack_type_t jack_type;
    pa_qahw_jack_in_config *jack_in_config;
    pa_qahw_jack_type_t active_port_type;
    char *obj_path;
    pa_dbus_protocol *dbus_protocol;
    qahw_module_handle_t *module_handle;
    char *channel_status;
} pa_qahw_spdif_jack_data_t;

pa_qahw_jack_out_config curr_spdif_jack_config;

static void pa_qahw_jack_spdif_set_channel_bit_mask(DBusConnection *conn, DBusMessage *msg, void *userdata);
static int32_t pa_qahw_jack_spdif_read_ch_status(pa_qahw_spdif_jack_data_t *spdif_jdata);
static void pa_qahw_jack_spdif_raise_signal(pa_qahw_spdif_jack_data_t *spdif_jdata);

static pa_dbus_arg_info set_channel_bit_mask_args[] = {
    {"bit_mask", "ay", "in"},
};

static pa_dbus_arg_info channel_status_update_args[] = {
    {"channel_status_info", "ay", NULL},
};

static pa_dbus_method_handler module_method_handlers[METHOD_HANDLER_MODULE_MAX] = {
[METHOD_HANDLER_SET_CHANNEL_BIT_MASK] = {
        .method_name = "SetChBitMask",
        .arguments = set_channel_bit_mask_args,
        .n_arguments = sizeof(set_channel_bit_mask_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = pa_qahw_jack_spdif_set_channel_bit_mask },
};

static pa_dbus_signal_info ch_status_update_signal[SIGNAL_MAX] = {
    [SIGNAL_CHANNEL_STATUS_UPDATE] = {
        .name = "ChannelStatusUpdate",
        .arguments = channel_status_update_args,
        .n_arguments = sizeof(channel_status_update_args)/sizeof(pa_dbus_arg_info)},
};

pa_dbus_interface_info pa_qahw_jack_spdif_module_interface_info = {
    .name = QAHW_DBUS_MODULE_IFACE,
    .method_handlers = module_method_handlers,
    .n_method_handlers = METHOD_HANDLER_MODULE_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = ch_status_update_signal,
    .n_signals = SIGNAL_MAX
};

static int poll_data_event_init(pa_qahw_jack_type_t jack_type) {
    struct sockaddr_nl sock_addr;
    int sz = SOCKET_BUFFER_SIZE;
    int soc = -1;

    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.nl_family = AF_NETLINK;
    sock_addr.nl_pid = getpid() + jack_type;
    sock_addr.nl_groups = 0xffffffff;

    soc = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (soc < 0)
        return soc;

    if (setsockopt(soc, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) < 0) {
        pa_log_error("setsockopt %s", strerror(errno));
        close(soc);
        soc = -1;
    }

    if (bind(soc, (struct sockaddr*) &sock_addr, sizeof(sock_addr)) < 0) {
        pa_log_error("bind %s", strerror(errno));
        close(soc);
        soc = -1;
    }

    return soc;
}

static void jack_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t io_events, void *userdata) {
    pa_qahw_spdif_jack_data_t *spdif_jdata = userdata;

    char buffer[SOCKET_BUFFER_SIZE];
    int count, iterator = 0;
    bool audio_change_detected;
    pa_qahw_jack_event_data_t event_data;
    static pa_qahw_jack_out_config new_port_config;

    pa_assert(spdif_jdata);
    event_data.jack_type = spdif_jdata->jack_type;

    count = recv(spdif_jdata->fd, buffer, (SOCKET_BUFFER_SIZE), 0 );

    if (count > 0) {
        buffer[count] = '\0';
        audio_change_detected = false;

        for (iterator = 0; iterator < count;) {
            if (pa_strneq(&buffer[iterator], SPDIF_UEVENT_NAME, strlen(SPDIF_UEVENT_NAME))) {
                audio_change_detected = true;
                iterator += strlen(SPDIF_UEVENT_NAME);
                continue;
            } else if (pa_strneq(&buffer[iterator], SPDIF_CH_STATUS_UEVENT_NAME, strlen(SPDIF_CH_STATUS_UEVENT_NAME))) {
                iterator += strlen(SPDIF_CH_STATUS_UEVENT_NAME);
                if (pa_qahw_jack_spdif_read_ch_status(spdif_jdata) >= 0)
                    pa_qahw_jack_spdif_raise_signal(spdif_jdata);
                continue;
            }

            iterator++;
        }

        if (audio_change_detected) {
            if (!pa_qahw_spdif_jack_get_config(spdif_jdata->active_port_type, spdif_jdata->jack_in_config->jack_sys_path, &new_port_config)) {
                if ((new_port_config.active_jack != PA_QAHW_JACK_TYPE_INVALID) ||
                    (new_port_config.preemph_status != curr_spdif_jack_config.preemph_status)) {
                    pa_log_info("qahw jack type %d config update", spdif_jdata->jack_type);
                    event_data.event = PA_QAHW_JACK_CONFIG_UPDATE;
                    event_data.pa_qahw_jack_info = &new_port_config;
                    pa_hook_fire(&(spdif_jdata->event_hook), &event_data);
                    spdif_jdata->active_port_type = PA_QAHW_JACK_TYPE_SPDIF;
                    memcpy(&curr_spdif_jack_config, &new_port_config, sizeof(pa_qahw_jack_out_config));
                }
            }
        }
    }
}

struct pa_qahw_jack_data* pa_qahw_spdif_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m,
                                               pa_hook_slot **hook_slot, pa_qahw_jack_callback_t callback,
                                                pa_qahw_jack_in_config *jack_in_config, void *client_data,
                                                                      qahw_module_handle_t *module_handle) {
    struct pa_qahw_jack_data *jdata = NULL;
    int sock_event_fd = -1;
    pa_qahw_spdif_jack_data_t *spdif_jdata = NULL;
    static pa_qahw_jack_out_config new_port_config;
    pa_qahw_jack_event_data_t event_data;
    const char *port_name = NULL;
    char *port_name_underscore = NULL;

    sock_event_fd = poll_data_event_init(jack_type);
    if (sock_event_fd <= 0) {
        pa_log_error("Socket initialization failed\n");
        return NULL;
    }

    /* Initialize current jack out config */
    memset(&curr_spdif_jack_config, 0, sizeof(pa_qahw_jack_out_config));

    jdata = pa_xnew0(struct pa_qahw_jack_data, 1);

    spdif_jdata = pa_xnew0(pa_qahw_spdif_jack_data_t, 1);
    spdif_jdata->channel_status = pa_xmalloc0(sizeof(char) * 48);

    jdata->prv_data = spdif_jdata;

    jdata->jack_type = jack_type;
    spdif_jdata->jack_type = jack_type;

    spdif_jdata->fd = sock_event_fd;
    spdif_jdata->jack_in_config = jack_in_config;
    spdif_jdata->active_port_type = PA_QAHW_JACK_TYPE_INVALID;

    pa_hook_init(&(spdif_jdata->event_hook), NULL);
    jdata->event_hook = &(spdif_jdata->event_hook);

    port_name = pa_qahw_util_get_port_name_from_jack_type(jack_type);
    if (!port_name) {
        pa_log_error("Invalid port jack %d\n", jack_type);
        return NULL;
    }

    port_name_underscore = pa_xstrdup(port_name);

    /* replace hyphen with underscore as in dbus doesn't allow hyphen in name */
    port_name_underscore = pa_replace(port_name, "-", "_");

    spdif_jdata->obj_path = pa_sprintf_malloc("%s/%s", QAHW_DBUS_OBJECT_PATH_PREFIX, port_name_underscore);
    spdif_jdata->dbus_protocol = pa_dbus_protocol_get(m->core);
    spdif_jdata->module_handle = module_handle;

    pa_xfree(port_name_underscore);

    /* Expose DBus interface */
    pa_assert_se(pa_dbus_protocol_add_interface(spdif_jdata->dbus_protocol, spdif_jdata->obj_path,
                                     &pa_qahw_jack_spdif_module_interface_info, spdif_jdata) >= 0);

    *hook_slot = pa_hook_connect(&(spdif_jdata->event_hook), PA_HOOK_NORMAL, (pa_hook_cb_t)callback, client_data);

    /* Manually raise event for first time */
    /* Raise jack available event */
    event_data.jack_type = jdata->jack_type;
    event_data.event = PA_QAHW_JACK_AVAILABLE;
    pa_log_info("qahw jack type %d available", jdata->jack_type);
    pa_hook_fire(&(spdif_jdata->event_hook), &event_data);

    /* Raise config update event */
    if (!pa_qahw_spdif_jack_get_config(spdif_jdata->active_port_type, spdif_jdata->jack_in_config->jack_sys_path, &new_port_config)) {
        event_data.event = PA_QAHW_JACK_CONFIG_UPDATE;
        pa_log_info("qahw jack type %d config update", jdata->jack_type);
        event_data.pa_qahw_jack_info = &new_port_config;
        pa_hook_fire(&(spdif_jdata->event_hook), &event_data);
        spdif_jdata->active_port_type = spdif_jdata->jack_type;
    }

    spdif_jdata->io = m->core->mainloop->io_new(m->core->mainloop, sock_event_fd, PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP, jack_io_callback, spdif_jdata);

    return jdata;
}

void pa_qahw_spdif_jack_detection_disable(struct pa_qahw_jack_data *jdata, pa_module *m) {
    pa_qahw_spdif_jack_data_t *spdif_jdata;

    pa_assert(jdata);

    spdif_jdata = (pa_qahw_spdif_jack_data_t *)jdata->prv_data;

    /* Reset current jack out config */
    memset(&curr_spdif_jack_config, 0, sizeof(pa_qahw_jack_out_config));

    pa_assert_se(pa_dbus_protocol_remove_interface(spdif_jdata->dbus_protocol, spdif_jdata->obj_path,
                                                 pa_qahw_jack_spdif_module_interface_info.name) >= 0);

    pa_dbus_protocol_unref(spdif_jdata->dbus_protocol);

    pa_xfree(spdif_jdata->obj_path);

    if (spdif_jdata->io)
        m->core->mainloop->io_free(spdif_jdata->io);

    if (close(spdif_jdata->fd))
        pa_log_error("Close socket failed with error %s\n", strerror(errno));

    if (spdif_jdata->jack_in_config)
        pa_xfree(spdif_jdata->jack_in_config);

    pa_hook_done(&(spdif_jdata->event_hook));

    pa_xfree(spdif_jdata->channel_status);

    pa_xfree(spdif_jdata);

    pa_xfree(jdata);
    jdata = NULL;
}

static void pa_qahw_jack_spdif_set_channel_bit_mask(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_qahw_spdif_jack_data_t *spdif_jdata = (pa_qahw_spdif_jack_data_t *)userdata;
    DBusMessageIter arg_i, array_i;
    char *ch_bit_mask = NULL;
    char **addr_value = &ch_bit_mask;
    int32_t n_elements = 0;
    qahw_param_payload payload;
    int ret = 0;
    const char *port_name = NULL;

    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("%s", __func__);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "SetChBitMask has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "ay")) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Invalid signature for SetChBitMask");
        dbus_error_free(&error);
        return;
    }

    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);

    port_name = pa_qahw_util_get_port_name_from_jack_type(spdif_jdata->active_port_type);
    payload.ch_bit_mask.device = pa_qahw_util_port_to_qahw_device(port_name);
    memscpy(payload.ch_bit_mask.bit_mask,
            sizeof(payload.ch_bit_mask.bit_mask) / sizeof(payload.ch_bit_mask.bit_mask[0]),
            ch_bit_mask, n_elements);

    ret = qahw_set_param_data(spdif_jdata->module_handle, QAHW_PARAM_CHANNEL_BIT_MASK, &payload);

    pa_log_info("%s: QAHW_PARAM_CHANNEL_BIT_MASK set returned %d", __func__, ret);

    pa_dbus_send_empty_reply(conn, msg);
}

static int32_t pa_qahw_jack_spdif_read_ch_status(pa_qahw_spdif_jack_data_t *spdif_jdata) {
    int fd = -1;
    int ret = 0;
    const char *path = spdif_jdata->jack_in_config->jack_sys_path.channel_status;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        pa_log_error("Unable open fd for file %s", path);
        return -EINVAL;
    }

    ret = read(fd, spdif_jdata->channel_status, 48);
    if (ret < 0)
        pa_log_error("File %s Data is empty", path);

    close(fd);
    return ret;
}

static void pa_qahw_jack_spdif_raise_signal(pa_qahw_spdif_jack_data_t *spdif_jdata) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, array_i;

    pa_log_info("%s: enter", __func__);

    /* Raising signal to client on the event of channel status update */
    pa_assert_se(message = dbus_message_new_signal(spdif_jdata->obj_path, pa_qahw_jack_spdif_module_interface_info.name,
                                                           ch_status_update_signal[SIGNAL_CHANNEL_STATUS_UPDATE].name));

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &spdif_jdata->channel_status, 48);
    dbus_message_iter_close_container(&arg_i, &array_i);

    pa_dbus_protocol_send_signal(spdif_jdata->dbus_protocol, message);

    dbus_message_unref(message);
}