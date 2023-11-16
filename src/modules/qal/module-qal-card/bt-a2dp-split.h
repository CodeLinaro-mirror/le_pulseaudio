/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __BTA2DP_SPLIT_H__
#define __BTA2DP_SPLIT_H__

#define BTSINK_IN   0
#define BTSINK_OUT  1

/* usecase structs */
typedef struct btsink_module {
    bool is_running;
    bool is_mute;
    double volume;
    pal_stream_handle_t* stream_handle;
} btsink_t;

int init_btsink(btsink_t **btsink, pa_pal_loopback_config *loopback_conf);
int start_btsink(btsink_t *btsink, pa_pal_loopback_config *loopback);
int stop_btsink(btsink_t *btsink);
void deinit_btsink(btsink_t *btsink, pa_pal_loopback_config *loopback_conf);

#endif

