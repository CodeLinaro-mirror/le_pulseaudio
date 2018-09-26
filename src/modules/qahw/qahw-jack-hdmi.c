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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>

#include "qahw-jack-common.h"
#include "qahw-jack-format.h"

#define HDMI_JACK_SYS_PATH  "/sys/devices/virtual/switch/hpd_state/state"

typedef struct {
    int fd;
    pa_io_event *io;
    pa_hook event_hook;
    pa_qahw_jack_event_t jack_status;
    pa_qahw_jack_type_t jack_type;
} pa_qahw_hdmi_jack_data_t;

pa_qahw_jack_config_t curr_hdmi_jack_config;

static int poll_data_event_init(void) {
    struct sockaddr_nl sock_addr;
    int sz = (64 * 1024);
    int soc = -1;

    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.nl_family = AF_NETLINK;
    sock_addr.nl_pid = getpid() + 1;
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

static void check_hdmi_connection(pa_qahw_hdmi_jack_data_t *hdmi_jdata) {
    const char *path = HDMI_JACK_SYS_PATH;
    pa_qahw_jack_event_data_t event_data;

    int fd = -1;
    char buf[16];
    int value;
    int ret;

    event_data.jack_type = hdmi_jdata->jack_type;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        pa_log_error("Unable open fd for file %s\n", path);
        return;
    }

    ret = read(fd, buf, 15);
    if (ret < 0) {
        pa_log_error("File %s Data is empty\n", path);
        close(fd);
        return;
    }

    buf[ret] = '\0';
    value = atoi(buf);
    close(fd);

    if (value == 1) {
        pa_log_info("qahw jack type %d available", hdmi_jdata->jack_type);
        event_data.event = PA_QAHW_JACK_AVAILABLE;
        hdmi_jdata->jack_status = PA_QAHW_JACK_AVAILABLE;
        pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
    }
}

static void jack_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t io_events, void *userdata) {
    pa_qahw_hdmi_jack_data_t *hdmi_jdata = userdata;

    char buffer[64 * 1024];
    int count, j;
    char *dev_path = NULL;
    char *switch_state = NULL;
    char *switch_name = NULL;
    pa_qahw_jack_event_data_t event_data;
    static pa_qahw_jack_config_t new_port_config;

    pa_assert(hdmi_jdata);
    event_data.jack_type = hdmi_jdata->jack_type;

    count = recv(hdmi_jdata->fd, buffer, (64 * 1024), 0 );

    if (count > 0) {
        buffer[count] = '\0';
        j = 0;

        while(j < count) {
            if (pa_strneq(&buffer[j], "DEVPATH=", 8)) {
                dev_path = &buffer[j + 8];
                j += 8;
                continue;
            } else if (pa_strneq(&buffer[j], "SWITCH_NAME=", 12)) {
                switch_name = &buffer[j + 12];
                j += 12;
                continue;
            } else if (pa_strneq(&buffer[j], "SWITCH_STATE=", 13)) {
                switch_state = &buffer[j + 13];
                j += 13;
                continue;
            }
            j++;
        }

        if ((dev_path != NULL) && (switch_name != NULL) && (switch_state != NULL)) {
            if (pa_streq(switch_name, "hpd_state") && (atoi(switch_state) == 1)) {
                pa_log_info("qahw jack type %d available", hdmi_jdata->jack_type);
                event_data.event = PA_QAHW_JACK_AVAILABLE;
                hdmi_jdata->jack_status = PA_QAHW_JACK_AVAILABLE;
                pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
            } else if (pa_streq(switch_name, "hpd_state") && (atoi(switch_state) == 0)) {
                pa_log_info("qahw jack type %d not available", hdmi_jdata->jack_type);
                event_data.event = PA_QAHW_JACK_UNAVAILABLE;
                hdmi_jdata->jack_status = PA_QAHW_JACK_UNAVAILABLE;
                pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
            } else if ((pa_streq(switch_name, "audio_format") || pa_streq(switch_name, "channels") ||
                        pa_streq(switch_name, "sample_rate"))) {
                if ((hdmi_jdata->jack_status == PA_QAHW_JACK_AVAILABLE) &&
                          (!pa_qahw_hdmi_jack_get_config(&new_port_config))) {
                    if (memcmp(&new_port_config, &curr_hdmi_jack_config, sizeof(pa_qahw_jack_config_t))) {
                        memcpy(&curr_hdmi_jack_config, &new_port_config, sizeof(pa_qahw_jack_config_t));
                        event_data.pa_qahw_jack_info = &new_port_config;
                        pa_log_info("qahw jack type %d config update", hdmi_jdata->jack_type);
                        event_data.event = PA_QAHW_JACK_CONFIG_UPDATE;
                        pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
                    }
                }
            }
        }
    }
}

struct pa_qahw_jack_data* pa_qahw_hdmi_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m,
                             pa_hook_slot **hook_slot, pa_qahw_jack_callback_t callback, void *client_data) {
    struct pa_qahw_jack_data *jdata = NULL;
    int sock_event_fd = -1;
    pa_qahw_hdmi_jack_data_t *hdmi_jdata = NULL;

    sock_event_fd = poll_data_event_init();
    if (sock_event_fd <= 0) {
        pa_log_error("Socket initialization failed\n");
        return NULL;
    }

    jdata = pa_xnew0(struct pa_qahw_jack_data, 1);

    hdmi_jdata = pa_xnew0(pa_qahw_hdmi_jack_data_t, 1);
    jdata->prv_data = hdmi_jdata;

    hdmi_jdata->jack_type = jack_type;
    jdata->jack_type = jack_type;

    hdmi_jdata->fd = sock_event_fd;

    pa_hook_init(&(hdmi_jdata->event_hook), NULL);
    jdata->event_hook = &(hdmi_jdata->event_hook);

    *hook_slot = pa_hook_connect(&(hdmi_jdata->event_hook), PA_HOOK_NORMAL, (pa_hook_cb_t)callback, client_data);

    /* Check if HDMI is already connected */
    check_hdmi_connection(hdmi_jdata);

    hdmi_jdata->io = m->core->mainloop->io_new(m->core->mainloop, sock_event_fd, PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP, jack_io_callback, hdmi_jdata);


    return jdata;
}

void pa_qahw_hdmi_jack_detection_disable(struct pa_qahw_jack_data *jdata, pa_module *m) {
    pa_qahw_hdmi_jack_data_t *hdmi_jdata;
    pa_assert(jdata);

    hdmi_jdata = (pa_qahw_hdmi_jack_data_t *)jdata->prv_data;

    if(hdmi_jdata->io)
        m->core->mainloop->io_free(hdmi_jdata->io);

    if (close(hdmi_jdata->fd))
        pa_log_error("Close socket failed with error %s\n", strerror(errno));

    pa_hook_done(&(hdmi_jdata->event_hook));

    pa_xfree(hdmi_jdata);

    pa_xfree(jdata);
    jdata = NULL;
}
