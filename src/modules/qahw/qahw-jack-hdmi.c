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

#include "qahw-jack-common.h"
#include "qahw-jack-format.h"

#define SOCKET_BUFFER_SIZE 64 * 1024
#define EP92EVT_LINK_ON0_CONNECTED_UEVENT "EP92EVT_LINK_ON0=CONNECTED"
#define EP92EVT_LINK_ON0_DISCONNECTED_UEVENT "EP92EVT_LINK_ON0=DISCONNECTED"
#define EP92EVT_AUDIO_MEDIA_CONFIG_CHANGE_UEVENT "EP92EVT_AUDIO=MEDIA_CONFIG_CHANGE"
#define SEC_SPDIF_TX_MEDIA_CONFIG_CHANGE_UEVENT "SEC_SPDIF_TX=MEDIA_CONFIG_CHANGE"
#define EP92EVT_ARC_ENABLE_UEVENT "EP92EVT_ARC_EN=ON"
#define EP92EVT_ARC_DISABLE_UEVENT "EP92EVT_ARC_EN=OFF"

typedef struct {
    int fd;
    pa_io_event *io;
    pa_hook event_hook;
    pa_qahw_jack_event_t port_status;
    pa_qahw_jack_event_t jack_plugin_status;
    pa_qahw_jack_type_t port_type;
    pa_qahw_jack_in_config *jack_in_config;
    pa_hashmap *linked_ports;
    pa_qahw_jack_type_t active_port_type;
    pa_qahw_jack_type_t active_valid_port_type;
} pa_qahw_hdmi_jack_data_t;

typedef struct {
    pa_qahw_jack_event_t port_status;
    pa_qahw_jack_type_t port_type;
} pa_qahw_hdmi_jack_linked_port_data_t;

pa_qahw_jack_out_config curr_hdmi_jack_config;

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

static bool is_hdmi_config_update_event_valid(pa_qahw_jack_out_config new_port_config, pa_qahw_hdmi_jack_data_t *hdmi_jdata) {
    bool status = false;
    bool config_change = false;
    const char *path = NULL;
    int arc_audio_state_value = -1;
    int audio_state_value = -1;

    /* Always return true if there is device switch */
    if (new_port_config.active_jack != curr_hdmi_jack_config.active_jack)
        return true;

    /* Always return true if arc audio state is EOS */
    if (new_port_config.active_jack == PA_QAHW_JACK_TYPE_HDMI_ARC) {
        path = hdmi_jdata->jack_in_config->jack_sys_path.arc_audio_state;
        pa_qahw_format_detection_get_value_from_path(path, &arc_audio_state_value);
        if (arc_audio_state_value == 2) {
            pa_log_info("%s: SPDIF interface audio state valid", __func__);
            return true;
        }
    }

    /* Always return true if preemph value changes */
    if (new_port_config.preemph_status != curr_hdmi_jack_config.preemph_status) {
        pa_log_info("%s: Change in preemphasis value", __func__);
        curr_hdmi_jack_config.preemph_status = new_port_config.preemph_status;
        return true;
    }

    /* Check whether there is any difference in detected configs */
    if ((new_port_config.encoding != curr_hdmi_jack_config.encoding) ||
         memcmp(&(new_port_config.ss), &(curr_hdmi_jack_config.ss), sizeof(pa_sample_spec)) ||
         memcmp(&(new_port_config.map), &(curr_hdmi_jack_config.map), sizeof(pa_channel_map)))
        config_change = true;

    /* Check whether HDMI audio state is valid */
    path = hdmi_jdata->jack_in_config->jack_sys_path.audio_state;
    if (pa_qahw_format_detection_get_value_from_path(path, &audio_state_value) && (audio_state_value != -1))
        status = (audio_state_value == 0) ? false : true;

    if ((status == true) && (config_change == false))
        status = false;

    return status;
}

static bool is_hdmi_no_stream_event_valid(pa_qahw_hdmi_jack_data_t *hdmi_jdata) {
    bool status = false;
    const char *path = NULL;
    int audio_state_value = -1;
    int arc_enable_value = -1;

    pa_assert(hdmi_jdata);

    if (hdmi_jdata->active_port_type == PA_QAHW_JACK_TYPE_INVALID)
        return status;

    path = hdmi_jdata->jack_in_config->jack_sys_path.audio_state;
    pa_qahw_format_detection_get_value_from_path(hdmi_jdata->jack_in_config->jack_sys_path.arc_enable, &arc_enable_value);

    if (arc_enable_value == 1) {
        pa_log_info("%s: no need to check audio_state for SPDIF interface", __func__);
    } else {
        if (pa_qahw_format_detection_get_value_from_path(path, &audio_state_value) && (audio_state_value != -1))
            status = (audio_state_value == 0) ? true : false;
    }

    if (status) {
        hdmi_jdata->active_valid_port_type = PA_QAHW_JACK_TYPE_INVALID;
        memset(&curr_hdmi_jack_config, 0, sizeof(pa_qahw_jack_out_config));
        curr_hdmi_jack_config.active_jack = PA_QAHW_JACK_TYPE_INVALID;
    }

    return status;
}

static void pa_qahw_hdmi_jack_log_event(pa_qahw_jack_event_t event, pa_qahw_jack_type_t port_type) {
    if (event == PA_QAHW_JACK_UNAVAILABLE)
        pa_log_info("qahw jack type %d not available", port_type);
    else if (event == PA_QAHW_JACK_AVAILABLE)
        pa_log_info("qahw jack type %d available", port_type);
    else if (event == PA_QAHW_JACK_CONFIG_UPDATE)
        pa_log_info("qahw jack type %d config update", port_type);
    else if (event == PA_QAHW_JACK_NO_VALID_STREAM)
        pa_log_info("qahw jack type %d no valid stream", port_type);
}

static void pa_qahw_hdmi_jack_raise_event(pa_qahw_jack_type_t jack_port_type, pa_qahw_jack_event_t event,
                              pa_qahw_jack_out_config *port_config, pa_qahw_hdmi_jack_data_t *hdmi_jdata,
                                                                                   bool all_linked_ports) {
    pa_qahw_jack_event_data_t event_data;
    void *state;
    pa_qahw_hdmi_jack_linked_port_data_t *linked_port = NULL;
    const char *port_name = NULL;

    pa_assert(hdmi_jdata);

    if (jack_port_type == PA_QAHW_JACK_TYPE_INVALID)
    pa_log_error("%s: jack type %d, ignoring request", __func__, jack_port_type);

    if ((event == PA_QAHW_JACK_CONFIG_UPDATE) && port_config) {
        /* Raise PA_QAHW_JACK_CONFIG_UPDATE irrespective of port_type */
        event_data.event = event;
        event_data.jack_type = jack_port_type;
        event_data.pa_qahw_jack_info = port_config;
        pa_qahw_hdmi_jack_log_event(event, jack_port_type);
        pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
    } else if (event == PA_QAHW_JACK_NO_VALID_STREAM) {
        /* Raise PA_QAHW_JACK_NO_VALID_STREAM irrespective of port_type */
        event_data.event = event;
        event_data.jack_type = jack_port_type;
        pa_qahw_hdmi_jack_log_event(event, jack_port_type);
        pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
    } else {
        if (all_linked_ports) {
            /* If port type is secondary and the event is applicable to all linked ports */
            PA_HASHMAP_FOREACH(linked_port, hdmi_jdata->linked_ports, state) {
                if (linked_port->port_status != event) {
                    event_data.event = event;
                    event_data.jack_type = linked_port->port_type;
                    pa_qahw_hdmi_jack_log_event(event, linked_port->port_type);
                    pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
                    linked_port->port_status = event;
                }
            }
        } else if (jack_port_type == hdmi_jdata->port_type) {
            /* This implies that the port is primary port */
            event_data.event = event;
            event_data.jack_type = jack_port_type;
            pa_qahw_hdmi_jack_log_event(event, hdmi_jdata->port_type);
            pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
            hdmi_jdata->port_status = event;
        } else {
            /* If port type is secondary and event is applicable to a single linked port */
            /* Get the linked port */
            port_name = pa_qahw_util_get_port_name_from_jack_type(jack_port_type);
            linked_port = pa_hashmap_get(hdmi_jdata->linked_ports, port_name);

            /* Raise the event */
            if (linked_port) {
                if (linked_port->port_status != event) {
                    event_data.event = event;
                    event_data.jack_type = linked_port->port_type;
                    pa_qahw_hdmi_jack_log_event(event, linked_port->port_type);
                    pa_hook_fire(&(hdmi_jdata->event_hook), &event_data);
                    linked_port->port_status = event;
                }
            }
        }
    }
}

static void check_hdmi_arc_state(pa_qahw_hdmi_jack_data_t *hdmi_jdata) {
    const char *path = NULL;
    static pa_qahw_jack_out_config new_port_config;

    int arc_state_value;

    path = hdmi_jdata->jack_in_config->jack_sys_path.arc_enable;
    pa_qahw_format_detection_get_value_from_path(path, &arc_state_value);

    if (arc_state_value == 1) {
        /* Raise PA_QAHW_JACK_AVAILABLE event for HDMI-ARC */
        pa_qahw_hdmi_jack_raise_event(PA_QAHW_JACK_TYPE_HDMI_ARC, PA_QAHW_JACK_AVAILABLE, NULL, hdmi_jdata, false);

        if (!pa_qahw_hdmi_in_jack_get_config(hdmi_jdata->active_valid_port_type, hdmi_jdata->jack_in_config->jack_sys_path, &new_port_config)) {
            if (new_port_config.active_jack == PA_QAHW_JACK_TYPE_HDMI_ARC) {
                /* Raise PA_QAHW_JACK_CONFIG_UPDATE for HDMI-ARC */
                memcpy(&curr_hdmi_jack_config, &new_port_config, sizeof(pa_qahw_jack_out_config));
                pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_CONFIG_UPDATE, &new_port_config, hdmi_jdata, false);
                hdmi_jdata->active_port_type = PA_QAHW_JACK_TYPE_HDMI_ARC;
                hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;
            }
        }
    }
}

static void check_hdmi_connection(pa_qahw_hdmi_jack_data_t *hdmi_jdata) {
    const char *path = NULL;
    pa_qahw_jack_out_config new_port_config;

    int hdmi_plugin_value;

    pa_assert(hdmi_jdata);
    pa_assert(hdmi_jdata->jack_in_config->jack_sys_path.linkon_0);

    path = hdmi_jdata->jack_in_config->jack_sys_path.linkon_0;

    pa_qahw_format_detection_get_value_from_path(path, &hdmi_plugin_value);

    if (hdmi_plugin_value == 1) {
        /* Raise PA_QAHW_JACK_AVAILABLE event for primary port and PA_QAHW_JACK_UNAVAILABLE for linked ports*/
        hdmi_jdata->jack_plugin_status = PA_QAHW_JACK_AVAILABLE;
        pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_AVAILABLE, NULL, hdmi_jdata, false);
        pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, true);
        hdmi_jdata->active_port_type = hdmi_jdata->port_type;
        hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;

        /* Check current hdmi jack config */
        if (!pa_qahw_hdmi_in_jack_get_config(hdmi_jdata->active_valid_port_type, hdmi_jdata->jack_in_config->jack_sys_path, &new_port_config)) {
            if ((new_port_config.active_jack != PA_QAHW_JACK_TYPE_INVALID) ||
                (new_port_config.preemph_status != curr_hdmi_jack_config.preemph_status)) {
                memcpy(&curr_hdmi_jack_config, &new_port_config, sizeof(pa_qahw_jack_out_config));

                if (new_port_config.active_jack != hdmi_jdata->port_type) {
                    if (hdmi_jdata->jack_in_config->linked_ports) {
                        /* Raise PA_QAHW_JACK_UNAVAILABLE event for primary port */
                        pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, false);

                        /* Raise PA_QAHW_JACK_AVAILABLE event for active linked port and update current active port */
                        pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_AVAILABLE, NULL, hdmi_jdata, false);
                        hdmi_jdata->active_port_type = new_port_config.active_jack;
                        hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;
                    }
                    /* Check whether the audio_state is invalid *
                     * If true raise PA_QAHW_JACK_NO_VALID_STREAM for active port *
                     * else raise PA_QAHW_JACK_CONFIG_UPDATE event for active port */
                    if (is_hdmi_no_stream_event_valid(hdmi_jdata))
                        pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_NO_VALID_STREAM, NULL, hdmi_jdata, false);
                    else
                        pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_CONFIG_UPDATE, &new_port_config, hdmi_jdata, false);
                } else {
                    /* Check whether the audio_state is invalid *
                     * If true raise PA_QAHW_JACK_NO_VALID_STREAM for active port *
                     * else raise PA_QAHW_JACK_CONFIG_UPDATE event for active port */
                    if (is_hdmi_no_stream_event_valid(hdmi_jdata))
                        pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_NO_VALID_STREAM, NULL, hdmi_jdata, false);
                    else
                        pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_CONFIG_UPDATE, &new_port_config, hdmi_jdata, false);
                }
            }
        }
    } else if ((hdmi_jdata->linked_ports) || (hdmi_jdata->port_type == PA_QAHW_JACK_TYPE_HDMI_ARC)) {
        /* If linked ports set means HDMI ARC is linked to HDMI IN */
        /* Check arc state */
        check_hdmi_arc_state(hdmi_jdata);
    }
}

static void jack_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t io_events, void *userdata) {
    pa_qahw_hdmi_jack_data_t *hdmi_jdata = userdata;

    char buffer[SOCKET_BUFFER_SIZE];
    int count, j;
    char *dev_path = NULL;
    char *switch_state = NULL;
    char *switch_name = NULL;
    static pa_qahw_jack_out_config new_port_config;
    bool audio_state_changed;
    int arc_enable_state = 0; /* -1: arc disable, 0: invalid, 1: arc enable*/
    int hdmi_in_flag; /* -1: unplugged state, 0: default value, 1: plugged state*/
    bool sec_spdif_event = false;

    pa_assert(hdmi_jdata);

    count = recv(hdmi_jdata->fd, buffer, SOCKET_BUFFER_SIZE, 0 );

    if (count > 0) {
        buffer[count] = '\0';
        j = 0;
        audio_state_changed = false;
        hdmi_in_flag = 0;

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
            } else if (pa_strneq(&buffer[j], EP92EVT_LINK_ON0_CONNECTED_UEVENT, strlen(EP92EVT_LINK_ON0_CONNECTED_UEVENT))) {
                hdmi_in_flag = 1;
                j += strlen(EP92EVT_LINK_ON0_CONNECTED_UEVENT);
                continue;
            } else if (pa_strneq(&buffer[j], EP92EVT_LINK_ON0_DISCONNECTED_UEVENT, strlen(EP92EVT_LINK_ON0_DISCONNECTED_UEVENT))) {
                hdmi_in_flag = -1;
                j += strlen(EP92EVT_LINK_ON0_DISCONNECTED_UEVENT);
                continue;
            } else if (pa_strneq(&buffer[j], EP92EVT_AUDIO_MEDIA_CONFIG_CHANGE_UEVENT, strlen(EP92EVT_AUDIO_MEDIA_CONFIG_CHANGE_UEVENT))) {
                audio_state_changed = true;
                j += strlen(EP92EVT_AUDIO_MEDIA_CONFIG_CHANGE_UEVENT);
                continue;
            }  else if (pa_strneq(&buffer[j], SEC_SPDIF_TX_MEDIA_CONFIG_CHANGE_UEVENT, strlen(SEC_SPDIF_TX_MEDIA_CONFIG_CHANGE_UEVENT))) {
                audio_state_changed = true;
                sec_spdif_event = true;
                j += strlen(SEC_SPDIF_TX_MEDIA_CONFIG_CHANGE_UEVENT);
                continue;
            } else if (pa_strneq(&buffer[j], EP92EVT_ARC_ENABLE_UEVENT, strlen(EP92EVT_ARC_ENABLE_UEVENT))) {
                audio_state_changed = true;
                arc_enable_state = 1;
                j += strlen(EP92EVT_ARC_ENABLE_UEVENT);
                continue;
            } else if (pa_strneq(&buffer[j], EP92EVT_ARC_DISABLE_UEVENT, strlen(EP92EVT_ARC_DISABLE_UEVENT))) {
                audio_state_changed = true;
                arc_enable_state = -1;
                j += strlen(EP92EVT_ARC_DISABLE_UEVENT);
                continue;
            }
            j++;
        }

        if ((dev_path != NULL) && (switch_name != NULL) && (switch_state != NULL)) {
            if ((pa_streq(switch_name, "hpd_state") && (atoi(switch_state) == 1)))
                hdmi_in_flag = 1;
            else if ((pa_streq(switch_name, "hpd_state") && (atoi(switch_state) == 0)))
                hdmi_in_flag = -1;
            else if ((pa_streq(switch_name, "audio_format") || pa_streq(switch_name, "channels") ||
                                                             pa_streq(switch_name, "sample_rate")))
                audio_state_changed = true;
        }

        if ((hdmi_in_flag == 1) && (hdmi_jdata->jack_plugin_status != PA_QAHW_JACK_AVAILABLE)) {
            /* Raise PA_QAHW_JACK_AVAILABLE event for primary port and PA_QAHW_JACK_UNAVAILABLE for linked ports*/
            hdmi_jdata->jack_plugin_status = PA_QAHW_JACK_AVAILABLE;
            pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_AVAILABLE, NULL, hdmi_jdata, false);
            pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, true);
            hdmi_jdata->active_port_type = hdmi_jdata->port_type;
            hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;

            /* Check current hdmi jack config */
            if (!pa_qahw_hdmi_in_jack_get_config(hdmi_jdata->active_valid_port_type, hdmi_jdata->jack_in_config->jack_sys_path, &new_port_config)) {
                if ((new_port_config.active_jack != PA_QAHW_JACK_TYPE_INVALID) ||
                    (new_port_config.preemph_status != curr_hdmi_jack_config.preemph_status)) {
                    memcpy(&curr_hdmi_jack_config, &new_port_config, sizeof(pa_qahw_jack_out_config));

                    if (new_port_config.active_jack != hdmi_jdata->port_type) {
                        if (hdmi_jdata->jack_in_config->linked_ports) {
                            /* Raise PA_QAHW_JACK_UNAVAILABLE event for primary port */
                            pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, false);

                            /* Raise PA_QAHW_JACK_AVAILABLE event for active linked port and update current active port */
                            pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_AVAILABLE, NULL, hdmi_jdata, false);
                            hdmi_jdata->active_port_type = new_port_config.active_jack;
                            hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;
                        }

                        /* Check whether the audio_state is invalid *
                         * If true raise PA_QAHW_JACK_NO_VALID_STREAM for active port *
                         * else raise PA_QAHW_JACK_CONFIG_UPDATE event for active port */
                        if (is_hdmi_no_stream_event_valid(hdmi_jdata))
                            pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_NO_VALID_STREAM, NULL, hdmi_jdata, false);
                        else
                            pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_CONFIG_UPDATE, &new_port_config, hdmi_jdata, false);
                    } else {
                        /* Check whether the audio_state is invalid *
                         * If true raise PA_QAHW_JACK_NO_VALID_STREAM for active port *
                         * else raise PA_QAHW_JACK_CONFIG_UPDATE event for active port */
                        if (is_hdmi_no_stream_event_valid(hdmi_jdata))
                            pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_NO_VALID_STREAM, NULL, hdmi_jdata, false);
                        else
                            pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_CONFIG_UPDATE, &new_port_config, hdmi_jdata, false);
                    }
                }
            }
        }  else if ((hdmi_in_flag == -1) && (hdmi_jdata->jack_plugin_status != PA_QAHW_JACK_UNAVAILABLE)) {
            /* Raise PA_QAHW_JACK_UNAVAILABLE event */
            hdmi_jdata->jack_plugin_status = PA_QAHW_JACK_UNAVAILABLE;
            pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, false);

            /* Raise PA_QAHW_JACK_UNAVAILABLE event for linked ports */
            pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, true);
            hdmi_jdata->active_port_type = PA_QAHW_JACK_TYPE_INVALID;
            hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;

            /* If linked ports set means HDMI ARC is linked to HDMI IN */
            /* Check arc state */
            if ((hdmi_jdata->linked_ports) || (hdmi_jdata->port_type == PA_QAHW_JACK_TYPE_HDMI_ARC))
                check_hdmi_arc_state(hdmi_jdata);

            /* Reset current jack out config */
            memset(&curr_hdmi_jack_config, 0, sizeof(pa_qahw_jack_out_config));
        } else if ((audio_state_changed) && (hdmi_jdata->jack_plugin_status == PA_QAHW_JACK_AVAILABLE) &&
                   (!pa_qahw_hdmi_in_jack_get_config(hdmi_jdata->active_valid_port_type, hdmi_jdata->jack_in_config->jack_sys_path, &new_port_config))) {
            if (new_port_config.active_jack == PA_QAHW_JACK_TYPE_INVALID) {
                /* Check whether the audio_state is invalid *
                 * If true raise PA_QAHW_JACK_NO_VALID_STREAM for active port */
                if (is_hdmi_no_stream_event_valid(hdmi_jdata))
                    pa_qahw_hdmi_jack_raise_event(hdmi_jdata->active_port_type, PA_QAHW_JACK_NO_VALID_STREAM, NULL, hdmi_jdata, false);
            } else if ((new_port_config.active_jack != PA_QAHW_JACK_TYPE_INVALID) ||
                       (new_port_config.preemph_status != curr_hdmi_jack_config.preemph_status)) {
                hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;

                /* If primary port is same as new active jack */
                if (hdmi_jdata->port_type == new_port_config.active_jack) {
                    /* This situation might happen when linked ports are supported */
                    if ((hdmi_jdata->jack_in_config->linked_ports) && (hdmi_jdata->port_status == PA_QAHW_JACK_UNAVAILABLE)) {
                        /* Raise PA_QAHW_JACK_UNAVAILABLE for current active linked port */
                        pa_qahw_hdmi_jack_raise_event(hdmi_jdata->active_port_type, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, false);

                        /* Raise PA_QAHW_JACK_AVAILABLE for active  port */
                        pa_qahw_hdmi_jack_raise_event(hdmi_jdata->port_type, PA_QAHW_JACK_AVAILABLE, NULL, hdmi_jdata, false);
                        hdmi_jdata->active_port_type = hdmi_jdata->port_type;
                        hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;
                    }
                } else if (hdmi_jdata->jack_in_config->linked_ports) {
                    if (hdmi_jdata->active_port_type != new_port_config.active_jack) {
                        /* If current active port is not same as new active jack */
                        /* Raise PA_QAHW_JACK_UNAVAILABLE for current active port */
                        pa_qahw_hdmi_jack_raise_event(hdmi_jdata->active_port_type, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, false);

                        /* Raise PA_QAHW_JACK_AVAILABLE for new active jack */
                        pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_AVAILABLE, NULL, hdmi_jdata, false);
                        hdmi_jdata->active_port_type = new_port_config.active_jack;
                        hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;
                    }
                }
                /* Check whether the audio_state is invalid *
                 * If true raise PA_QAHW_JACK_NO_VALID_STREAM for active port *
                 * else raise PA_QAHW_JACK_CONFIG_UPDATE event for active port */
                if (is_hdmi_no_stream_event_valid(hdmi_jdata)) {
                    pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_NO_VALID_STREAM, NULL, hdmi_jdata, false);
                } else {
                    if (is_hdmi_config_update_event_valid(new_port_config, hdmi_jdata))
                        pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_CONFIG_UPDATE, &new_port_config, hdmi_jdata, false);

                    memcpy(&curr_hdmi_jack_config, &new_port_config, sizeof(pa_qahw_jack_out_config));
                }
            }
        } else if (hdmi_jdata->jack_plugin_status != PA_QAHW_JACK_AVAILABLE) {
            /* This situation can arrive when linkon_0 state is unset */
            if (arc_enable_state == 1) {
                /* ARC events can occur even when linkon_0 is unset */
                /* Raise PA_QAHW_JACK_AVAILABLE event for HDMI-ARC */
                pa_qahw_hdmi_jack_raise_event(PA_QAHW_JACK_TYPE_HDMI_ARC, PA_QAHW_JACK_AVAILABLE, NULL, hdmi_jdata, false);

                if (!pa_qahw_hdmi_in_jack_get_config(hdmi_jdata->active_valid_port_type, hdmi_jdata->jack_in_config->jack_sys_path, &new_port_config)) {
                    if ((new_port_config.active_jack == PA_QAHW_JACK_TYPE_HDMI_ARC) ||
                        (new_port_config.preemph_status != curr_hdmi_jack_config.preemph_status)) {
                        /* Raise PA_QAHW_JACK_CONFIG_UPDATE for HDMI-ARC */
                        memcpy(&curr_hdmi_jack_config, &new_port_config, sizeof(pa_qahw_jack_out_config));
                        pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_CONFIG_UPDATE, &new_port_config, hdmi_jdata, false);
                        hdmi_jdata->active_port_type = PA_QAHW_JACK_TYPE_HDMI_ARC;
                        hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;
                    }
                }
            } else if (arc_enable_state == -1) {
                /* Raise PA_QAHW_JACK_UNAVAILABLE event for HDMI-ARC */
                pa_qahw_hdmi_jack_raise_event(PA_QAHW_JACK_TYPE_HDMI_ARC, PA_QAHW_JACK_UNAVAILABLE, NULL, hdmi_jdata, false);
                hdmi_jdata->active_port_type = PA_QAHW_JACK_TYPE_INVALID;
                hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;
            } else if (sec_spdif_event) {
                /* Proceed only if corresponding jack is available */
                if ((hdmi_jdata->active_port_type == PA_QAHW_JACK_TYPE_HDMI_ARC) &&
                    (!pa_qahw_hdmi_in_jack_get_config(hdmi_jdata->active_valid_port_type, hdmi_jdata->jack_in_config->jack_sys_path, &new_port_config))) {
                    if ((new_port_config.active_jack == PA_QAHW_JACK_TYPE_HDMI_ARC) ||
                        (new_port_config.preemph_status != curr_hdmi_jack_config.preemph_status)) {
                        /* Raise PA_QAHW_JACK_CONFIG_UPDATE for HDMI-ARC */
                        if (is_hdmi_config_update_event_valid(new_port_config, hdmi_jdata)) {
                            memcpy(&curr_hdmi_jack_config, &new_port_config, sizeof(pa_qahw_jack_out_config));
                            pa_qahw_hdmi_jack_raise_event(new_port_config.active_jack, PA_QAHW_JACK_CONFIG_UPDATE, &new_port_config, hdmi_jdata, false);
                        }
                    }
                }
            }
        }
    }
}

struct pa_qahw_jack_data* pa_qahw_hdmi_in_jack_detection_enable(pa_qahw_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                       pa_qahw_jack_callback_t callback, pa_qahw_jack_in_config *jack_in_config, void *client_data) {
    struct pa_qahw_jack_data *jdata = NULL;
    pa_qahw_hdmi_jack_data_t *hdmi_jdata = NULL;
    int sock_event_fd = -1;
    int i = 0;
    char *port_name;
    pa_qahw_hdmi_jack_linked_port_data_t *linked_port_data = NULL;

    sock_event_fd = poll_data_event_init(jack_type);
    if (sock_event_fd <= 0) {
        pa_log_error("Socket initialization failed\n");
        return NULL;
    }

    /* Initialize current jack out config */
    memset(&curr_hdmi_jack_config, 0, sizeof(pa_qahw_jack_out_config));

    jdata = pa_xnew0(struct pa_qahw_jack_data, 1);

    hdmi_jdata = pa_xnew0(pa_qahw_hdmi_jack_data_t, 1);
    jdata->prv_data = hdmi_jdata;

    hdmi_jdata->port_type = jack_type;
    jdata->jack_type = jack_type;

    hdmi_jdata->fd = sock_event_fd;
    hdmi_jdata->jack_in_config = jack_in_config;
    hdmi_jdata->active_port_type = PA_QAHW_JACK_TYPE_INVALID;
    hdmi_jdata->active_valid_port_type = hdmi_jdata->active_port_type;

    pa_hook_init(&(hdmi_jdata->event_hook), NULL);
    jdata->event_hook = &(hdmi_jdata->event_hook);

    if (hdmi_jdata->jack_in_config->linked_ports) {
        /* Create a list of linked ports and set status as PA_QAHW_JACK_UNAVAILABLE */
        hdmi_jdata->linked_ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

        while ((port_name = hdmi_jdata->jack_in_config->linked_ports[i++])) {
            linked_port_data = pa_xnew0(pa_qahw_hdmi_jack_linked_port_data_t, 1);
            linked_port_data->port_status = PA_QAHW_JACK_UNAVAILABLE;
            linked_port_data->port_type = pa_qahw_util_get_jack_type_from_port_name(port_name);

            pa_hashmap_put(hdmi_jdata->linked_ports, port_name, linked_port_data);
            linked_port_data = NULL;
        }
    }

    *hook_slot = pa_hook_connect(&(hdmi_jdata->event_hook), PA_HOOK_NORMAL, (pa_hook_cb_t)callback, client_data);

    /* Check if HDMI is already connected */
    check_hdmi_connection(hdmi_jdata);

    hdmi_jdata->io = m->core->mainloop->io_new(m->core->mainloop, sock_event_fd, PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP, jack_io_callback, hdmi_jdata);

    return jdata;
}

void pa_qahw_hdmi_in_jack_detection_disable(struct pa_qahw_jack_data *jdata, pa_module *m) {
    pa_qahw_hdmi_jack_data_t *hdmi_jdata;
    int i = 0;
    char *port_name;
    pa_qahw_hdmi_jack_linked_port_data_t *linked_port_data = NULL;

    pa_assert(jdata);

    hdmi_jdata = (pa_qahw_hdmi_jack_data_t *)jdata->prv_data;

    if (hdmi_jdata->jack_in_config->linked_ports) {
        while ((port_name = hdmi_jdata->jack_in_config->linked_ports[i++])) {
            linked_port_data = pa_hashmap_remove(hdmi_jdata->linked_ports, port_name);
            pa_xfree(linked_port_data);
        }

        pa_hashmap_free(hdmi_jdata->linked_ports);
    }

    /* Reset current jack out config */
    memset(&curr_hdmi_jack_config, 0, sizeof(pa_qahw_jack_out_config));

    if(hdmi_jdata->io)
        m->core->mainloop->io_free(hdmi_jdata->io);

    if (close(hdmi_jdata->fd))
        pa_log_error("Close socket failed with error %s\n", strerror(errno));

    if (hdmi_jdata->jack_in_config)
        pa_xfree(hdmi_jdata->jack_in_config);

    pa_hook_done(&(hdmi_jdata->event_hook));

    pa_xfree(hdmi_jdata);

    pa_xfree(jdata);
    jdata = NULL;
}
