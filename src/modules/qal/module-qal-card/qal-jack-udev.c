/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>

#include "qal-jack-common.h"
#include "qal-jack-format.h"

#define USB_CARD_SND_FLAG "usb"
#define USB_BUFF_SIZE 4096
#define MAX_DEVICE_NUM 10
#define SYSFS_PATH_MAX 128

enum {
    UNKNOWN = 0,
    PLAYBACK,
    CAPTURE,
};

typedef struct {
    struct udev* udev;
    int udev_fd;
    struct udev_monitor *monitor;

    pa_io_event *udev_io;
    pa_hook event_hook;
    pa_pal_jack_type_t jack_type;
    pa_pal_jack_event_t jack_plugin_status;
    pa_pal_jack_in_config *jack_in_config;
    pa_pal_jack_usb_device_address_t usb_addr;
} pa_pal_udev_jack_data_t;

static int get_capability(pa_pal_udev_jack_data_t *udev_jdata, int card_id) {
    const char *card_stream = "/proc/asound/card%u/stream0";
    char path[SYSFS_PATH_MAX];
    FILE *fd = NULL;
    size_t num_read = 0;
    char *read_buf = NULL;
    int ret = 0;

    ret = snprintf(path, sizeof(path), card_stream, card_id);
    if(ret < 0) {
        pa_log_error("failed on snprintf (%d) to path %s\n", ret, path);
        goto exit;
    }

    fd = fopen(path, "r");
    if (!fd) {
        pa_log_error("failed to open config file %s error: %d\n", path, errno);
        ret = -EINVAL;
        goto exit;
    }

    read_buf = pa_xnew0(char, USB_BUFF_SIZE + 1);
    if (!read_buf) {
        pa_log_error("failed to create read_buf");
        ret = -ENOMEM;
        goto exit;
    }

    if ((num_read = fread(read_buf, 1, USB_BUFF_SIZE, fd)) < 0) {
        pa_log_error("file read error");
        goto exit;
    }
    read_buf[num_read] = '\0';

    if (strstr(read_buf, "Playback:")) {
        udev_jdata->usb_addr.capability |= PLAYBACK;
    }

    if (strstr(read_buf, "Capture:")) {
        udev_jdata->usb_addr.capability |= CAPTURE;
    }

exit:
    return ret;
}

static void report_events(pa_pal_udev_jack_data_t *udev_jdata, bool plugin) {
    pa_pal_jack_event_data_t event_data;

    if (((udev_jdata->jack_type == PA_PAL_JACK_TYPE_USB_OUT) && (udev_jdata->usb_addr.capability & PLAYBACK)) ||
            ((udev_jdata->jack_type == PA_PAL_JACK_TYPE_USB_IN) && (udev_jdata->usb_addr.capability & CAPTURE))) {
        if (plugin) {
            pa_pal_jack_out_config config;

            /* Raise jack available event */
            event_data.jack_type = udev_jdata->jack_type;
            event_data.event = PA_PAL_JACK_AVAILABLE;
            event_data.pa_pal_jack_info = &(udev_jdata->usb_addr);
            pa_log_info("qal jack type 0x%x available", udev_jdata->jack_type);
            pa_hook_fire(&(udev_jdata->event_hook), &event_data);
            udev_jdata->jack_plugin_status = PA_PAL_JACK_AVAILABLE;

            /* Set default config */
            pa_pal_format_set_jack_default_config(&config);

            /* Generate jack config update event */
            event_data.pa_pal_jack_info = &config;
            event_data.event = PA_PAL_JACK_CONFIG_UPDATE;
            pa_hook_fire(&(udev_jdata->event_hook), &event_data);
        } else {
            /* Raise jack unavailable event */
            event_data.jack_type = udev_jdata->jack_type;
            event_data.event = PA_PAL_JACK_UNAVAILABLE;
            event_data.pa_pal_jack_info = &(udev_jdata->usb_addr);
            pa_log_info("qal jack type 0x%x unavailable", udev_jdata->jack_type);
            pa_hook_fire(&(udev_jdata->event_hook), &event_data);
            udev_jdata->jack_plugin_status = PA_PAL_JACK_UNAVAILABLE;
        }
   }
}

static void check_usb_audio_connection(pa_pal_udev_jack_data_t *udev_jdata) {
    const char *cards = "/proc/asound/cards";
    const char *snd = "/dev/snd/pcmC%uD%u%c";
    FILE *pf = NULL;
    char path[SYSFS_PATH_MAX];
    int fd;
    char **items = NULL;
    char *item = NULL;
    char *card_string[USB_BUFF_SIZE];
    bool found_usb_card = false;
    int card_id = -1;
    int device_num = 0;
    int i;

    pa_assert(udev_jdata);

    if (!(pf = pa_fopen_cloexec(cards, "rb"))) {
        pa_log_error("Open %s failed\n", cards);
        return;
    }

    /* Get card id */
    while (fgets(card_string, USB_BUFF_SIZE - 1, pf) != NULL) {
        pa_strip_nl(card_string);

        items = pa_split_spaces_strv(card_string);
        if (!items) {
            pa_log_error("%s: invalid sound card name %s", __func__, card_string);
            continue;
        }

        if (strstr(card_string, " ["))
            card_id = atoi(items[0]);

        i = 0;
        while ((item = items[i++])) {
            if (strstr(item, USB_CARD_SND_FLAG)) {
                found_usb_card = true;
                break;
            }
        }

        if (found_usb_card && (card_id >= 0 ))
            break;
    }

    if (!found_usb_card || (card_id < 0)) {
        pa_log_debug("%s: no usb sound card found", __func__);
        return;
    }

    /* Get capability */
    if (get_capability(udev_jdata, card_id) < 0)
        return;

    udev_jdata->usb_addr.card_id = card_id;
    while (device_num < MAX_DEVICE_NUM) {
        snprintf(path, sizeof(path), snd, card_id, device_num++, udev_jdata->usb_addr.capability & PLAYBACK ? 'p' : 'c');
        fd = open(path, O_RDWR|O_NONBLOCK);
        if (fd >= 0) {
            close(fd);
            pa_log_debug("%s: found %s", __func__, path);
            break;
        }
    }
    udev_jdata->usb_addr.device_num = device_num;

    pa_log_debug("%s: found usb sound card %d capability %d", __func__, card_id,
                                udev_jdata->usb_addr.capability);

    report_events(udev_jdata, true);
}

static void jack_io_callback(pa_mainloop_api *io, pa_io_event* e, int fd, pa_io_event_flags_t io_events,
        void *userdata) {
    pa_pal_udev_jack_data_t *udev_jdata = userdata;
    struct udev_device *dev;
    const char *path;
    const char *action;
    const char *tail;
    int card_id, device_num = -1;
    int capability = UNKNOWN;

    pa_assert(io);
    pa_assert(udev_jdata);

    if (!(dev = udev_monitor_receive_device(udev_jdata->monitor))) {
        pa_log_error("Failed to get udev device object from monitor.");
        goto fail;
    }

    path = udev_device_get_devpath(dev);

    /* Get card id and capability */
    if ((tail = strrchr(path, '/')) && pa_startswith(tail, "/pcmC")) {
        card_id = atoi(tail + 5);
        device_num = atoi(tail + 7);
        if (strstr((tail + 8), "p"))
            capability |= PLAYBACK;
        else if (strstr((tail + 8), "c"))
            capability |= CAPTURE;
    }

    if (card_id < 0)
        goto end;

    udev_jdata->usb_addr.card_id = card_id;
    udev_jdata->usb_addr.device_num = device_num;
    udev_jdata->usb_addr.capability = capability;

    /* Check add or remove */
    action = udev_device_get_action(dev);
    if (action && pa_streq(action, "remove")) {
        pa_log_debug("%s: found usb sound card %d removed capability %d", __func__, card_id, capability);
        report_events(udev_jdata, false);
    } else if ((action && pa_streq(action, "add")) ||
                ((!action || pa_streq(action, "change")) && udev_device_get_property_value(dev, "SOUND_INITIALIZED"))) {
        pa_log_debug("%s: found usb sound card %d added", __func__, card_id, capability);
        report_events(udev_jdata, true);
    }

end:
    udev_device_unref(dev);
    return;

fail:
    io->io_free(udev_jdata->udev_io);
    udev_jdata->udev_io = NULL;
}

struct pa_pal_jack_data* pa_pal_udev_jack_detection_enable(pa_pal_jack_type_t jack_type, pa_module *m,
                                               pa_hook_slot **hook_slot, pa_pal_jack_callback_t callback,
                                                pa_pal_jack_in_config *jack_in_config, void *client_data) {
    struct pa_pal_jack_data *jdata = NULL;
    pa_pal_udev_jack_data_t *udev_jdata = NULL;

    jdata = pa_xnew0(struct pa_pal_jack_data, 1);
    udev_jdata = pa_xnew0(pa_pal_udev_jack_data_t, 1);

    jdata->prv_data = udev_jdata;

    jdata->jack_type = jack_type;
    udev_jdata->jack_type = jack_type;

    udev_jdata->jack_in_config = jack_in_config;
    udev_jdata->jack_plugin_status = PA_PAL_JACK_UNAVAILABLE;

    udev_jdata->usb_addr.card_id = -1;
    udev_jdata->usb_addr.device_num = -1;
    udev_jdata->usb_addr.capability = UNKNOWN;

    if (!(udev_jdata->udev = udev_new())) {
        pa_log_error("Failed to initialize udev library.");
        goto end;
    }

    if (!(udev_jdata->monitor = udev_monitor_new_from_netlink(udev_jdata->udev, "udev"))) {
        pa_log_error("Failed to initialize monitor.");
        goto end;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(udev_jdata->monitor, "sound", NULL) < 0) {
        pa_log_error("Failed to subscribe to sound devices.");
        goto end;
    }

    if (udev_monitor_enable_receiving(udev_jdata->monitor) < 0) {
        pa_log_error("Failed to enable monitor: %s", pa_cstrerror(errno));
        goto end;
    }

    if ((udev_jdata->udev_fd = udev_monitor_get_fd(udev_jdata->monitor)) < 0) {
        pa_log_error("Failed to get udev monitor fd.");
        goto end;
    }

    pa_hook_init(&(udev_jdata->event_hook), NULL);
    jdata->event_hook = &(udev_jdata->event_hook);

    *hook_slot = pa_hook_connect(&(udev_jdata->event_hook), PA_HOOK_NORMAL, (pa_hook_cb_t)callback, client_data);

    /* Check if jack is already connected */
    check_usb_audio_connection(udev_jdata);

    pa_assert_se(udev_jdata->udev_io = m->core->mainloop->io_new(m->core->mainloop, udev_jdata->udev_fd,
            PA_IO_EVENT_INPUT, jack_io_callback, udev_jdata));

end:
    return jdata;
}

void pa_pal_udev_jack_detection_disable(struct pa_pal_jack_data *jdata, pa_module *m) {
    pa_pal_udev_jack_data_t *udev_jdata;

    pa_assert(jdata);

    udev_jdata = (pa_pal_udev_jack_data_t *)jdata->prv_data;

    if (udev_jdata->udev_io)
        m->core->mainloop->io_free(udev_jdata->udev_io);

    if (close(udev_jdata->udev_fd))
        pa_log_error("Close socket failed with error %s\n", strerror(errno));

    if (udev_jdata->jack_in_config)
        pa_xfree(udev_jdata->jack_in_config);

    pa_hook_done(&(udev_jdata->event_hook));

    pa_xfree(udev_jdata);

    pa_xfree(jdata);
    jdata = NULL;
}
