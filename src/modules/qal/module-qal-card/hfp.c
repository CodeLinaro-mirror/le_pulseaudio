/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
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
#include "qal-card.h"
#include "pal-loopback.h"
#include "qal-utils.h"
#include "hfp.h"

#define DEFAULT_BIT_WIDTH                   16
#define DEFAULT_SAMPLE_RATE                 16000
#define DEFAULT_VOLUME                      10.0f
#define HFPRX_OUT_PAL_CUSTOM_CONFIG_KEY     "hfp-usecase"

int set_btsco_params(btsco_t *btsco, pal_param_id_type_t param_id, bool is_sco_on)
{
    int ret = E_SUCCESS;

    pal_param_btsco_t param_btsco;
    pa_assert(btsco);

    memset(&param_btsco, 0, sizeof(param_btsco));
    param_btsco.is_bt_hfp = true;
    param_btsco.bt_sco_on = is_sco_on;
    if (param_id == PAL_PARAM_ID_BT_SCO_WB) {
        if (btsco->sample_rate == DEFAULT_SAMPLE_RATE)
            param_btsco.bt_wb_speech_enabled = true;
        else
            param_btsco.bt_wb_speech_enabled = false;
    }

    ret =  pal_set_param(param_id, (void*)&param_btsco,
            sizeof(pal_param_btsco_t));
    if (ret != 0) {
        pa_log_error("Set param_id=%d failed", param_id);
    }

    return ret;
}

int init_btsco(btsco_t **btsco, pa_pal_loopback_config **loopback_config)
{
    btsco_t *btsco_p = NULL;
    pa_pal_card_port_config *config_port_in = NULL;
    pa_pal_card_port_config *config_port_out = NULL;

    pa_assert(btsco);
    pa_assert(loopback_config);

    config_port_in = pa_hashmap_first(loopback_config[LB_PROF_HFP_RX]->in_ports);
    config_port_out = pa_hashmap_first(loopback_config[LB_PROF_HFP_TX]->out_ports);

    if (!config_port_in || !config_port_out)
        return E_FAILURE;

    btsco_p = calloc(1, sizeof(btsco_t));
    if (!btsco_p) {
        pa_log_error("%s: Memory allocation failed", __func__);
        return -ENOMEM;
    }
    btsco_p->is_running = false;
    btsco_p->rx_mute = false;
    btsco_p->tx_mute = false;
    btsco_p->rx_volume = DEFAULT_VOLUME;
    btsco_p->tx_volume = DEFAULT_VOLUME;
    btsco_p->sample_rate = config_port_in->default_spec.rate =
        config_port_out->default_spec.rate;

    if (pa_pal_set_device_connection_state(config_port_in->device, true)) {
        pa_log_error("%s: set_device_connection failed for pal device %d", __func__,
                config_port_in->device);
        goto error_1;
    }
    if (pa_pal_set_device_connection_state(config_port_out->device, true)) {
        pa_log_error("%s: set_device_connection failed for pal device %d", __func__,
                config_port_out->device);
        goto error_1;
    }
    if (set_btsco_params(btsco_p, PAL_PARAM_ID_BT_SCO, true)) {
        pa_log_error("%s: set_params failed for btsco", __func__);
        goto error_1;
    }
    if (set_btsco_params(btsco_p, PAL_PARAM_ID_BT_SCO_WB, true)) {
        pa_log_error("%s: set_params failed for btsco", __func__);
        goto error_1;
    }
    *btsco = btsco_p;

    return E_SUCCESS;

error_1:
    free(btsco_p);
    *btsco = NULL;
    return E_FAILURE;
}

int start_hfp(btsco_t *btsco, pa_pal_loopback_config **loopback)
{
    int ret = E_SUCCESS;
    uint32_t no_of_devices = LOOPBACK_NUM_DEVICES;
    struct pal_stream_attributes stream_rx_attr = {};
    struct pal_stream_attributes stream_tx_attr = {};
    struct pal_device devices[LOOPBACK_NUM_DEVICES] = {};
    struct pal_channel_info ch_info;
    pa_pal_card_port_config *rx_config_port_in = NULL, *rx_config_port_out = NULL;
    pa_pal_card_port_config *tx_config_port_in = NULL, *tx_config_port_out = NULL;

    pa_assert(btsco);
    pa_assert(loopback);

    pa_log_debug("%s Enter", __func__);

    rx_config_port_in = pa_hashmap_first(loopback[LB_PROF_HFP_RX]->in_ports);
    rx_config_port_out = pa_hashmap_first(loopback[LB_PROF_HFP_RX]->out_ports);
    tx_config_port_in = pa_hashmap_first(loopback[LB_PROF_HFP_TX]->in_ports);
    tx_config_port_out = pa_hashmap_first(loopback[LB_PROF_HFP_TX]->out_ports);

    if (!rx_config_port_in || !rx_config_port_out ||
            !tx_config_port_in || !tx_config_port_out)
        return -EINVAL;

    if (btsco->sample_rate != rx_config_port_in->default_spec.rate) {
        ret = set_btsco_params(btsco, PAL_PARAM_ID_BT_SCO_WB, true);
        if (ret != 0) {
            pa_log_error("%s: set_params failed for btsco", __func__);
            return ret;
        }
    }

    /* Channel info */
    pa_pal_channel_map_to_pal(&rx_config_port_in->default_map, &ch_info);
    stream_rx_attr.in_media_config.ch_info = ch_info;
    stream_rx_attr.out_media_config.ch_info = ch_info;
    devices[HFPRX_IN].config.ch_info = ch_info;

    /* Stream info */
    stream_rx_attr.type = PAL_STREAM_LOOPBACK;
    stream_rx_attr.info.opt_stream_info.loopback_type = PAL_STREAM_LOOPBACK_HFP_RX;
    stream_rx_attr.direction = PAL_AUDIO_INPUT_OUTPUT;
    stream_rx_attr.in_media_config.sample_rate = btsco->sample_rate;
    stream_rx_attr.in_media_config.bit_width = DEFAULT_BIT_WIDTH;
    stream_rx_attr.in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
    stream_rx_attr.out_media_config.sample_rate = rx_config_port_out->default_spec.rate;
    stream_rx_attr.out_media_config.bit_width = DEFAULT_BIT_WIDTH;
    stream_rx_attr.out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

    /* Device info */
    devices[HFPRX_IN].id = rx_config_port_in->device;
    devices[HFPRX_IN].config.sample_rate = btsco->sample_rate;
    devices[HFPRX_IN].config.bit_width = DEFAULT_BIT_WIDTH;
    devices[HFPRX_IN].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
    devices[HFPRX_OUT].id = rx_config_port_out->device;
    pa_strlcpy(devices[HFPRX_OUT].custom_config.custom_key, HFPRX_OUT_PAL_CUSTOM_CONFIG_KEY,
            sizeof(devices[HFPRX_OUT].custom_config.custom_key));

    pa_log_debug("HFP-Rx source port config: device-id %d, sample_rate %u, "
            "channels %u, format %d, bw %d\n",
             devices[HFPRX_IN].id, devices[HFPRX_IN].config.sample_rate,
             devices[HFPRX_IN].config.ch_info.channels,
             devices[HFPRX_IN].config.aud_fmt_id, devices[HFPRX_IN].config.bit_width);

    /* RX stream */
    ret = pal_stream_open(&stream_rx_attr,
            no_of_devices, devices,
            0,
            NULL,
            NULL,
            0,
            &btsco->rx_stream_handle);
    if (ret != E_SUCCESS) {
        pa_log_error("HFP rx stream (BT SCO->Spkr) open failed, rc %d", ret);
        btsco->rx_stream_handle = NULL;
        return ret;
    }

    ret = pal_stream_start(btsco->rx_stream_handle);
    if (ret != E_SUCCESS) {
        pa_log_error("HFP rx stream (BT SCO->Spkr) start failed, rc %d", ret);
        pal_stream_close(btsco->rx_stream_handle);
        btsco->rx_stream_handle = NULL;
        return ret;
    }

    /* Channel info */
    pa_pal_channel_map_to_pal(&tx_config_port_out->default_map, &ch_info);
    stream_tx_attr.in_media_config.ch_info = ch_info;
    stream_tx_attr.out_media_config.ch_info = ch_info;
    devices[HFPTX_OUT].config.ch_info = ch_info;

    /* Stream info */
    stream_tx_attr.type = PAL_STREAM_LOOPBACK;
    stream_tx_attr.info.opt_stream_info.loopback_type = PAL_STREAM_LOOPBACK_HFP_TX;
    stream_tx_attr.direction = PAL_AUDIO_INPUT_OUTPUT;
    stream_tx_attr.in_media_config.sample_rate = btsco->sample_rate;
    stream_tx_attr.in_media_config.bit_width = DEFAULT_BIT_WIDTH;
    stream_tx_attr.in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
    stream_tx_attr.out_media_config.sample_rate = tx_config_port_in->default_spec.rate;
    stream_tx_attr.out_media_config.bit_width = DEFAULT_BIT_WIDTH;
    stream_tx_attr.out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

    /* Device info */
    devices[HFPTX_OUT].id = tx_config_port_out->device;
    devices[HFPTX_OUT].config.sample_rate = btsco->sample_rate;
    devices[HFPTX_OUT].config.bit_width = DEFAULT_BIT_WIDTH;
    devices[HFPTX_OUT].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
    devices[HFPTX_IN].id = tx_config_port_in->device;

    pa_log_debug("HFP-Tx sink port config: id %d, sample_rate %u, "
            "channels %u, format %d, bw %d\n",
             devices[HFPTX_OUT].id, devices[HFPTX_OUT].config.sample_rate,
             devices[HFPTX_OUT].config.ch_info.channels,
             devices[HFPTX_OUT].config.aud_fmt_id, devices[HFPTX_OUT].config.bit_width);

    /* TX stream */
    ret = pal_stream_open(&stream_tx_attr,
            no_of_devices, devices,
            0,
            NULL,
            NULL,
            0,
            &btsco->tx_stream_handle);
    if (ret != E_SUCCESS) {
        pa_log_error("HFP tx stream (Mic->BT SCO) open failed, rc %d", ret);
        btsco->tx_stream_handle = NULL;
        pal_stream_stop(btsco->rx_stream_handle);
        pal_stream_close(btsco->rx_stream_handle);
        btsco->rx_stream_handle = NULL;
        return ret;
    }

    ret = pal_stream_start(btsco->tx_stream_handle);
    if (ret != E_SUCCESS) {
        pa_log_error("HFP tx stream (Mic->BT SCO) start failed, rc %d", ret);
        pal_stream_close(btsco->tx_stream_handle);
        pal_stream_stop(btsco->rx_stream_handle);
        pal_stream_close(btsco->rx_stream_handle);
        btsco->tx_stream_handle = NULL;
        btsco->rx_stream_handle = NULL;
        return ret;
    }

    btsco->is_running = true;

    /* Set default volume */
    pa_pal_set_volume(btsco->rx_stream_handle, rx_config_port_in->default_map.channels,
            btsco->rx_volume);
    pa_pal_set_volume(btsco->tx_stream_handle, tx_config_port_out->default_map.channels,
            btsco->tx_volume);

    pa_log_debug("%s Exit", __func__);

    return ret;
}

int stop_hfp(btsco_t *btsco)
{
    int ret = E_SUCCESS;

    pa_assert(btsco);

    pa_log_debug("%s Enter", __func__);

    if (!btsco->is_running) {
        pa_log_error("Usecase not active. Failed to stop!!!");
        return E_FAILURE;
    }

    btsco->is_running = false;
    if (btsco->rx_stream_handle) {
        ret = pal_stream_stop(btsco->rx_stream_handle);
        if (ret != E_SUCCESS) {
            pa_log_error("pal stream stop failed for rx_stream_handle");
            return ret;
        }
        ret = pal_stream_close(btsco->rx_stream_handle);
        if (ret != E_SUCCESS) {
            pa_log_error("pal stream close failed for rx_stream_handle");
            return ret;
        }
        btsco->rx_stream_handle = NULL;
    }
    if (btsco->tx_stream_handle) {
        ret = pal_stream_stop(btsco->tx_stream_handle);
        if (ret != E_SUCCESS) {
            pa_log_error("pal stream stop failed for tx_stream_handle");
            return ret;
        }
        ret = pal_stream_close(btsco->tx_stream_handle);
        if (ret != E_SUCCESS) {
            pa_log_error("pal stream close failed for tx_stream_handle");
            return ret;
        }
        btsco->tx_stream_handle = NULL;
    }

    pa_log_debug("%s Exit", __func__);

    return ret;
}

void deinit_btsco(btsco_t *btsco, pa_pal_loopback_config **loopback_config)
{
    pa_pal_card_port_config *config_port_in = NULL;
    pa_pal_card_port_config *config_port_out = NULL;

    pa_assert(loopback_config);

    if (!btsco) {
        pa_log_debug("%s: No active btsco connection", __func__);
        return;
    }

    if (btsco->is_running) {
        stop_hfp(btsco);
    }

    config_port_in = pa_hashmap_first(loopback_config[LB_PROF_HFP_RX]->in_ports);
    config_port_out = pa_hashmap_first(loopback_config[LB_PROF_HFP_TX]->out_ports);

    if (set_btsco_params(btsco, PAL_PARAM_ID_BT_SCO, false)) {
        pa_log_error("%s: set_params failed for btsco", __func__);
    }

    if (config_port_in && pa_pal_set_device_connection_state(config_port_in->device,
                false)) {
        pa_log_error("%s: set_device_connection failed for pal device %d", __func__,
                config_port_in->device);
    }
    if (config_port_out && pa_pal_set_device_connection_state(config_port_out->device,
                false)) {
        pa_log_error("%s: set_device_connection failed for pal device %d", __func__,
                config_port_out->device);
    }

    free(btsco);
}

