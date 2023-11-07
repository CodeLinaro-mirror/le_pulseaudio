/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
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
#include <errno.h>
#include <math.h>

#include "pal-utils.h"

/* Supports session ID allocation upto 16 concurrent sessions */
static unsigned short bit_pool = 0;

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
    { (char *)"speaker",          PAL_DEVICE_OUT_SPEAKER,               (char *)"PAL_DEVICE_OUT_SPEAKER" },
    { (char *)"lineout",          PAL_DEVICE_OUT_LINE,                  (char *)"PAL_DEVICE_OUT_LINE" },
    { (char *)"headset",          PAL_DEVICE_OUT_WIRED_HEADSET,         (char *)"PAL_DEVICE_OUT_WIRED_HEADSET" },
    { (char *)"headphone",        PAL_DEVICE_OUT_WIRED_HEADPHONE,       (char *)"PAL_DEVICE_OUT_WIRED_HEADPHONE" },
    { (char *)"bta2dp-out",       PAL_DEVICE_OUT_BLUETOOTH_A2DP,        (char *)"PAL_DEVICE_OUT_BLUETOOTH_A2DP" },
    { (char *)"builtin-mic",      PAL_DEVICE_IN_HANDSET_MIC,            (char *)"PAL_DEVICE_IN_HANDSET_MIC" },
    { (char *)"speaker-mic",      PAL_DEVICE_IN_SPEAKER_MIC,            (char *)"PAL_DEVICE_IN_SPEAKER_MIC" },
    { (char *)"linein",           PAL_DEVICE_IN_LINE,                   (char *)"PAL_DEVICE_IN_LINE" },
    { (char *)"headset-mic",      PAL_DEVICE_IN_WIRED_HEADSET,          (char *)"PAL_DEVICE_IN_WIRED_HEADSET" },
    { (char *)"bta2dp-in",        PAL_DEVICE_IN_BLUETOOTH_A2DP,         (char *)"PAL_DEVICE_IN_BLUETOOTH_A2DP" },
    { (char *)"btsco-in",         PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET,  (char *)"PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET" },
    { (char *)"btsco-out",        PAL_DEVICE_OUT_BLUETOOTH_SCO,         (char *)"PAL_DEVICE_OUT_BLUETOOTH_SCO" },
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

int pa_pal_set_volume(pal_stream_handle_t *handle, uint32_t num_channels, float value)
{
    int32_t vol = 0, ret = 0;
    struct pal_volume_data *pal_volume = NULL;

    pa_log_debug("%s: volume to be set (%f)\n", __func__, value);

    if (!handle) {
        pa_log_debug("%s: Usecase is not active yet !!\n", __func__);
        return -EINVAL;
    }

    if (value < 0.0) {
        pa_log_debug("(%f) Under 0.0, assuming 0.0\n", value);
        value = 0.0;
    } else {
        value = ((value > 15.000000) ? 1.0 : (value / 15));
        pa_log_debug("Volume brought with in range (%f)\n", value);
    }
    vol  = lrint((value * 0x2000) + 0.5);

    pa_log_debug("Setting volume to %d \n", vol);

    pal_volume = (struct pal_volume_data *)calloc(1, sizeof(struct pal_volume_data)
            + (sizeof(struct pal_channel_vol_kv) * num_channels));
    if (!pal_volume)
        return -ENOMEM;

    pal_volume->no_of_volpair = num_channels;
    for (int i = 0; i < num_channels; i++) {
        pal_volume->volume_pair[i].channel_mask = 0x03;
        pal_volume->volume_pair[i].vol = value;
    }
    ret = pal_stream_set_volume(handle, pal_volume);
    if (ret)
        pa_log_error("%s failed: %d \n", __func__, ret);

    free(pal_volume);
    pa_log_debug("%s: exit", __func__);

    return ret;
}

int pa_pal_set_device_connection_state(pal_device_id_t pal_dev_id, bool connection_state)
{
    int ret = 0;
    pal_param_device_connection_t param_device_connection;

    param_device_connection.id = pal_dev_id;
    param_device_connection.connection_state = connection_state;

    ret = pal_set_param(PAL_PARAM_ID_DEVICE_CONNECTION,
            (void*)&param_device_connection,
            sizeof(pal_param_device_connection_t));
    if (ret != 0) {
        pa_log_error("Set PAL_PARAM_ID_DEVICE_CONNECTION for %d failed", param_device_connection.id);
    }

    return ret;
}

unsigned short pa_pal_alloc_session_id(void)
{
    unsigned short mask = 1;
    unsigned short session_id_avail = 1;

    if (bit_pool >= BITPOOL_MAX) {
        pa_log_error("bit_pool is already full\n");
        return BITPOOL_MAX;
    }

    while (bit_pool & mask)
    {
        mask <<= 1;
        session_id_avail++;
    }
    bit_pool |= mask;

    pa_log_debug("Allocating session_id %hu", session_id_avail);
    return session_id_avail;
}

void pa_pal_release_session_id(unsigned short session_id)
{
    unsigned short mask = 1;

    if (session_id > BITPOOL_MAX_CONC_SESSION_IDS || session_id < 0) {
        pa_log_error("%hu is invalid session id\n", session_id);
        return;
    }

    mask = BITPOOL_MAX ^ (mask << (session_id - 1));
    bit_pool &= mask;

    pa_log_debug("Released session_id %hu\n", session_id);
    return;
}
