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

#include <linux/input.h>

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include "qahw-jack.h"
#include "qahw-jack-common.h"
#include "qahw-utils.h"

#define MAX_PORT_NAME_SIZE 256
#define SW_HEADPHONE_INSERT_BIT (1 << SW_HEADPHONE_INSERT)
#define SW_MICROPHONE_INSERT_BIT (1 << SW_MICROPHONE_INSERT)
#define SW_LINEOUT_INSERT_BIT (1 << SW_LINEOUT_INSERT)

#define DEVICE_PATH "/dev/input"

#define HEADSET_SWITCH_MASK (SW_HEADPHONE_INSERT_BIT | SW_MICROPHONE_INSERT_BIT|SW_LINEOUT_INSERT_BIT)

/* Macro to tell if a bit in array is set */
#define TEST_BIT_SET(BIT, ARRAY)  (ARRAY[(BIT)/8] & (1<<((BIT)%8)))

static struct pa_qahw_jack_info supported_jacks[] = {
    {PA_QAHW_JACK_TYPE_WIRED_HEADSET, (char *)"snd-card Headset Jack"},
    {PA_QAHW_JACK_TYPE_WIRED_HEADPHONE, (char *)"snd-card Headset Jack"},
    {PA_QAHW_JACK_TYPE_LINEOUT, (char *)"snd-card Headset Jack"},
    {PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS, (char *)"snd-card Button Jack"}
};

static void report_jack_state(struct pa_qahw_jack_data *jdata) {
    pa_qahw_jack_event_data_t event_data;
    pa_qahw_jack_type_t jack_type = PA_QAHW_JACK_TYPE_INVALID;

    if (!jdata->switch_values) {
        pa_log_debug("Invalid switch value %x", jdata->switch_values);
        return;
    }

    jdata->switch_values = (jdata->switch_values & ~HEADSET_SWITCH_MASK) | jdata->switch_values;

    pa_log_debug("switch value %x", jdata->switch_values);

    switch (jdata->switch_values & HEADSET_SWITCH_MASK) {
       case SW_HEADPHONE_INSERT_BIT | SW_MICROPHONE_INSERT_BIT:
            jack_type = PA_QAHW_JACK_TYPE_WIRED_HEADSET;
            break;

        case SW_HEADPHONE_INSERT_BIT:
            jack_type = PA_QAHW_JACK_TYPE_WIRED_HEADPHONE;
            break;

        case SW_MICROPHONE_INSERT_BIT:
            jack_type = PA_QAHW_JACK_TYPE_WIRED_HEADSET;
            break;

        case SW_LINEOUT_INSERT_BIT:
            jack_type = PA_QAHW_JACK_TYPE_LINEOUT;
            break;

        default :
            jack_type = PA_QAHW_JACK_TYPE_INVALID;
     }

    jdata->switch_values = 0x0;

    if (jack_type == PA_QAHW_JACK_TYPE_INVALID)
        return;

    event_data.jack_type = jack_type;
    if (jdata->jack_status == PA_AVAILABLE_YES) {
        pa_log_info("qahw jack type %d available", jack_type);
        event_data.event = PA_QAHW_JACK_AVAILABLE;
        pa_hook_fire(&(jdata->event_hook), &event_data);
    } else {
        pa_log_info("qahw jack type %d unavailable", jack_type);
        event_data.event = PA_QAHW_JACK_UNAVAILABLE;
        pa_hook_fire(&(jdata->event_hook), &event_data);
    }
}

static void update_switch_value(struct pa_qahw_jack_data *jdata, uint16_t code, uint16_t value) {

    bool update_jack_status = true;

    switch (code) {
        case SW_HEADPHONE_INSERT:
            jdata->switch_values |= SW_HEADPHONE_INSERT_BIT;
            break;

        case SW_MICROPHONE_INSERT:
            jdata->switch_values |= SW_MICROPHONE_INSERT_BIT;
            break;

        case SW_LINEOUT_INSERT:
            jdata->switch_values |= SW_LINEOUT_INSERT_BIT;
            break;

        case SW_JACK_PHYSICAL_INSERT:
            /* ignore this event, not needed for jack detection */
            update_jack_status = false;
            break;

        default :
            update_jack_status = false;
            pa_log_debug("Unsupported code %x", code);
    }

    if (update_jack_status)
        jdata->jack_status = value ? PA_AVAILABLE_YES : PA_AVAILABLE_NO;

    return;
}

static void jack_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t io_events, void *userdata) {
    struct pa_qahw_jack_data *jdata = userdata;
    int fd_type;
    struct input_event event;

    pa_assert(io);
    pa_assert(jdata);

    if (io_events & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) {
        pa_log("connection to evdev device Lost");
        return;
    }

    if (io_events & PA_IO_EVENT_INPUT) {

        if (pa_loop_read(jdata->fd, &event, sizeof(event), &fd_type) <= 0) {
            pa_log("Failed to read from event device: %s", pa_cstrerror(errno));
            return;
        }

        pa_log_debug("event type %d Key code=%u, value=%u", event.type, event.code, event.value);

        if (event.type == EV_SW) {
            pa_log_debug("event type is Switch");
            update_switch_value(jdata, event.code, event.value);
        } else if ((event.type == EV_SYN)) {
            report_jack_state(jdata);
        } else if (event.type == EV_KEY) {
            pa_log_debug("event type is Button(not supported currently)");
        }
    }
    return;
}

static struct pa_qahw_jack_data* open_device(pa_qahw_jack_type_t jack_type, pa_module *m, const char *dev_name,
                                    pa_hook_slot **hook_slot, pa_qahw_jack_callback_t callback, void *prv_data) {
    int version;
    char name[256];
    uint8_t evtype_bitmask[EV_MAX/8 + 1];
    uint8_t sw_state[EV_MAX/8 + 1];
    struct input_id input_id;
    int fd, i, size;
    struct pa_qahw_jack_data *jdata = NULL;

    pa_assert(m);
    pa_assert(dev_name);

    if ((fd = pa_open_cloexec(dev_name, O_RDONLY | O_NONBLOCK, 0)) < 0) {
        pa_log_error("open evdev device: %s failed", pa_cstrerror(errno));
        goto fail;
    }

    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        pa_log_error("ioct for EVIOCGNAME failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    pa_log_info("evdev device name: %s", name);

    size = ARRAY_SIZE(supported_jacks);
    for (i = 0; i < size; i++)
        if ((jack_type == supported_jacks[i].jack_type) && (strstr(name, supported_jacks[i].name)))
            break;

    if (i == size) {
        pa_log_debug("Not a valid jack type");
        goto fail;
    } else {
        pa_log_info("device %s jack type %d\n", dev_name, jack_type);
    }

    if (ioctl(fd, EVIOCGVERSION, &version) < 0) {
        pa_log_error("ioctl for EVIOCGVERSION failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    pa_log_info("evdev driver version %i.%i.%i", version >> 16, (version >> 8) & 0xff, version & 0xff);

    if (ioctl(fd, EVIOCGID, &input_id)) {
        pa_log_error("ioctl for EVIOCGID failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    pa_log_info("evdev  bustype %u vendor 0x%04x product 0x%04x version 0x%04x", input_id.bustype, input_id.vendor, input_id.product, input_id.version);

    memset(evtype_bitmask, 0, sizeof(evtype_bitmask));
    if (ioctl(fd, EVIOCGBIT(0, EV_MAX), evtype_bitmask) < 0) {
        pa_log_error("ioctl for EVIOCGBIT failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    jdata = pa_xnew0(struct pa_qahw_jack_data, 1);

    jdata->jack_type = jack_type;
    jdata->fd = fd;
    pa_hook_init(&(jdata->event_hook), NULL);

    *hook_slot = pa_hook_connect(&(jdata->event_hook), PA_HOOK_NORMAL, (pa_hook_cb_t)callback, prv_data);

    jdata->io = m->core->mainloop->io_new(m->core->mainloop, fd, PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP, jack_io_callback, jdata);

    if ((jack_type == PA_QAHW_JACK_TYPE_WIRED_HEADSET) || (jack_type == PA_QAHW_JACK_TYPE_LINEOUT)) {

        if (TEST_BIT_SET(EV_SW, evtype_bitmask)) {
            /* check if device is already connected */
            memset(sw_state, 0, sizeof(sw_state));
            if (ioctl(fd, EVIOCGSW(sizeof(sw_state)), sw_state) >= 0) {
                if (TEST_BIT_SET(SW_MICROPHONE_INSERT, sw_state)) {
                    pa_log_debug("SW_MICROPHONE_INSERT");
                    update_switch_value(jdata, SW_MICROPHONE_INSERT, 1);
                }

                if (TEST_BIT_SET(SW_HEADPHONE_INSERT, sw_state)) {
                    pa_log_debug("SW_HEADPHONE_INSERT");
                    update_switch_value(jdata, SW_HEADPHONE_INSERT, 1);
                }

                if (TEST_BIT_SET(SW_LINEOUT_INSERT, sw_state)) {
                    pa_log_debug("SW_LINEOUT_INSERT");
                    update_switch_value(jdata, SW_LINEOUT_INSERT, 1);
                }

                report_jack_state(jdata);
            } else {
                pa_log_error("Device has no software switch.");
                goto fail;
            }
        }
    } else if ((jack_type == PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS) && !TEST_BIT_SET(EV_KEY, evtype_bitmask)) {
        pa_log("Device has no software key");
        goto fail;
    }

   return jdata;

fail:
   if (jdata)
       pa_xfree(jdata);

   pa_close(fd);
   return NULL;
}

int pa_qahw_evdev_jack_device_close(struct pa_qahw_jack_data *jdata, pa_module *m) {
    int rc;

    pa_assert(jdata);

    if(jdata->io)
        m->core->mainloop->io_free(jdata->io);

    rc= pa_close(jdata->fd);
    if (rc < 0)
        pa_log("pa_close (fd %d) failed: %s",jdata->fd, pa_cstrerror(errno));

    pa_hook_done(&(jdata->event_hook));

    pa_xfree(jdata);
    jdata = NULL;

    return 0;
}

struct pa_qahw_jack_data* pa_qahw_evdev_jack_device_open(pa_qahw_jack_type_t jack_type, pa_module *m, pa_hook_slot **hook_slot,
                                                                              pa_qahw_jack_callback_t callback, void *prv_data) {
    char dev_name[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;
    struct pa_qahw_jack_data *jdata;

    /* scan directory and find appropriate device */
    dir = opendir(DEVICE_PATH);
    if(dir == NULL)
        return NULL;
    strlcpy(dev_name, DEVICE_PATH, sizeof(dev_name));
    filename = dev_name + strlen(dev_name);
    *filename++ = '/';
    while((de = readdir(dir))) {
        if(de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        strlcpy(filename, de->d_name, sizeof(filename));

        /* continue till both headset and button device are opened */
        jdata = open_device(jack_type, m, dev_name, hook_slot, callback, prv_data);
        if (jdata) {
            pa_log_debug("jack type %d is supported", jack_type);
            break;
        }
    }

    if (jdata == NULL) {
        pa_log_error(" jack type %d not supported", jack_type);
    }

    closedir(dir);
    return jdata;
}
