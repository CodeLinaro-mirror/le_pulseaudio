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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-format.h>
#include <pulse/channelmap.h>

#include "qahw-utils.h"

typedef struct{
    pa_channel_position_t pa_channel_map_position;
    uint32_t qahw_channel_map_position;
} pa_qahw_util_pa_qahw_channel_map;

typedef struct {
    pa_qahw_jack_type_t jack_type;
    char *port_name;
} pa_qahw_util_jack_type_to_port_name;

typedef struct {
    char *port_name;
    audio_devices_t qahw_device;
    char *qahw_device_name;
} pa_qahw_util_port_to_qahw_device_mapping;

pa_qahw_util_jack_type_to_port_name jack_type_to_port_name[] = {
    { PA_QAHW_JACK_TYPE_WIRED_HEADSET, (char*)"headset" },
    { PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS, (char*)"headset-mic" },
    { PA_QAHW_JACK_TYPE_WIRED_HEADPHONE, (char*)"headphone" },
    { PA_QAHW_JACK_TYPE_LINEOUT, (char*)"lineout"},
    { PA_QAHW_JACK_TYPE_HDMI, (char*)"hdmi-in" },
};

pa_qahw_util_port_to_qahw_device_mapping port_to_qahw_device[] = {
    { (char*)"speaker",          AUDIO_DEVICE_OUT_SPEAKER,          (char *)"AUDIO_DEVICE_OUT_SPEAKER" },
    { (char *)"headset",         AUDIO_DEVICE_OUT_WIRED_HEADSET,    (char *)"AUDIO_DEVICE_OUT_WIRED_HEADSET" },
    { (char*)"lineout",          AUDIO_DEVICE_OUT_LINE,             (char *)"AUDIO_DEVICE_OUT_LINE"},
    { (char*)"headphone",        AUDIO_DEVICE_OUT_WIRED_HEADPHONE,  (char *)"AUDIO_DEVICE_OUT_WIRED_HEADPHONE" },
    { (char *)"headset-mic",     AUDIO_DEVICE_IN_WIRED_HEADSET,     (char *)"AUDIO_DEVICE_IN_WIRED_HEADSET" },
    { (char *)"builtin-mic",     AUDIO_DEVICE_IN_BUILTIN_MIC,       (char *)"AUDIO_DEVICE_IN_BUILTIN_MIC" },
    { (char *)"hdmi-in",         AUDIO_DEVICE_IN_HDMI,              (char *)"AUDIO_DEVICE_IN_HDMI" },
    { (char *)"spdif-in",        AUDIO_DEVICE_IN_SPDIF ,            (char *)"AUDIO_DEVICE_IN_SPDIF" },
    { (char *)"linein",          AUDIO_DEVICE_IN_LINE,              (char *)"AUDIO_DEVICE_IN_LINE" },
};

audio_format_t pa_qahw_util_get_qahw_format_from_pa_sample(pa_sample_format_t format) {
    audio_format_t qahw_format;

    switch(format) {
        case PA_SAMPLE_S16LE:
            qahw_format = AUDIO_FORMAT_PCM_16_BIT;
            break;
        case PA_SAMPLE_S24LE:
            qahw_format = AUDIO_FORMAT_PCM_24_BIT_PACKED;
            break;
        case PA_SAMPLE_S32LE:
            qahw_format = AUDIO_FORMAT_PCM_32_BIT;
            break;
        default:
            qahw_format = AUDIO_FORMAT_INVALID;
            pa_log_error("Unsupported format %d",format);
    }

    return qahw_format;
}

const char* pa_qahw_util_get_port_name_from_jack_type(pa_qahw_jack_type_t jack_type) {
    uint32_t count;

    for (count = 0; count < ARRAY_SIZE(jack_type_to_port_name); count++) {
        if (jack_type_to_port_name[count].jack_type == jack_type)
            return jack_type_to_port_name[count].port_name;
    }

    return NULL;
}

pa_qahw_jack_type_t pa_qahw_util_get_jack_type_from_port_name(const char *port_name) {
    uint32_t count;

    for (count = 0; count < ARRAY_SIZE(jack_type_to_port_name); count++) {
        if (pa_streq(jack_type_to_port_name[count].port_name, port_name))
            return jack_type_to_port_name[count].jack_type;
    }

    return PA_QAHW_JACK_TYPE_INVALID;
}

audio_format_t pa_qahw_util_get_qahw_format_from_pa_encoding(pa_encoding_t pa_format) {
    audio_format_t qahw_format = AUDIO_FORMAT_INVALID;

    switch (pa_format) {
        case PA_ENCODING_ANY:
            qahw_format = AUDIO_FORMAT_DEFAULT;
            break;
        case PA_ENCODING_PCM:
            qahw_format = AUDIO_FORMAT_PCM_16_BIT;
            break;
        case PA_ENCODING_AC3_IEC61937:
            qahw_format = AUDIO_FORMAT_AC3;
            break;
        case PA_ENCODING_EAC3_IEC61937:
            qahw_format = AUDIO_FORMAT_E_AC3;
            break;
        case PA_ENCODING_TRUEHD_IEC61937:
            qahw_format = AUDIO_FORMAT_DOLBY_TRUEHD;
            break;
        case PA_ENCODING_UNKNOWN_IEC61937:
        case PA_ENCODING_UNKNOWN_4X_IEC61937:
        case PA_ENCODING_UNKNOWN_HBR_IEC61937:
            qahw_format = AUDIO_FORMAT_IEC61937;
            break;
        default:
            pa_log_error("PA format encoding not supported in QAHW\n");
            break;
    }

    return qahw_format;
}

audio_channel_mask_t pa_qahw_util_get_channel_mask_from_num_channels(unsigned int num_channels) {
    audio_channel_mask_t channel_mask = AUDIO_CHANNEL_INVALID;

    switch (num_channels) {
        case 1:
            channel_mask = AUDIO_CHANNEL_OUT_MONO;
            break;
        case 2:
            channel_mask = AUDIO_CHANNEL_OUT_STEREO;
            break;
        case 3:
            channel_mask = AUDIO_CHANNEL_OUT_2POINT1;
            break;
        case 4:
            channel_mask = AUDIO_CHANNEL_OUT_QUAD;
            break;
        case 5:
            channel_mask = AUDIO_CHANNEL_OUT_PENTA;
            break;
        case 6:
            channel_mask = AUDIO_CHANNEL_OUT_5POINT1;
            break;
        case 7:
            channel_mask = AUDIO_CHANNEL_OUT_6POINT1;
            break;
        case 8:
            channel_mask = AUDIO_CHANNEL_OUT_7POINT1;
            break;
        default:
            pa_log_error("Unsupported number of channels\n");
            break;
    }

    return channel_mask;
}

unsigned int pa_qahw_util_get_num_channels_from_channel_mask(audio_channel_mask_t channel_mask) {
    unsigned int num_channels = 0;

    switch (channel_mask) {
        case AUDIO_CHANNEL_OUT_MONO:
            num_channels = 1;
            break;
        case AUDIO_CHANNEL_OUT_STEREO:
            num_channels = 2;
            break;
        case AUDIO_CHANNEL_OUT_2POINT1:
            num_channels = 3;
            break;
        case AUDIO_CHANNEL_OUT_QUAD:
            num_channels = 4;
            break;
        case AUDIO_CHANNEL_OUT_PENTA:
            num_channels = 5;
            break;
        case AUDIO_CHANNEL_OUT_5POINT1:
            num_channels = 6;
            break;
        case AUDIO_CHANNEL_OUT_6POINT1:
            num_channels = 7;
            break;
        case AUDIO_CHANNEL_OUT_7POINT1:
            num_channels = 8;
            break;
        default:
            pa_log_error("Unsupported channel mask\n");
            break;
    }

    return num_channels;
}

pa_encoding_t pa_qahw_util_get_pa_encoding_from_qahw_format(audio_format_t qahw_format) {
    pa_encoding_t pa_format = PA_ENCODING_INVALID;

    switch (qahw_format) {
        case AUDIO_FORMAT_DEFAULT:
            pa_format = PA_ENCODING_ANY;
            break;
        case AUDIO_FORMAT_PCM_16_BIT:
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
        case AUDIO_FORMAT_PCM_32_BIT:
            pa_format = PA_ENCODING_PCM;
            break;
        case AUDIO_FORMAT_AC3:
            pa_format = PA_ENCODING_AC3_IEC61937;
            break;
        case AUDIO_FORMAT_E_AC3:
            pa_format = PA_ENCODING_EAC3_IEC61937;
            break;
        case AUDIO_FORMAT_DOLBY_TRUEHD:
            pa_format = PA_ENCODING_TRUEHD_IEC61937;
            break;
        default:
            pa_log_debug("QAHW format not supported\n");
            break;
    }

    return pa_format;
}

const char* pa_qahw_util_audio_device_to_port_name(audio_devices_t audio_device, pa_hashmap *ports) {
    audio_devices_t *p_audio_device;
    pa_device_port *p;
    void *state = NULL;
    const void *key;

    pa_assert(ports);

    p = pa_hashmap_iterate(ports, &state, &key);
    while (p) {
        p_audio_device = PA_DEVICE_PORT_DATA(p);
        pa_assert(p_audio_device);
        if (audio_device == *p_audio_device)
            break;

        p = pa_hashmap_iterate(ports, &state, &key);
    }

    return (char *)key;
}

audio_devices_t pa_qahw_util_port_to_qahw_device(const char *port_name) {
    audio_devices_t device = AUDIO_DEVICE_NONE;
    uint32_t count;

    pa_assert(port_name);

    for (count = 0; count < ARRAY_SIZE(port_to_qahw_device); count++) {
        if (pa_streq(port_name, port_to_qahw_device[count].port_name)) {
            device = port_to_qahw_device[count].qahw_device;
            break;
        }
    }

    pa_log_debug("%s: port %s qahw device %u", __func__, port_name, device);

    return device;
}

audio_devices_t pa_qahw_util_device_name_to_enum(const char *device_name) {
    uint32_t count;
    audio_devices_t device = AUDIO_DEVICE_NONE;

    pa_assert(device_name);

    for (count = 0; count < ARRAY_SIZE(port_to_qahw_device); count++) {
        if (pa_streq(device_name, port_to_qahw_device[count].qahw_device_name)) {
            device = port_to_qahw_device[count].qahw_device;
            break;
        }
    }

    pa_log_debug("%s: device_name %s qahw device %u", __func__, device_name, device);

    return device;
}

pa_sample_format_t pa_qahw_util_get_pa_sample_from_qahw_format(audio_format_t format) {
    pa_sample_format_t pa_sample_format;

    switch(format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            pa_sample_format = PA_SAMPLE_S16LE;
            break;
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
            pa_sample_format = PA_SAMPLE_S24LE;
            break;
        case AUDIO_FORMAT_PCM_32_BIT:
            pa_sample_format = PA_SAMPLE_S32LE;
            break;
        default:
            pa_sample_format = PA_SAMPLE_INVALID;
            pa_log_error("%s: Unsupported format %d", __func__, format);
    }

    return pa_sample_format;
}

int pa_qahw_utils_convert_format_to_sample_spec(pa_format_info *format, pa_sample_spec *ss, pa_channel_map *map, pa_sample_spec *default_ss, pa_channel_map *default_map,
                                                int rate_idx, int sample_format_idx) {
    int32_t *sample_rates = NULL;
    int32_t num_sample_rates;
    int32_t *sample_formats = NULL;
    int32_t num_sample_formats;
    int32_t rc = -1;
    pa_format_info *new_format;

    pa_assert(format);
    pa_assert(ss);
    pa_assert(map);
    pa_assert(default_ss);
    pa_assert(default_map);

    new_format = pa_format_info_copy(format);

    /* if sample rate is an array then overwite default sample rate with first first sample rate in array */
    if (pa_format_info_get_prop_type(format, PA_PROP_FORMAT_RATE) == PA_PROP_TYPE_INT_ARRAY) {
        pa_log_info("%s: sample rate is in an array", __func__);
        pa_format_info_get_prop_int_array(format, PA_PROP_FORMAT_RATE, &sample_rates, &num_sample_rates);
        if (rate_idx >= num_sample_rates) {
            pa_log_error("%s: invalid sample rate index %d", __func__, rate_idx);
            goto exit;
        }
        pa_format_info_set_rate(new_format, sample_rates[rate_idx]);
        pa_xfree(sample_rates);
    }

    /* if sample rate is an array then overwite default sample rate with first first sample rate in array */
    if (pa_format_info_get_prop_type(format, PA_PROP_FORMAT_SAMPLE_FORMAT) == PA_PROP_TYPE_INT_ARRAY) {
        pa_log_info("%s: sample format is in an array", __func__);
        pa_format_info_get_prop_int_array(format, PA_PROP_FORMAT_SAMPLE_FORMAT, &sample_formats, &num_sample_formats);
        if (sample_format_idx >= num_sample_formats) {
            pa_log_error("%s: invalid sample format index %d", __func__, rate_idx);
            goto exit;
        }
        pa_format_info_set_sample_format(new_format, sample_formats[sample_format_idx]);
        pa_xfree(sample_formats);
    }

    rc = pa_format_info_to_sample_spec2(new_format, ss, map, default_ss, default_map);
    if (rc) {
        pa_log_error("%s: pa_format_info_to_sample_spec2 failed %d", __func__, rc);
        goto exit;
    }

    rc = 0;

exit:
    pa_format_info_free(new_format);
    return rc;
}

static pa_qahw_util_pa_qahw_channel_map pa_qahw_channel_map[] = {
    { PA_CHANNEL_POSITION_MONO, QAHW_PCM_CHANNEL_MS },
    { PA_CHANNEL_POSITION_FRONT_LEFT , QAHW_PCM_CHANNEL_FL },
    { PA_CHANNEL_POSITION_FRONT_RIGHT , QAHW_PCM_CHANNEL_FR },
    { PA_CHANNEL_POSITION_FRONT_CENTER, QAHW_PCM_CHANNEL_FC },
    { PA_CHANNEL_POSITION_SIDE_LEFT, QAHW_PCM_CHANNEL_LS },
    { PA_CHANNEL_POSITION_SIDE_RIGHT, QAHW_PCM_CHANNEL_RS },
    { PA_CHANNEL_POSITION_LFE, QAHW_PCM_CHANNEL_LFE },
    { PA_CHANNEL_POSITION_REAR_CENTER, QAHW_PCM_CHANNEL_CS },
    { PA_CHANNEL_POSITION_REAR_LEFT, QAHW_PCM_CHANNEL_LB },
    { PA_CHANNEL_POSITION_REAR_RIGHT, QAHW_PCM_CHANNEL_RB },
    { PA_CHANNEL_POSITION_TOP_CENTER, QAHW_PCM_CHANNEL_TS },
    { PA_CHANNEL_POSITION_TOP_FRONT_CENTER, QAHW_PCM_CHANNEL_CVH },
    { PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER, QAHW_PCM_CHANNEL_FLC },
    { PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER, QAHW_PCM_CHANNEL_FRC },
    { PA_CHANNEL_POSITION_SIDE_LEFT, QAHW_PCM_CHANNEL_SL },
    { PA_CHANNEL_POSITION_SIDE_RIGHT, QAHW_PCM_CHANNEL_SR },
    { PA_CHANNEL_POSITION_TOP_FRONT_LEFT, QAHW_PCM_CHANNEL_TFL },
    { PA_CHANNEL_POSITION_TOP_FRONT_RIGHT, QAHW_PCM_CHANNEL_TFR },
    { PA_CHANNEL_POSITION_TOP_CENTER, QAHW_PCM_CHANNEL_TC },
    { PA_CHANNEL_POSITION_TOP_REAR_LEFT, QAHW_PCM_CHANNEL_TBL },
    { PA_CHANNEL_POSITION_TOP_REAR_RIGHT, QAHW_PCM_CHANNEL_TBR },
    { PA_CHANNEL_POSITION_TOP_REAR_CENTER, QAHW_PCM_CHANNEL_TBC },

    /* FIXME: mapping for is missing in PA
       #define QAHW_PCM_CHANNEL_MS   12
       #define QAHW_PCM_CHANNEL_RLC  15
       #define QAHW_PCM_CHANNEL_RRC  16
       #define QAHW_PCM_CHANNEL_LFE2 17
       #define QAHW_PCM_CHANNEL_TSL 25
       #define QAHW_PCM_CHANNEL_TSR  26
       #define QAHW_PCM_CHANNEL_BFC  28
       #define QAHW_PCM_CHANNEL_BFL  29
       #define QAHW_PCM_CHANNEL_BFR  30
    */
};

bool pa_qahw_channel_map_to_qahw(pa_channel_map *pa_map, struct qahw_out_channel_map_param *qahw_map) {
    uint32_t channels;
    uint32_t count;
    bool present = false;

    pa_assert(pa_map);
    pa_assert(qahw_map);

    qahw_map->channels = pa_map->channels;
    for (channels = 0; channels < pa_map->channels; channels++) {
        present = false;
        for (count = 0; count < ARRAY_SIZE(pa_qahw_channel_map); count++) {
            if (pa_map->map[channels] == pa_qahw_channel_map[count].pa_channel_map_position) {
                qahw_map->channel_map[channels] = pa_qahw_channel_map[count].qahw_channel_map_position;
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

bool pa_qahw_channel_map_from_qahw(struct qahw_out_channel_map_param *qahw_map, pa_channel_map *pa_map) {
    uint32_t channels;
    uint32_t count;
    bool present = false;

    pa_assert(pa_map);
    pa_assert(qahw_map);

    pa_map->channels = qahw_map->channels;
    for (channels = 0; channels < pa_map->channels; channels++) {
        present = false;
        for (count = 0; count < ARRAY_SIZE(pa_qahw_channel_map); count++) {
            if (qahw_map->channel_map[channels] == pa_qahw_channel_map[count].qahw_channel_map_position) {
                pa_map->map[channels] = pa_qahw_channel_map[count].pa_channel_map_position;
                present = true;
                break;
            }
        }

        if (!present) {
            pa_log_error("%s: unsupported qahw channel position %x", __func__, qahw_map->channel_map[channels]);
            return false;
        }
    }

    return true;
}
