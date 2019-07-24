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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>

#include "qahw-jack-common.h"
#include "qahw-jack-format.h"

#define SOCKET_BUFFER_SIZE 64 * 1024
#define SPDIF_UEVENT_NAME "PRI_SPDIF_TX=MEDIA_CONFIG_CHANGE"

typedef struct {
    int fd;
    pa_io_event *io;
    pa_hook event_hook;
    pa_qahw_jack_type_t jack_type;
    pa_qahw_jack_in_config *jack_in_config;
    pa_qahw_jack_type_t active_port_type;
} pa_qahw_spdif_jack_data_t;

pa_qahw_jack_out_config curr_spdif_jack_config;

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
                                                pa_qahw_jack_in_config *jack_in_config, void *client_data) {
    struct pa_qahw_jack_data *jdata = NULL;
    int sock_event_fd = -1;
    pa_qahw_spdif_jack_data_t *spdif_jdata = NULL;
    static pa_qahw_jack_out_config new_port_config;
    pa_qahw_jack_event_data_t event_data;

    sock_event_fd = poll_data_event_init(jack_type);
    if (sock_event_fd <= 0) {
        pa_log_error("Socket initialization failed\n");
        return NULL;
    }

    /* Initialize current jack out config */
    memset(&curr_spdif_jack_config, 0, sizeof(pa_qahw_jack_out_config));

    jdata = pa_xnew0(struct pa_qahw_jack_data, 1);

    spdif_jdata = pa_xnew0(pa_qahw_spdif_jack_data_t, 1);
    jdata->prv_data = spdif_jdata;

    jdata->jack_type = jack_type;
    spdif_jdata->jack_type = jack_type;

    spdif_jdata->fd = sock_event_fd;
    spdif_jdata->jack_in_config = jack_in_config;
    spdif_jdata->active_port_type = PA_QAHW_JACK_TYPE_INVALID;

    pa_hook_init(&(spdif_jdata->event_hook), NULL);
    jdata->event_hook = &(spdif_jdata->event_hook);

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

    if (spdif_jdata->io)
        m->core->mainloop->io_free(spdif_jdata->io);

    if (close(spdif_jdata->fd))
        pa_log_error("Close socket failed with error %s\n", strerror(errno));

    if (spdif_jdata->jack_in_config)
        pa_xfree(spdif_jdata->jack_in_config);

    pa_hook_done(&(spdif_jdata->event_hook));

    pa_xfree(spdif_jdata);

    pa_xfree(jdata);
    jdata = NULL;
}
