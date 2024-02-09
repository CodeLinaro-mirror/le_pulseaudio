/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
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

#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-format.h>
#include <pulse/channelmap.h>
#include <errno.h>
#include <math.h>

#include "qal-utils.h"

#define PA_PAL_SINK_PROP_FORMAT_FLAG    "stream-format"

#define AAC_AOT_PS    29

typedef struct{
    pa_channel_position_t pa_channel_map_position;
    uint32_t pal_channel_map_position;
} pa_pal_util_pa_pal_channel_map;

typedef struct {
    pal_audio_fmt_t stream_format;
} pa_pal_util_aac_compress_metadata;

typedef union {
    pa_pal_util_aac_compress_metadata aac;
} pa_pal_util_compress_metadata;

pa_pal_util_compress_metadata compress_metadata;

typedef struct {
    char *port_name;
    pal_device_id_t pal_device;
    char *pal_device_name;
} pa_pal_util_port_to_pal_device_mapping;

typedef struct {
    pa_pal_jack_type_t jack_type;
    char *port_name;
} pa_pal_util_jack_type_to_port_name;

pa_pal_util_port_to_pal_device_mapping port_to_pal_device[] = {
    { (char *)"speaker",          PAL_DEVICE_OUT_SPEAKER,               (char *)"PAL_DEVICE_OUT_SPEAKER" },
    { (char *)"lineout",          PAL_DEVICE_OUT_LINE,                  (char *)"PAL_DEVICE_OUT_LINE" },
    { (char *)"headset",          PAL_DEVICE_OUT_WIRED_HEADSET,         (char *)"PAL_DEVICE_OUT_WIRED_HEADSET" },
    { (char *)"headphone",        PAL_DEVICE_OUT_WIRED_HEADPHONE,       (char *)"PAL_DEVICE_OUT_WIRED_HEADPHONE" },
    { (char *)"bta2dp-out",       PAL_DEVICE_OUT_BLUETOOTH_A2DP,        (char *)"PAL_DEVICE_OUT_BLUETOOTH_A2DP" },
    { (char *)"builtin-mic",      PAL_DEVICE_IN_HANDSET_MIC,            (char *)"PAL_DEVICE_IN_HANDSET_MIC" },
    { (char *)"speaker-mic",      PAL_DEVICE_IN_SPEAKER_MIC,            (char *)"PAL_DEVICE_IN_SPEAKER_MIC" },
    { (char *)"headset-mic",      PAL_DEVICE_IN_WIRED_HEADSET,          (char *)"PAL_DEVICE_IN_WIRED_HEADSET" },
    { (char *)"linein",           PAL_DEVICE_IN_LINE,                   (char *)"PAL_DEVICE_IN_LINE" },
    { (char *)"hdmi-out",         PAL_DEVICE_OUT_AUX_DIGITAL,           (char *)"PAL_DEVICE_OUT_AUX_DIGITAL" },
    { (char *)"bta2dp-in",        PAL_DEVICE_IN_BLUETOOTH_A2DP,         (char *)"PAL_DEVICE_IN_BLUETOOTH_A2DP" },
    { (char *)"btsco-in",         PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET,  (char *)"PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET" },
    { (char *)"btsco-out",        PAL_DEVICE_OUT_BLUETOOTH_SCO,         (char *)"PAL_DEVICE_OUT_BLUETOOTH_SCO" },
};

pa_pal_util_jack_type_to_port_name jack_type_to_port_name[] = {
    { PA_PAL_JACK_TYPE_WIRED_HEADSET, (char*)"headset" },
    { PA_PAL_JACK_TYPE_WIRED_HEADSET_BUTTONS, (char*)"headset-mic" },
    { PA_PAL_JACK_TYPE_WIRED_HEADPHONE, (char*)"headphone" },
    { PA_PAL_JACK_TYPE_LINEOUT, (char*)"lineout"},
    { PA_PAL_JACK_TYPE_HDMI_IN, (char*)"hdmi-in" },
    { PA_PAL_JACK_TYPE_BTA2DP_OUT, (char*)"bta2dp-out" },
    { PA_PAL_JACK_TYPE_BTA2DP_IN, (char*)"bta2dp-in" },
    { PA_PAL_JACK_TYPE_HDMI_ARC, (char *)"hdmi-arc"},
    { PA_PAL_JACK_TYPE_SPDIF, (char *)"spdif-in"},
    { PA_PAL_JACK_TYPE_BTSCO_IN, (char *)"btsco-in"},
    { PA_PAL_JACK_TYPE_BTSCO_OUT, (char *)"btsco-out"},
    { PA_PAL_JACK_TYPE_HDMI_OUT, (char *)"hdmi-out"},
    { PA_PAL_JACK_TYPE_SPDIF_OUT_OPTICAL, (char *)"spdif-out-optical"},
    { PA_PAL_JACK_TYPE_SPDIF_OUT_COAXIAL, (char *)"spdif-out-coaxial"},
};

static pa_channel_position_t pa_pal_be_channel_map[] = {
    PAL_PCM_CHANNEL_FL,
    PAL_PCM_CHANNEL_FR,
    PAL_PCM_CHANNEL_LFE,
    PAL_PCM_CHANNEL_FC,
    PAL_PCM_CHANNEL_LS,
    PAL_PCM_CHANNEL_RS,
    PAL_PCM_CHANNEL_LB,
    PAL_PCM_CHANNEL_RB
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

pal_device_id_t pa_pal_util_port_name_to_enum(const char *port_name) {
    uint32_t count;
    pal_device_id_t device = PAL_DEVICE_NONE;

    pa_assert(port_name);

    for (count = 0; count < ARRAY_SIZE(port_to_pal_device); count++) {
        if (pa_streq(port_name, port_to_pal_device[count].port_name)) {
            device = port_to_pal_device[count].pal_device;
            break;
        }
    }

    pa_log_debug("%s: device_name %s pal device %u", __func__, port_name, device);

    return device;
}

int pa_pal_util_set_pal_metadata_from_pa_format(const pa_format_info *format) {
    int rc = 0;
    char *stream_format;

    pa_assert(format);

    switch (format->encoding) {
        case PA_ENCODING_AAC:
            rc = pa_format_info_get_prop_string(format,
                 PA_PAL_SINK_PROP_FORMAT_FLAG, &stream_format);
            if (rc) {
                pa_log_error("%s: Failed to obtain AAC stream format", __func__);
            } else {
               if (pa_streq(stream_format, "adts")) {
                   pa_log_debug("%s: adts format", __func__);
                   compress_metadata.aac.stream_format = PAL_AUDIO_FMT_AAC_ADTS;
               } else {
                   pa_log_debug("%s: raw format", __func__);
                   compress_metadata.aac.stream_format = PAL_AUDIO_FMT_AAC;
               }

               pa_xfree(stream_format);
            }

            break;

        case PA_ENCODING_MPEG:
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
    { PA_CHANNEL_POSITION_TOP_REAR_CENTER, PAL_CHMAP_CHANNEL_TBC },
    { PA_CHANNEL_POSITION_AUX0, PAL_CHMAP_CHANNEL_RLC },
    { PA_CHANNEL_POSITION_AUX1, PAL_CHMAP_CHANNEL_RRC }
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
        case PA_ENCODING_MPEG:
            pal_format = PAL_AUDIO_FMT_MP3;
            break;
        case PA_ENCODING_AAC:
            pal_format = compress_metadata.aac.stream_format;
            pal_snd_dec->aac_dec.audio_obj_type = AAC_AOT_PS;
            pal_snd_dec->aac_dec.pce_bits_size = 0;
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

pa_pal_jack_type_t pa_pal_util_get_jack_type_from_port_name(const char *port_name) {
    uint32_t count;

    for (count = 0; count < ARRAY_SIZE(jack_type_to_port_name); count++) {
        if (pa_streq(jack_type_to_port_name[count].port_name, port_name))
            return jack_type_to_port_name[count].jack_type;
    }

    return PA_PAL_JACK_TYPE_INVALID;
}

const char* pa_pal_util_get_port_name_from_jack_type(pa_pal_jack_type_t jack_type) {
    uint32_t count;

    for (count = 0; count < ARRAY_SIZE(jack_type_to_port_name); count++) {
        if (jack_type_to_port_name[count].jack_type == jack_type)
            return jack_type_to_port_name[count].port_name;
    }

    return NULL;
}

/*
 * This Function will remove the invalid channels in case of invalid channels passed
 * in hdmi non-2ch and non-ch usecases ultimately to derive map w.r.t be channel map.
 */
pa_channel_map pa_pal_map_remove_invalid_channels(pa_channel_map *def_map_with_inval_ch) {
    uint32_t i=0, j=0;
    pa_channel_map pa_map;
    pa_assert(def_map_with_inval_ch);

    while(i < ARRAY_SIZE(pa_pal_be_channel_map)) {
        if (def_map_with_inval_ch->map[i] != PA_CHANNEL_POSITION_INVALID) {
            pa_map.map[j] = def_map_with_inval_ch->map[i];
            j++;
        }
        i++;
    }
    pa_map.channels = def_map_with_inval_ch->channels;
    return pa_map;
}

void pa_pal_util_get_jack_sys_path(pa_pal_card_port_config *config_port, pa_pal_jack_in_config *jack_in_config) {
    pa_assert(config_port);
    pa_assert(jack_in_config);

    if (config_port->state_node_path)
        jack_in_config->jack_sys_path.audio_state = config_port->state_node_path;

    if (config_port->sample_format_node_path)
        jack_in_config->jack_sys_path.audio_format = config_port->sample_format_node_path;

    if (config_port->sample_rate_node_path)
        jack_in_config->jack_sys_path.audio_rate = config_port->sample_rate_node_path;

    if (config_port->sample_layout_node_path)
        jack_in_config->jack_sys_path.audio_layout = config_port->sample_layout_node_path;

    if (config_port->sample_channel_node_path)
        jack_in_config->jack_sys_path.audio_channel = config_port->sample_channel_node_path;

    if (config_port->sample_channel_alloc_node_path)
        jack_in_config->jack_sys_path.audio_channel_alloc = config_port->sample_channel_alloc_node_path;

    if (config_port->linkon0_node_path)
        jack_in_config->jack_sys_path.linkon_0 = config_port->linkon0_node_path;

    if (config_port->poweron_node_path)
        jack_in_config->jack_sys_path.power_on = config_port->poweron_node_path;

    if (config_port->audio_path_node_path)
        jack_in_config->jack_sys_path.audio_path = config_port->audio_path_node_path;

    if (config_port->arc_enable_node_path)
        jack_in_config->jack_sys_path.arc_enable = config_port->arc_enable_node_path;

    if (config_port->earc_enable_node_path)
        jack_in_config->jack_sys_path.earc_enable = config_port->earc_enable_node_path;

    if (config_port->arc_state_node_path)
        jack_in_config->jack_sys_path.arc_audio_state = config_port->arc_state_node_path;

    if (config_port->arc_sample_format_node_path)
        jack_in_config->jack_sys_path.arc_audio_format = config_port->arc_sample_format_node_path;

    if (config_port->arc_sample_rate_node_path)
        jack_in_config->jack_sys_path.arc_audio_rate = config_port->arc_sample_rate_node_path;

    if (config_port->audio_preemph_node_path)
        jack_in_config->jack_sys_path.audio_preemph = config_port->audio_preemph_node_path;

    if (config_port->arc_audio_preemph_node_path)
        jack_in_config->jack_sys_path.arc_audio_preemph = config_port->arc_audio_preemph_node_path;

    if (config_port->dsd_rate_node_path)
        jack_in_config->jack_sys_path.dsd_rate = config_port->dsd_rate_node_path;

    if (config_port->hdmi_tx_state_path)
        jack_in_config->jack_sys_path.hdmi_tx_state = config_port->hdmi_tx_state_path;

    if (config_port->channel_status_path)
        jack_in_config->jack_sys_path.channel_status = config_port->channel_status_path;

}
