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

struct userdata {
    struct pa_qahw_jack_data **jdata;
    int jack_count;
};

static unsigned int num_of_set_bits(int value) {
    unsigned int count = 0;
    while (value) {
        value &= (value-1) ;
        count++;
    }
    return count;
}

int pa_qahw_jack_enable(pa_module *m, pa_qahw_jack_type_t jack_types, pa_qahw_jack_callback_t callback, pa_qahw_jack_handle_t **jack_handle, void *prv_data) {
    struct userdata *u;
    int count;

    pa_assert(m);

    u = pa_xnew(struct userdata, 1);
    memset(u, 0, sizeof(struct userdata));

    *jack_handle = (pa_qahw_jack_handle_t *)u;

    count = num_of_set_bits(jack_types);

    u->jdata = pa_xnew(struct pa_qahw_jack_data *, count);
    pa_log_info("jack_type %x", jack_types);

    /* fall through as multiple bits might be set */
    if ((jack_types & PA_QAHW_JACK_TYPE_WIRED_HEADSET) || (jack_types & PA_QAHW_JACK_TYPE_LINEOUT) || (jack_types & PA_QAHW_JACK_TYPE_WIRED_HEADPHONE)) {
        /* call pa_qahw_evdev_jack_device_open only once for headset, headphone and lineout as same dev/input/device is used for both */
        u->jdata[u->jack_count] = pa_qahw_evdev_jack_device_open(PA_QAHW_JACK_TYPE_WIRED_HEADSET, m, callback, prv_data);
        if (!u->jdata[u->jack_count])
            pa_log_error("PA_QAHW_JACK_TYPE_WIRED_HEADSET/PA_QAHW_JACK_TYPE_LINEOUT detection failed");
        else
            u->jack_count++;
    }

    if (jack_types & PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS) {
        pa_log_info("Enabling PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS detection");

        u->jdata[u->jack_count] = pa_qahw_evdev_jack_device_open(PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS, m, callback, prv_data);
        if (!u->jdata[u->jack_count])
            pa_log_error("Enabling PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS detection failed");
        else
            u->jack_count++;
    }

    if (u->jack_count <= 0)
        goto fail;

    return 0;
fail:
    pa_log_info("Unsupported jack type");
    pa_qahw_jack_disable(*jack_handle);
    *jack_handle = NULL;
    return -1;
}

void pa_qahw_jack_disable(pa_qahw_jack_handle_t *jack_handle) {
    struct userdata *u = (struct userdata *)jack_handle;
    struct pa_qahw_jack_data *jdata;
    int i;

    pa_assert(u);

    for (i = 0; i < u->jack_count; i++) {
        jdata = u->jdata[i];

        if (!jdata)
            continue;

        if (jdata->jack_type & PA_QAHW_JACK_TYPE_WIRED_HEADSET) {
            pa_log_debug("Disabling PA_QAHW_JACK_TYPE_WIRED_HEADSET detection");
            pa_qahw_evdev_jack_device_close(jdata);
        } else if (jdata->jack_type & PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS) {
            pa_log_debug("Disabling PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS detection");
            pa_qahw_evdev_jack_device_close(jdata);
        }
    }
    pa_xfree(u);
}
