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

#ifndef fooqahwjackformathfoo
#define fooqahwjackformathfoo

typedef enum {
    PA_QAHW_JACK_INPUT_MODE_PCM = 0,
    PA_QAHW_JACK_INPUT_MODE_COMPRESS = 1,
} pa_qahw_jack_input_mode_t;

typedef struct pa_qahw_jack_config {
    uint32_t sample_rate;
    uint32_t bitwidth;
    uint32_t channels;
    uint32_t layout;
    uint32_t channel_allocation;
    pa_qahw_jack_input_mode_t mode;
} pa_qahw_jack_config_t;

bool pa_qahw_hdmi_jack_get_config(pa_qahw_jack_config_t *curr_config);

#endif

