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

#define HDMI_JACK_SYS_PATH  "/sys/devices/virtual/switch/hpd_state/state"

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

static void check_hdmi_connection(struct pa_qahw_jack_data *jdata, pa_qahw_jack_event_data_t event_data) {
    const char *path = HDMI_JACK_SYS_PATH;
    int fd = -1;
    char buf[16];
    int value;
    int ret;

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
        pa_log_info("qahw jack type %d available", jdata->jack_type);
        jdata->callback(PA_QAHW_JACK_AVAILABLE, &event_data, jdata->prv_data);
    }
}

static void jack_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t io_events, void *userdata) {
    struct pa_qahw_jack_data *jdata = userdata;
    char buffer[64 * 1024];
    int count, j;
    char *dev_path = NULL;
    char *switch_state = NULL;
    char *switch_name = NULL;
    pa_qahw_jack_event_data_t event_data;

    pa_assert(jdata);
    event_data.jack_type = jdata->jack_type;

    count = recv(jdata->fd, buffer, (64 * 1024), 0 );

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

        if ((dev_path != NULL) && pa_streq(switch_name, "hpd_state") && (atoi(switch_state) == 1)) {
            pa_log_info("qahw jack type %d available", jdata->jack_type);
            jdata->callback(PA_QAHW_JACK_AVAILABLE, &event_data, jdata->prv_data);
        }
        else if ((dev_path != NULL) && pa_streq(switch_name, "hpd_state") && (atoi(switch_state) == 0)) {
            pa_log_info("qahw jack type %d not available", jdata->jack_type);
            jdata->callback(PA_QAHW_JACK_UNAVAILABLE, &event_data, jdata->prv_data);
        }
    }
}

struct pa_qahw_jack_data* pa_qahw_hdmi_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m,
                                                        pa_qahw_jack_callback_t callback, void *prv_data) {
    struct pa_qahw_jack_data *jdata = NULL;
    pa_qahw_jack_event_data_t event_data;
    int sock_event_fd = -1;

    sock_event_fd = poll_data_event_init();
    if (sock_event_fd <= 0) {
        pa_log_error("Socket initialization failed\n");
        return NULL;
    }

    jdata = pa_xnew0(struct pa_qahw_jack_data, 1);
    jdata->fd = sock_event_fd;
    jdata->jack_type = jack_type;
    jdata->callback = callback;
    jdata->prv_data = prv_data;
    jdata->module = m;

    event_data.jack_type = jdata->jack_type;

    /* Check if HDMI is already connected */
    check_hdmi_connection(jdata, event_data);

    jdata->io = m->core->mainloop->io_new(m->core->mainloop, sock_event_fd, PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP, jack_io_callback, jdata);

    return jdata;
}

void pa_qahw_hdmi_jack_detection_disable(struct pa_qahw_jack_data *jdata) {
    pa_assert(jdata);

    if(jdata->io)
        jdata->module->core->mainloop->io_free(jdata->io);

    if (close(jdata->fd))
        pa_log_error("Close socket failed with error %s\n", strerror(errno));

    pa_xfree(jdata);
    jdata = NULL;
}
