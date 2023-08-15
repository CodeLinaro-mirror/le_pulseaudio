/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
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

#include "qal-jack-common.h"
#include "qal-jack-format.h"

#define SOCKET_BUFFER_SIZE 64 * 1024
#define UEVENT_MSG_LEN 4 * 1024
#define EXT_HDMI_DISPLAY_SWITCH_NAME "soc:qcom,msm-ext-disp"

typedef struct {
    int fd;
    pa_io_event *io;
    pa_hook event_hook;
    pa_pal_jack_type_t jack_type;
    pa_pal_jack_event_t jack_plugin_status;
    pa_pal_jack_in_config *jack_in_config;
} pa_pal_hdmi_out_jack_data_t;

static int poll_data_event_init(pa_pal_jack_type_t jack_type) {
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

static void set_default_config(pa_pal_jack_out_config *config) {
    config->preemph_status = 0;
    config->ss.format = PA_SAMPLE_S16LE;
    config->encoding = PA_ENCODING_PCM;
    config->ss.rate = 48000;
    config->ss.channels = 2;
    pa_channel_map_init(&(config->map));
    pa_channel_map_init_auto(&(config->map), 2, PA_CHANNEL_MAP_DEFAULT);
}

static void check_hdmi_out_connection (pa_pal_hdmi_out_jack_data_t *hdmi_out_jdata) {
    const char *path = NULL;
    int hdmi_tx_state = 0;
    pa_pal_jack_event_data_t event_data;
    pa_pal_jack_out_config config;
    int audio_path_value = -1;

    pa_assert(hdmi_out_jdata);
    pa_assert(hdmi_out_jdata->jack_in_config->jack_sys_path.hdmi_tx_state);

    /* Ignore events if audio_path is 1 */
    pa_pal_format_detection_get_value_from_path(hdmi_out_jdata->jack_in_config->jack_sys_path.audio_path, &audio_path_value);
    if (audio_path_value == 1)
        return;

    hdmi_out_jdata->jack_plugin_status = PA_PAL_JACK_UNAVAILABLE;
    path = hdmi_out_jdata->jack_in_config->jack_sys_path.hdmi_tx_state;

    pa_pal_format_detection_get_value_from_path(path, &hdmi_tx_state);

    if (hdmi_tx_state) {
        /* Raise jack available event */
        event_data.jack_type = hdmi_out_jdata->jack_type;
        event_data.event = PA_PAL_JACK_AVAILABLE;
        pa_log_info("qal jack type %d available", hdmi_out_jdata->jack_type);
        pa_hook_fire(&(hdmi_out_jdata->event_hook), &event_data);
        hdmi_out_jdata->jack_plugin_status = PA_PAL_JACK_AVAILABLE;

        /* Set default config */
        set_default_config(&config);

        /* generate jack config update event */
        event_data.pa_pal_jack_info = &config;
        event_data.event = PA_PAL_JACK_CONFIG_UPDATE;
        pa_hook_fire(&(hdmi_out_jdata->event_hook), &event_data);
    }
}

static void jack_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t io_events, void *userdata) {
    pa_pal_hdmi_out_jack_data_t *hdmi_out_jdata = userdata;

    char buffer[UEVENT_MSG_LEN+2];
    int count, j = 0;
    pa_pal_jack_event_data_t event_data;
    int hdmi_out_flag = 0;
    char *switch_state = NULL;
    char *dp_switch_state = NULL;
    char *switch_name = NULL;
    pa_pal_jack_out_config config;

    pa_assert(hdmi_out_jdata);
    event_data.jack_type = hdmi_out_jdata->jack_type;

    count = recv(hdmi_out_jdata->fd, buffer, (UEVENT_MSG_LEN), 0 );

    if (count > 0) {
        buffer[count] = '\0';
        buffer[count+1] = '\0';
        j = 0;
        hdmi_out_flag = 0;

        while (j < count) {
            if (pa_strneq(&buffer[j], "NAME=", strlen("NAME="))) {
                switch_name = &buffer[j + strlen("NAME=")];
                j += strlen("NAME=");
                continue;
            } else if (pa_strneq(&buffer[j], "HDMI=", strlen("HDMI="))) {
                switch_state = &buffer[j + strlen("HDMI=")];
                j += strlen("HDMI=");
                continue;
            } else if (pa_strneq(&buffer[j], "DP=", strlen("DP="))) {
                dp_switch_state = &buffer[j + strlen("DP=")];
                j += strlen("DP=");
                continue;
            }

            j++;
        }

        if ((switch_name != NULL) && ((switch_state != NULL) || (dp_switch_state != NULL))) {
            if (pa_strneq(switch_name, EXT_HDMI_DISPLAY_SWITCH_NAME, strlen(EXT_HDMI_DISPLAY_SWITCH_NAME))) {
                if (( switch_state && atoi(switch_state) == 1) || ( dp_switch_state && atoi(dp_switch_state) == 1))
                    hdmi_out_flag = 1;
                else if ((switch_state && atoi(switch_state) == 0) && ( switch_state && atoi(dp_switch_state) == 0))
                    hdmi_out_flag = -1;
            }
        }

        if ((hdmi_out_flag == 1) && (hdmi_out_jdata->jack_plugin_status != PA_PAL_JACK_AVAILABLE)) {
            event_data.jack_type = hdmi_out_jdata->jack_type;
            event_data.event = PA_PAL_JACK_AVAILABLE;
            pa_log_info("qal jack type %d available", hdmi_out_jdata->jack_type);
            pa_hook_fire(&(hdmi_out_jdata->event_hook), &event_data);
            hdmi_out_jdata->jack_plugin_status = PA_PAL_JACK_AVAILABLE;

            /* Set default config */
            set_default_config(&config);

            /* generate jack config update event */
            event_data.pa_pal_jack_info = &config;
            event_data.event = PA_PAL_JACK_CONFIG_UPDATE;
            pa_hook_fire(&(hdmi_out_jdata->event_hook), &event_data);
        } else if ((hdmi_out_flag == -1) && (hdmi_out_jdata->jack_plugin_status != PA_PAL_JACK_UNAVAILABLE)) {
            /* Raise jack unavailable event */
            event_data.jack_type = hdmi_out_jdata->jack_type;
            event_data.event = PA_PAL_JACK_UNAVAILABLE;
            pa_log_info("qal jack type %d unavailable", hdmi_out_jdata->jack_type);
            pa_hook_fire(&(hdmi_out_jdata->event_hook), &event_data);
            hdmi_out_jdata->jack_plugin_status = PA_PAL_JACK_UNAVAILABLE;
        }
    }
}

struct pa_pal_jack_data* pa_pal_hdmi_out_jack_detection_enable(pa_pal_jack_type_t jack_type, pa_module *m,
                                               pa_hook_slot **hook_slot, pa_pal_jack_callback_t callback,
                                                pa_pal_jack_in_config *jack_in_config, void *client_data) {
    struct pa_pal_jack_data *jdata = NULL;
    int sock_event_fd = -1;
    pa_pal_hdmi_out_jack_data_t *hdmi_out_jdata = NULL;

    sock_event_fd = poll_data_event_init(jack_type);
    if (sock_event_fd <= 0) {
        pa_log_error("Socket initialization failed\n");
        return NULL;
    }

    jdata = pa_xnew0(struct pa_pal_jack_data, 1);

    hdmi_out_jdata = pa_xnew0(pa_pal_hdmi_out_jack_data_t, 1);
    jdata->prv_data = hdmi_out_jdata;

    jdata->jack_type = jack_type;
    hdmi_out_jdata->jack_type = jack_type;

    hdmi_out_jdata->fd = sock_event_fd;
    hdmi_out_jdata->jack_in_config = jack_in_config;

    pa_hook_init(&(hdmi_out_jdata->event_hook), NULL);
    jdata->event_hook = &(hdmi_out_jdata->event_hook);

    *hook_slot = pa_hook_connect(&(hdmi_out_jdata->event_hook), PA_HOOK_NORMAL, (pa_hook_cb_t)callback, client_data);

    /* Check if jack is already connected */
    check_hdmi_out_connection(hdmi_out_jdata);

    hdmi_out_jdata->io = m->core->mainloop->io_new(m->core->mainloop, sock_event_fd, PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP, jack_io_callback, hdmi_out_jdata);

    return jdata;
}

void pa_pal_hdmi_out_jack_detection_disable(struct pa_pal_jack_data *jdata, pa_module *m) {
    pa_pal_hdmi_out_jack_data_t *hdmi_out_jdata;

    pa_assert(jdata);

    hdmi_out_jdata = (pa_pal_hdmi_out_jack_data_t *)jdata->prv_data;

    if (hdmi_out_jdata->io)
        m->core->mainloop->io_free(hdmi_out_jdata->io);

    if (close(hdmi_out_jdata->fd))
        pa_log_error("Close socket failed with error %s\n", strerror(errno));

    if (hdmi_out_jdata->jack_in_config)
        pa_xfree(hdmi_out_jdata->jack_in_config);

    pa_hook_done(&(hdmi_out_jdata->event_hook));

    pa_xfree(hdmi_out_jdata);

    pa_xfree(jdata);
    jdata = NULL;
}
