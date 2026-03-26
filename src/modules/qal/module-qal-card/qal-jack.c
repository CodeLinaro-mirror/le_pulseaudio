/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
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
 *
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

#include <pulsecore/thread.h>
#include "qal-jack.h"
#include "qal-jack-common.h"
#include "qal-utils.h"
#include "qal-jack-format.h"

struct userdata {
    struct pa_pal_jack_data **jdata;
    int jack_count;
};

static unsigned int enabled_jacks = 0x0;
static pa_hashmap *registered_jacks = NULL;

static void toggle_jack_status_bits(pa_pal_jack_type_t jack_type) {
    enabled_jacks ^= jack_type;
}

static bool is_jack_enabled(pa_pal_jack_type_t jack_type) {
    return (enabled_jacks & jack_type);
}

static bool pa_pal_jack_check_enable_status(struct pa_pal_jack_data *jdata, const char *port_name, pa_pal_jack_type_t jack_type) {
    bool status = true;

    if (!jdata) {
        pa_log_error("Jack %s detection failed", port_name);
        status = false;
    } else {
        jdata->ref_count++;
        pa_hashmap_put(registered_jacks, (char *)port_name, jdata);
        toggle_jack_status_bits(jack_type);
    }

    return status;
}

pa_pal_jack_handle_t *pa_pal_jack_register_event_callback(pa_pal_jack_type_t jack_type, pa_pal_jack_callback_t callback, pa_module *m,
                         pa_pal_jack_in_config *jack_in_config, void *client_data, bool is_external) {
    struct jack_userdata *u;
    struct pa_pal_jack_data *jdata = NULL;
    const char *port_name = NULL;

    pa_assert(m);

    if (!registered_jacks)
        registered_jacks = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    u = pa_xnew0(struct jack_userdata, 1);

    if ((jack_type == PA_PAL_JACK_TYPE_LINEOUT) || (jack_type == PA_PAL_JACK_TYPE_WIRED_HEADPHONE))
        jack_type = PA_PAL_JACK_TYPE_WIRED_HEADSET;

    port_name = pa_pal_util_get_port_name_from_jack_type(jack_type);
    if (!port_name)
        goto fail;

    if (!is_jack_enabled(jack_type)) {
        pa_log_info("jack_type 0x%x", jack_type);
        u->jack_type = jack_type;

       switch(jack_type) {
           case PA_PAL_JACK_TYPE_HDMI_OUT:
           case PA_PAL_JACK_TYPE_NATIVE_HDMI_OUT :
               jdata = pa_pal_hdmi_out_jack_detection_enable(jack_type, m, &(u->hook_slot),
                       callback, jack_in_config, client_data);
               break;
           case PA_PAL_JACK_TYPE_BTA2DP_IN:
           case PA_PAL_JACK_TYPE_BTA2DP_OUT:
           case PA_PAL_JACK_TYPE_BTSCO_IN:
           case PA_PAL_JACK_TYPE_BTSCO_OUT:
               jdata = pa_pal_external_jack_detection_enable(jack_type, m, &(u->hook_slot),
                       callback, client_data);
               break;
           case PA_PAL_JACK_TYPE_USB_OUT:
           case PA_PAL_JACK_TYPE_USB_IN:
               jdata = pa_pal_udev_jack_detection_enable(jack_type, m, &(u->hook_slot),
                       callback, jack_in_config, client_data);
               break;
           default:
               break;
       }

       if (!(pa_pal_jack_check_enable_status(jdata, port_name, jack_type)))
           goto fail;
    } else {
        u->jack_type = jack_type;
        jdata = pa_hashmap_get(registered_jacks, (char *)port_name);
        u->hook_slot = pa_hook_connect(jdata->event_hook, PA_HOOK_NORMAL, (pa_hook_cb_t)callback, client_data);
        jdata->ref_count++;
    }

    return (pa_pal_jack_handle_t *)u;

fail:
    pa_log_info("Unsupported jack type");
    pa_xfree(u);
    return NULL;
}

bool pa_pal_jack_deregister_event_callback(pa_pal_jack_handle_t *jack_handle, pa_module *m, bool is_external) {
    struct pa_pal_jack_data *jdata = NULL;
    struct jack_userdata *u = NULL;
    const char *port_name = NULL;

    pa_assert(jack_handle);
    pa_assert(m);

    u = (struct jack_userdata *)jack_handle;

    port_name = pa_pal_util_get_port_name_from_jack_type(u->jack_type);
    jdata = pa_hashmap_get(registered_jacks, (char *)port_name);
    if (!jdata)
        return false;

    pa_hook_slot_free(u->hook_slot);

    jdata->ref_count--;

    if (jdata->ref_count == 0) {
        pa_log_info("%s: dergister jack type %d",__func__, jdata->jack_type);

        switch (jdata->jack_type) {
            case PA_PAL_JACK_TYPE_HDMI_OUT:
            case PA_PAL_JACK_TYPE_NATIVE_HDMI_OUT:
                pa_pal_hdmi_out_jack_detection_disable(jdata, m);
                break;
            case PA_PAL_JACK_TYPE_BTA2DP_IN:
            case PA_PAL_JACK_TYPE_BTA2DP_OUT:
            case PA_PAL_JACK_TYPE_BTSCO_IN:
            case PA_PAL_JACK_TYPE_BTSCO_OUT:
                pa_pal_external_jack_detection_disable(jdata, m);
                break;
            case PA_PAL_JACK_TYPE_USB_OUT:
            case PA_PAL_JACK_TYPE_USB_IN:
                pa_pal_udev_jack_detection_disable(jdata, m);
                break;
        }
        pa_hashmap_remove(registered_jacks, port_name);
        toggle_jack_status_bits(jdata->jack_type);
    }

    pa_xfree(u);

    if (pa_hashmap_size(registered_jacks) == 0) {
        pa_hashmap_free(registered_jacks);
        registered_jacks = NULL;
        enabled_jacks = 0x0;
    }

    return true;
}

int pa_pal_fill_dynamic_port_info(pa_device_port *card_port, struct pal_device *pal_device) {
    pa_pal_jack_usb_device_address_t *usb_addr;
    pa_pal_card_port_device_data *port_device_data;
    size_t nbytes = 0;
    int rc = 0;

    port_device_data = PA_DEVICE_PORT_DATA(card_port);

    switch (port_device_data->device) {
        case PAL_DEVICE_OUT_USB_HEADSET:
        case PAL_DEVICE_IN_USB_HEADSET:
            rc = pa_proplist_get(card_port->proplist, PA_PROP_USB_ADDR, (void *)&usb_addr, &nbytes);
            if (PA_UNLIKELY(rc)) {
                pa_log_error("Get usb device address failed %d", rc);
            } else {
                pal_device->address.card_id = usb_addr->card_id;
                pal_device->address.device_num = usb_addr->device_num;
            }
            break;
        default:
            break;
    }

    return rc;
}
