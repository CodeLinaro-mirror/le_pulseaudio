/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __HFP_H__
#define __HFP_H__

/* Loopback profiles for BTsco usecase */
#define LB_PROF_HFP_RX  0
#define LB_PROF_HFP_TX  1

/* hfp rx/tx in/out device indexes */
#define HFPRX_IN    0
#define HFPRX_OUT   1
#define HFPTX_IN    0
#define HFPTX_OUT   1

typedef struct btsco_module {
    bool is_running;
    bool rx_mute;
    bool tx_mute;
    double rx_volume;
    double tx_volume;
    unsigned int sample_rate;
    pal_stream_handle_t *rx_stream_handle;
    pal_stream_handle_t *tx_stream_handle;
} btsco_t;

int init_btsco(btsco_t **btsco, pa_pal_loopback_config **loopback_config);
int start_hfp(btsco_t *btsco, pa_pal_loopback_config **loopback);
int stop_hfp(btsco_t *btsco);
void deinit_btsco(btsco_t *btsco, pa_pal_loopback_config **loopback_config);

#endif
