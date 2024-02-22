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

