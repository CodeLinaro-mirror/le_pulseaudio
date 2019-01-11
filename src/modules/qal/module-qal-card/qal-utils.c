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
    uint32_t qal_channel_map_position;
} pa_qal_util_pa_qal_channel_map;

typedef struct {
    char *port_name;
    qal_device_id_t qal_device;
    char *qal_device_name;
} pa_qal_util_port_to_qal_device_mapping;

pa_qal_util_port_to_qal_device_mapping port_to_qal_device[] = {
    { (char*)"speaker",          QAL_DEVICE_OUT_SPEAKER,          (char *)"QAL_DEVICE_OUT_SPEAKER" },
    { (char *)"builtin-mic",     QAL_DEVICE_IN_SPEAKER_MIC,       (char *)"QAL_DEVICE_IN_SPEAKER_MIC" },
};

qal_device_id_t pa_qal_util_device_name_to_enum(const char *device_name) {
    uint32_t count;
    qal_device_id_t device = QAL_DEVICE_NONE;

    pa_assert(device_name);

    for (count = 0; count < ARRAY_SIZE(port_to_qal_device); count++) {
        if (pa_streq(device_name, port_to_qal_device[count].qal_device_name)) {
            device = port_to_qal_device[count].qal_device;
            break;
        }
    }

    pa_log_debug("%s: device_name %s qal device %u", __func__, device_name, device);

    return device;
}

static pa_qal_util_pa_qal_channel_map pa_qal_channel_map[] = {
    { PA_CHANNEL_POSITION_MONO, QAL_CHMAP_CHANNEL_MS },
    { PA_CHANNEL_POSITION_FRONT_LEFT , QAL_CHMAP_CHANNEL_FL },
    { PA_CHANNEL_POSITION_FRONT_RIGHT , QAL_CHMAP_CHANNEL_FR },
    { PA_CHANNEL_POSITION_FRONT_CENTER, QAL_CHMAP_CHANNEL_C },
    { PA_CHANNEL_POSITION_SIDE_LEFT, QAL_CHMAP_CHANNEL_LS },
    { PA_CHANNEL_POSITION_SIDE_RIGHT, QAL_CHMAP_CHANNEL_RS },
    { PA_CHANNEL_POSITION_LFE, QAL_CHMAP_CHANNEL_LFE },
    { PA_CHANNEL_POSITION_REAR_CENTER, QAL_CHMAP_CHANNEL_RC },
    { PA_CHANNEL_POSITION_REAR_LEFT, QAL_CHMAP_CHANNEL_LB },
    { PA_CHANNEL_POSITION_REAR_RIGHT, QAL_CHMAP_CHANNEL_RB },
    { PA_CHANNEL_POSITION_TOP_CENTER, QAL_CHMAP_CHANNEL_TS },
    { PA_CHANNEL_POSITION_TOP_FRONT_CENTER, QAL_CHMAP_CHANNEL_TFC },
    { PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER, QAL_CHMAP_CHANNEL_FLC },
    { PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER, QAL_CHMAP_CHANNEL_FRC },
    { PA_CHANNEL_POSITION_SIDE_LEFT, QAL_CHMAP_CHANNEL_SL },
    { PA_CHANNEL_POSITION_SIDE_RIGHT, QAL_CHMAP_CHANNEL_SR },
    { PA_CHANNEL_POSITION_TOP_FRONT_LEFT, QAL_CHMAP_CHANNEL_TFL },
    { PA_CHANNEL_POSITION_TOP_FRONT_RIGHT, QAL_CHMAP_CHANNEL_TFR },
    { PA_CHANNEL_POSITION_TOP_CENTER, QAL_CHMAP_CHANNEL_TC },
    { PA_CHANNEL_POSITION_TOP_REAR_LEFT, QAL_CHMAP_CHANNEL_TBL },
    { PA_CHANNEL_POSITION_TOP_REAR_RIGHT, QAL_CHMAP_CHANNEL_TBR },
    { PA_CHANNEL_POSITION_TOP_REAR_CENTER, QAL_CHMAP_CHANNEL_TBC },
    { PA_CHANNEL_POSITION_FRONT_LEFT_WIDE, QAL_CHMAP_CHANNEL_LW },
    { PA_CHANNEL_POSITION_FRONT_RIGHT_WIDE, QAL_CHMAP_CHANNEL_RW },
    { PA_CHANNEL_POSITION_TOP_SIDE_LEFT, QAL_CHMAP_CHANNEL_TSL },
    { PA_CHANNEL_POSITION_TOP_SIDE_RIGHT, QAL_CHMAP_CHANNEL_TSR }
};

bool pa_qal_channel_map_to_qal(pa_channel_map *pa_map, struct qal_channel_info *qal_map) {
    uint32_t channels;
    uint32_t count;
    bool present = false;

    pa_assert(pa_map);
    pa_assert(qal_map);

    qal_map->channels = pa_map->channels;
    for (channels = 0; channels < pa_map->channels; channels++) {
        present = false;
        for (count = 0; count < ARRAY_SIZE(pa_qal_channel_map); count++) {
            if (pa_map->map[channels] == pa_qal_channel_map[count].pa_channel_map_position) {
                qal_map->ch_map[channels] = pa_qal_channel_map[count].qal_channel_map_position;
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