/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/device-port.h>
#include <pulsecore/core-format.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/card.h>
#include <pulsecore/core-util.h>
#include <string.h>
#include <errno.h>
#include "PalDefs.h"
#include "PalApi.h"
#include "pal-card.h"
#include "pal-loopback.h"
#include "bt-a2dp-split.h"
#include "pal-utils.h"

#define BIT_WIDTH                       16
#define BTSINK_PAL_CUSTOM_CONFIG_KEY    "btsink-usecase"

int init_btsink(btsink_t **btsink, pa_pal_loopback_config *loopback_conf)
{
    btsink_t *btsink_p = NULL;
    pa_pal_card_port_config *config_port_in = NULL;

    pa_assert(btsink);
    pa_assert(loopback_conf);

    config_port_in = pa_hashmap_first(loopback_conf->in_ports);
    if (!config_port_in)
        return E_FAILURE;

    btsink_p = calloc(1, sizeof(btsink_t));
    if (btsink_p == NULL) {
        pa_log_error("%s: Memory allocation failed", __func__);
        return -ENOMEM;
    }
    btsink_p->is_running = false;
    btsink_p->is_mute = false;
    btsink_p->volume = 10.0;
    *btsink = btsink_p;

    if (pa_pal_set_device_connection_state(config_port_in->device, true)) {
        pa_log_error("bta2dp set_device_connection failed");
        free(btsink_p);
        *btsink = NULL;
        return E_FAILURE;
    }

    return E_SUCCESS;
}

int start_btsink(btsink_t *btsink, pa_pal_loopback_config *loopback_conf)
{
    int32_t ret = E_SUCCESS;
    struct pal_stream_attributes stream_attr;
    struct pal_channel_info ch_info;
    const int num_pal_devs = LOOPBACK_NUM_DEVICES;
    struct pal_device pal_devs[num_pal_devs];
    pa_pal_card_port_config *config_port_in = NULL;
    pa_pal_card_port_config *config_port_out = NULL;

    pa_log_debug("%s Enter", __func__);

    pa_assert(btsink);
    pa_assert(loopback_conf);

    config_port_in = pa_hashmap_first(loopback_conf->in_ports);
    config_port_out = pa_hashmap_first(loopback_conf->out_ports);

    if (!config_port_in || !config_port_out)
        return -EINVAL;

    /* Channel info */
    pa_pal_channel_map_to_pal(&config_port_in->default_map, &ch_info);
    stream_attr.in_media_config.ch_info = ch_info;
    pal_devs[BTSINK_IN].config.ch_info = ch_info;
    pa_pal_channel_map_to_pal(&config_port_out->default_map, &ch_info);
    stream_attr.out_media_config.ch_info = ch_info;
    pal_devs[BTSINK_OUT].config.ch_info = ch_info;

    /* Stream info */
    stream_attr.type = PAL_STREAM_LOOPBACK;
    stream_attr.direction = PAL_AUDIO_INPUT_OUTPUT;
    stream_attr.info.opt_stream_info.loopback_type = PAL_STREAM_LOOPBACK_PCM;
    stream_attr.in_media_config.sample_rate = config_port_in->default_spec.rate;
    stream_attr.in_media_config.bit_width = BIT_WIDTH;
    stream_attr.in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
    stream_attr.out_media_config.sample_rate = config_port_out->default_spec.rate;
    stream_attr.out_media_config.bit_width = BIT_WIDTH;
    stream_attr.out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

    /* Device info */
    pal_devs[BTSINK_IN].id = config_port_in->device;
    pal_devs[BTSINK_OUT].id = config_port_out->device;
    pal_devs[BTSINK_IN].config.sample_rate = config_port_in->default_spec.rate;
    pal_devs[BTSINK_IN].config.bit_width = BIT_WIDTH;
    pal_devs[BTSINK_IN].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
    pal_devs[BTSINK_OUT].config.sample_rate = config_port_out->default_spec.rate;
    pal_devs[BTSINK_OUT].config.bit_width = BIT_WIDTH;
    pal_devs[BTSINK_OUT].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

    pa_strlcpy(pal_devs[BTSINK_OUT].custom_config.custom_key, BTSINK_PAL_CUSTOM_CONFIG_KEY,
            sizeof(pal_devs[BTSINK_OUT].custom_config.custom_key));

    pa_log_debug("Source port config: id %d, sample_rate %u, "
            "channels %u, format %d, bw %d\n",
             pal_devs[BTSINK_IN].id, pal_devs[BTSINK_IN].config.sample_rate,
             pal_devs[BTSINK_IN].config.ch_info.channels,
             stream_attr.in_media_config.aud_fmt_id, pal_devs[BTSINK_IN].config.bit_width);

    pa_log_debug("Sink port config: id %d, sample_rate %u, "
            "channel_mask %u, format %d, bw %d\n",
            pal_devs[BTSINK_OUT].id, pal_devs[BTSINK_OUT].config.sample_rate,
            pal_devs[BTSINK_OUT].config.ch_info.channels,
            stream_attr.out_media_config.aud_fmt_id, pal_devs[BTSINK_OUT].config.bit_width);


    ret = pal_stream_open(&stream_attr, num_pal_devs, pal_devs,
            0, NULL, NULL, 0, &btsink->stream_handle);
    if (ret != E_SUCCESS) {
        pa_log_error("BT a2dp sink stream open failed, rc %d", ret);
        return ret;
    }
    ret = pal_stream_start(btsink->stream_handle);
    if (ret != E_SUCCESS) {
        pa_log_error("BT a2dp sink stream start failed, rc %d", ret);
        pal_stream_close(btsink->stream_handle);
        return ret;
    }
    btsink->is_running = true;
    pa_pal_set_volume(btsink->stream_handle, config_port_in->default_map.channels, btsink->volume);

    pa_log_debug("%s Exit", __func__);

    return ret;
}

int stop_btsink(btsink_t *btsink)
{
    int ret;
    pa_assert(btsink);

    pa_log_debug("%s Enter", __func__);

    if (!btsink->is_running) {
        pa_log_error("Usecase not active. Failed to stop!!!\n");
        return E_FAILURE;
    }

    btsink->is_running = false;
    ret = pal_stream_stop(btsink->stream_handle);
    if (ret != E_SUCCESS) {
        pa_log_error("BT a2dp sink stream stop failed\n");
        return ret;
    }

    ret = pal_stream_close(btsink->stream_handle);
    if (ret != E_SUCCESS) {
        pa_log_error("BT a2dp sink stream close failed\n");
        return ret;
    }
    btsink->stream_handle = NULL;
    pa_log_debug("%s Exit", __func__);

    return E_SUCCESS;
}

void deinit_btsink(btsink_t *btsink, pa_pal_loopback_config *loopback_conf)
{
    pa_pal_card_port_config *config_port_in = NULL;

    if (!btsink) {
        pa_log_debug("%s: No active btsink connection", __func__);
        return;
    }

    if (btsink->is_running) {
        stop_btsink(btsink);
    }

    pa_assert(loopback_conf);
    config_port_in = pa_hashmap_first(loopback_conf->in_ports);

    if (config_port_in && pa_pal_set_device_connection_state(config_port_in->device, false)) {
        pa_log_error("%s: set_device_connection failed for device id %u", __func__,
                config_port_in->device);
    }

    free(btsink);
}

