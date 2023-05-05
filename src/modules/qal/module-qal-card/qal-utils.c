/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
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
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-format.h>
#include <pulse/channelmap.h>

#include "qal-utils.h"

typedef struct{
    pa_channel_position_t pa_channel_map_position;
    uint32_t pal_channel_map_position;
} pa_pal_util_pa_pal_channel_map;

typedef struct {
    char *port_name;
    pal_device_id_t pal_device;
    char *pal_device_name;
} pa_pal_util_port_to_pal_device_mapping;

pa_pal_util_port_to_pal_device_mapping port_to_pal_device[] = {
    { (char *)"speaker",          PAL_DEVICE_OUT_SPEAKER,          (char *)"PAL_DEVICE_OUT_SPEAKER" },
    { (char *)"lineout",          PAL_DEVICE_OUT_LINE,             (char *)"PAL_DEVICE_OUT_LINE" },
    { (char *)"headset",          PAL_DEVICE_OUT_WIRED_HEADSET,    (char *)"PAL_DEVICE_OUT_WIRED_HEADSET" },
    { (char *)"headphone",        PAL_DEVICE_OUT_WIRED_HEADPHONE,  (char *)"PAL_DEVICE_OUT_WIRED_HEADPHONE" },
    { (char *)"bta2dp-out",       PAL_DEVICE_OUT_BLUETOOTH_A2DP,   (char *)"PAL_DEVICE_OUT_BLUETOOTH_A2DP" },
    { (char *)"builtin-mic",      PAL_DEVICE_IN_HANDSET_MIC,       (char *)"PAL_DEVICE_IN_HANDSET_MIC" },
    { (char *)"speaker-mic",      PAL_DEVICE_IN_SPEAKER_MIC,       (char *)"PAL_DEVICE_IN_SPEAKER_MIC" },
    { (char *)"linein",           PAL_DEVICE_IN_LINE,              (char *)"PAL_DEVICE_IN_LINE" },
};

pal_device_id_t pa_pal_util_device_name_to_enum(const char *device_name) {
    uint32_t count;
    pal_device_id_t device = PAL_DEVICE_NONE;

    pa_assert(device_name);

    for (count = 0; count < ARRAY_SIZE(port_to_pal_device); count++) {
        if (pa_streq(device_name, port_to_pal_device[count].pal_device_name)) {
            device = port_to_pal_device[count].pal_device;
            break;
        }
    }

    pa_log_debug("%s: device_name %s pal device %u", __func__, device_name, device);

    return device;
}

int pa_pal_util_set_pal_metadata_from_pa_format(const pa_format_info *format) {
    int rc = 0;
    char *format_flag;

    pa_assert(format);

    switch (format->encoding) {
        default:
           break;
    }

    return rc;
}

/* With reference to the translation table from "Dolby Atmos to Sound Bar Product
 * System Development Manual" */
pa_channel_map* pa_pal_util_channel_map_init(pa_channel_map *m, unsigned channels) {
    pa_assert(m);
    pa_assert(pa_channels_valid(channels));

    pa_channel_map_init(m);

    m->channels = (uint8_t) channels;

    switch (channels) {
        case 1:
            m->map[0] = PA_CHANNEL_POSITION_MONO;
            return m;
        case 7:
            m->map[6] = PA_CHANNEL_POSITION_REAR_CENTER;
            /* Fall through */
        case 6:
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[3] = PA_CHANNEL_POSITION_LFE;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            return m;
        case 5:
            m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[3] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            /* Fall through */
        case 2:
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            return m;
        case 8:
            m->map[3] = PA_CHANNEL_POSITION_LFE;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_REAR_LEFT;
            m->map[7] = PA_CHANNEL_POSITION_REAR_RIGHT;
            /* Fall through */
        case 3:
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
            return m;
        case 4:
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[2] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[3] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            return m;
        default:
            return NULL;
    }
}

static pa_pal_util_pa_pal_channel_map pa_pal_channel_map[] = {
    { PA_CHANNEL_POSITION_MONO, PAL_CHMAP_CHANNEL_MS },
    { PA_CHANNEL_POSITION_FRONT_LEFT , PAL_CHMAP_CHANNEL_FL },
    { PA_CHANNEL_POSITION_FRONT_RIGHT , PAL_CHMAP_CHANNEL_FR },
    { PA_CHANNEL_POSITION_FRONT_CENTER, PAL_CHMAP_CHANNEL_C },
    { PA_CHANNEL_POSITION_SIDE_LEFT, PAL_CHMAP_CHANNEL_LS },
    { PA_CHANNEL_POSITION_SIDE_RIGHT, PAL_CHMAP_CHANNEL_RS },
    { PA_CHANNEL_POSITION_LFE, PAL_CHMAP_CHANNEL_LFE },
    { PA_CHANNEL_POSITION_REAR_CENTER, PAL_CHMAP_CHANNEL_RC },
    { PA_CHANNEL_POSITION_REAR_LEFT, PAL_CHMAP_CHANNEL_LB },
    { PA_CHANNEL_POSITION_REAR_RIGHT, PAL_CHMAP_CHANNEL_RB },
    { PA_CHANNEL_POSITION_TOP_CENTER, PAL_CHMAP_CHANNEL_TS },
    { PA_CHANNEL_POSITION_TOP_FRONT_CENTER, PAL_CHMAP_CHANNEL_TFC },
    { PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER, PAL_CHMAP_CHANNEL_FLC },
    { PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER, PAL_CHMAP_CHANNEL_FRC },
    { PA_CHANNEL_POSITION_SIDE_LEFT, PAL_CHMAP_CHANNEL_SL },
    { PA_CHANNEL_POSITION_SIDE_RIGHT, PAL_CHMAP_CHANNEL_SR },
    { PA_CHANNEL_POSITION_TOP_FRONT_LEFT, PAL_CHMAP_CHANNEL_TFL },
    { PA_CHANNEL_POSITION_TOP_FRONT_RIGHT, PAL_CHMAP_CHANNEL_TFR },
    { PA_CHANNEL_POSITION_TOP_CENTER, PAL_CHMAP_CHANNEL_TC },
    { PA_CHANNEL_POSITION_TOP_REAR_LEFT, PAL_CHMAP_CHANNEL_TBL },
    { PA_CHANNEL_POSITION_TOP_REAR_RIGHT, PAL_CHMAP_CHANNEL_TBR },
    { PA_CHANNEL_POSITION_TOP_REAR_CENTER, PAL_CHMAP_CHANNEL_TBC }
};

pal_audio_fmt_t pa_pal_util_get_pal_format_from_pa_encoding(pa_encoding_t pa_format, pal_snd_dec_t *pal_snd_dec) {
    pal_audio_fmt_t pal_format = 0;

    switch (pa_format) {
        case PA_ENCODING_ANY:
            pal_format = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
        case PA_ENCODING_PCM:
            pal_format = PAL_AUDIO_FMT_PCM_S16_LE;
            break;
        default:
            pa_log_error("PA format encoding not supported in PAL\n");
            break;
    }

    return pal_format;
}

uint32_t pa_pal_get_channel_count(pa_channel_map *pa_map) {
    pa_assert(pa_map);
    return pa_map->channels;
}

bool pa_pal_channel_map_to_pal(pa_channel_map *pa_map, struct pal_channel_info *pal_map) {
    uint32_t channels;
    uint32_t count;
    bool present = false;

    pa_assert(pa_map);
    pa_assert(pal_map);

    pal_map->channels = pa_map->channels;
    for (channels = 0; channels < pa_map->channels; channels++) {
        present = false;
        for (count = 0; count < ARRAY_SIZE(pa_pal_channel_map); count++) {
            if (pa_map->map[channels] == pa_pal_channel_map[count].pa_channel_map_position) {
                pal_map->ch_map[channels] = pa_pal_channel_map[count].pal_channel_map_position;
                present = true;
                break;
            }
        }

        if (!present) {
            pa_log_error("%s: unsupported pa channel position %x", __func__, pa_map->map[channels]);
            return false;
        }
    }

    return true;
}
