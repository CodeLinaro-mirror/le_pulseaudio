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

static struct userdata g_jack_userdata = {0};
static unsigned int enabled_jacks = 0x0;

static void toggle_jack_status_bits(pa_qahw_jack_type_t jack_type) {
    enabled_jacks ^= jack_type;
}

static bool is_jack_enabled(pa_qahw_jack_type_t jack_type) {
    return (enabled_jacks & jack_type);
}

static struct pa_qahw_jack_data *pa_qahw_jack_get_jack_data(pa_qahw_jack_type_t jack_type) {
    int i;
    struct pa_qahw_jack_data *jdata = NULL;

    for (i = 0; i < g_jack_userdata.jack_count; i++) {
        jdata = g_jack_userdata.jdata[i];

        if ((jdata) && (jdata->jack_type == jack_type))
            break;
    }

    return jdata;
}

static bool pa_qahw_jack_check_enable_status(struct pa_qahw_jack_data *jdata, pa_qahw_jack_type_t jack_type) {
    bool status = true;

    if (!jdata) {
        pa_log_error("Jack %d detection failed", jack_type);
        status = false;
    } else {
        jdata->ref_count++;
        g_jack_userdata.jack_count++;
        toggle_jack_status_bits(jack_type);
    }

    return status;
}

pa_qahw_jack_handle_t *pa_qahw_jack_register_event_callback(pa_qahw_jack_type_t jack_type, pa_qahw_jack_callback_t callback, pa_module *m, void *client_data) {
    struct jack_userdata *u;
    struct pa_qahw_jack_data *jdata = NULL;

    pa_assert(m);

    if (!(g_jack_userdata.jdata)&& (g_jack_userdata.jack_count == 0))
        g_jack_userdata.jdata = pa_xnew(struct pa_qahw_jack_data *, 1);

    u = pa_xnew0(struct jack_userdata, 1);

    if ((jack_type & PA_QAHW_JACK_TYPE_LINEOUT) || (jack_type & PA_QAHW_JACK_TYPE_WIRED_HEADPHONE))
        jack_type = PA_QAHW_JACK_TYPE_WIRED_HEADSET;

    if (!is_jack_enabled(jack_type)) {
        pa_log_info("jack_type %x", jack_type);
        u->jack_type = jack_type;

        if ((jack_type == PA_QAHW_JACK_TYPE_WIRED_HEADSET) || (jack_type ==  PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS)) {
            g_jack_userdata.jdata[g_jack_userdata.jack_count] = pa_qahw_evdev_jack_device_open(jack_type, m, &(u->hook_slot), callback, client_data);
        } else if  (jack_type ==  PA_QAHW_JACK_TYPE_HDMI) {
            u->jack_type = PA_QAHW_JACK_TYPE_HDMI;
            g_jack_userdata.jdata[g_jack_userdata.jack_count] = pa_qahw_hdmi_jack_detection_enable(jack_type, m, &(u->hook_slot), callback, client_data);
        }

        if (!(pa_qahw_jack_check_enable_status(g_jack_userdata.jdata[g_jack_userdata.jack_count], jack_type)))
            goto fail;
    } else {
        u->jack_type = jack_type;
        jdata = pa_qahw_jack_get_jack_data(jack_type);
        u->hook_slot = pa_hook_connect(jdata->event_hook, PA_HOOK_NORMAL, (pa_hook_cb_t)callback, client_data);
        jdata->ref_count++;
    }

    return (pa_qahw_jack_handle_t *)u;

fail:
    pa_log_info("Unsupported jack type");
    pa_xfree(u);
    return NULL;
}

bool pa_qahw_jack_deregister_event_callback(pa_qahw_jack_handle_t *jack_handle, pa_module *m) {
    struct pa_qahw_jack_data *jdata = NULL;
    struct jack_userdata *u = NULL;

    pa_assert(jack_handle);
    pa_assert(m);

    u = (struct jack_userdata *)jack_handle;

    jdata = pa_qahw_jack_get_jack_data(u->jack_type);
    if (!jdata)
        return false;

    pa_hook_slot_free(u->hook_slot);

    jdata->ref_count--;

    if (jdata->ref_count == 0) {
        pa_log_info("%s: dergister jack type %d",__func__, jdata->jack_type);
        if ((jdata->jack_type ==  PA_QAHW_JACK_TYPE_WIRED_HEADSET) || (jdata->jack_type == PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS)) {
            pa_qahw_evdev_jack_device_close(jdata, m);
            toggle_jack_status_bits(jdata->jack_type);
            g_jack_userdata.jack_count--;
        } else if (jdata->jack_type & PA_QAHW_JACK_TYPE_HDMI) {
            pa_qahw_hdmi_jack_detection_disable(jdata, m);
            toggle_jack_status_bits(PA_QAHW_JACK_TYPE_HDMI);
            g_jack_userdata.jack_count--;
        }
    }

    pa_xfree(u);

    if (g_jack_userdata.jack_count == 0) {
        pa_xfree(g_jack_userdata.jdata);
        g_jack_userdata.jdata = NULL;
    }

    return true;
}
