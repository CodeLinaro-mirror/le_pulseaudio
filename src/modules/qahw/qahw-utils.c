/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
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

#define PA_QAHW_SINK_PROP_FORMAT_STREAM_FORMAT "stream-format"

typedef struct {
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

typedef struct {
    audio_format_t stream_format; /* raw (default), adts */
} pa_qahw_util_aac_compress_metadata;

typedef union {
    pa_qahw_util_aac_compress_metadata aac;
} pa_qahw_util_compress_metadata;

pa_qahw_util_compress_metadata compress_metadata;

pa_qahw_util_jack_type_to_port_name jack_type_to_port_name[] = {
    { PA_QAHW_JACK_TYPE_WIRED_HEADSET, (char*)"headset" },
    { PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS, (char*)"headset-mic" },
    { PA_QAHW_JACK_TYPE_WIRED_HEADPHONE, (char*)"headphone" },
    { PA_QAHW_JACK_TYPE_LINEOUT, (char*)"lineout"},
    { PA_QAHW_JACK_TYPE_HDMI_IN, (char*)"hdmi-in" },
    { PA_QAHW_JACK_TYPE_BTA2DP_OUT, (char*)"bta2dp-out" },
    { PA_QAHW_JACK_TYPE_BTA2DP_IN, (char*)"bta2dp-in" },
    { PA_QAHW_JACK_TYPE_HDMI_ARC, (char *)"hdmi-arc"},
    { PA_QAHW_JACK_TYPE_SPDIF, (char *)"spdif-in"},
    { PA_QAHW_JACK_TYPE_BTSCO_IN, (char *)"btsco-in"},
    { PA_QAHW_JACK_TYPE_BTSCO_OUT, (char *)"btsco-out"},
    { PA_QAHW_JACK_TYPE_HDMI_OUT, (char *)"hdmi-out"},
    { PA_QAHW_JACK_TYPE_SPDIF_OUT_OPTICAL, (char *)"spdif-out-optical"},
    { PA_QAHW_JACK_TYPE_SPDIF_OUT_COAXIAL, (char *)"spdif-out-coaxial"},
};

pa_qahw_util_port_to_qahw_device_mapping port_to_qahw_device[] = {
    { (char*)"speaker",          AUDIO_DEVICE_OUT_SPEAKER,              (char *)"AUDIO_DEVICE_OUT_SPEAKER" },
    { (char *)"headset",         AUDIO_DEVICE_OUT_WIRED_HEADSET,        (char *)"AUDIO_DEVICE_OUT_WIRED_HEADSET" },
    { (char*)"lineout",          AUDIO_DEVICE_OUT_LINE,                 (char *)"AUDIO_DEVICE_OUT_LINE"},
    { (char*)"headphone",        AUDIO_DEVICE_OUT_WIRED_HEADPHONE,      (char *)"AUDIO_DEVICE_OUT_WIRED_HEADPHONE" },
    { (char *)"bta2dp-out" ,     AUDIO_DEVICE_OUT_BLUETOOTH_A2DP,       (char *)"AUDIO_DEVICE_OUT_BLUETOOTH_A2DP"},
    { (char *)"headset-mic",     AUDIO_DEVICE_IN_WIRED_HEADSET,         (char *)"AUDIO_DEVICE_IN_WIRED_HEADSET" },
    { (char *)"builtin-mic",     AUDIO_DEVICE_IN_BUILTIN_MIC,           (char *)"AUDIO_DEVICE_IN_BUILTIN_MIC" },
    { (char *)"hdmi-in",         AUDIO_DEVICE_IN_HDMI,                  (char *)"AUDIO_DEVICE_IN_HDMI" },
    { (char *)"spdif-in",        AUDIO_DEVICE_IN_SPDIF,                 (char *)"AUDIO_DEVICE_IN_SPDIF" },
    { (char *)"linein",          AUDIO_DEVICE_IN_LINE,                  (char *)"AUDIO_DEVICE_IN_LINE" },
    { (char *)"bta2dp-in" ,      AUDIO_DEVICE_IN_BLUETOOTH_A2DP,        (char *)"AUDIO_DEVICE_IN_BLUETOOTH_A2DP"},
    { (char *)"hdmi-arc",        AUDIO_DEVICE_IN_HDMI_ARC,              (char *)"AUDIO_DEVICE_IN_HDMI_ARC" },
    { (char *)"builtin-mic-ec-ref-loopback", AUDIO_DEVICE_IN_BUILTIN_MIC | AUDIO_DEVICE_IN_LOOPBACK, (char *)"AUDIO_DEVICE_IN_BUILTIN_MIC_AND_EC_REF_LOOPBACK" },
    { (char *)"btsco-in",        AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET, (char *)"AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET" },
    { (char *)"btsco-out",       AUDIO_DEVICE_OUT_BLUETOOTH_SCO,        (char *)"AUDIO_DEVICE_OUT_BLUETOOTH_SCO" },
    { (char *)"speaker2",         QAHW_AUDIO_DEVICE_OUT_SPEAKER2,        (char *)"QAHW_AUDIO_DEVICE_OUT_SPEAKER2" },
    { (char *)"speaker3",         QAHW_AUDIO_DEVICE_OUT_SPEAKER3,        (char *)"QAHW_AUDIO_DEVICE_OUT_SPEAKER3" },
    { (char *)"hdmi-out",         AUDIO_DEVICE_OUT_HDMI,                 (char *)"AUDIO_DEVICE_OUT_HDMI" },
    { (char *)"spdif-out-optical",QAHW_AUDIO_DEVICE_OUT_OPTICAL,         (char *)"QAHW_AUDIO_DEVICE_OUT_OPTICAL" },
    { (char *)"spdif-out-coaxial",AUDIO_DEVICE_OUT_SPDIF,                (char *)"AUDIO_DEVICE_OUT_SPDIF" },
};

/*
 * pa_qahw_be_channel_map only contains supported QAHW backend channels.
 * which will be used to pass to HAL for corresponding PA channels.
 */
static pa_channel_position_t pa_qahw_be_channel_map[] = {
    QAHW_PCM_CHANNEL_FL,
    QAHW_PCM_CHANNEL_FR,
    QAHW_PCM_CHANNEL_LFE,
    QAHW_PCM_CHANNEL_FC,
    QAHW_PCM_CHANNEL_LS,
    QAHW_PCM_CHANNEL_RS,
    QAHW_PCM_CHANNEL_LB,
    QAHW_PCM_CHANNEL_RB
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
        case PA_ENCODING_MPEG:
            qahw_format = AUDIO_FORMAT_MP3;
            break;
        case PA_ENCODING_AAC:
            if (compress_metadata.aac.stream_format == AUDIO_FORMAT_AAC_ADTS)
                qahw_format = AUDIO_FORMAT_AAC_ADTS_HE_V2;
            else
                qahw_format = AUDIO_FORMAT_AAC_HE_V2;
            break;
        case PA_ENCODING_MAT_IEC61937:
            qahw_format = AUDIO_FORMAT_MAT;
            break;
        case PA_ENCODING_DSD:
            qahw_format = AUDIO_FORMAT_DSD;
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
        case AUDIO_FORMAT_MP3:
            pa_format = PA_ENCODING_MPEG;
            break;
        case AUDIO_FORMAT_AAC_HE_V2:
            pa_format = PA_ENCODING_AAC;
            break;
        case AUDIO_FORMAT_MAT:
            pa_format = PA_ENCODING_MAT_IEC61937;
            break;
        case AUDIO_FORMAT_DSD:
            pa_format = PA_ENCODING_DSD;
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

int pa_qahw_utils_format_to_sample_spec(pa_format_info *format, pa_sample_spec *ss, pa_channel_map *map,
                                                        pa_sample_spec *default_ss, pa_channel_map *default_map) {
    int32_t *sample_rates = NULL;
    int32_t num_sample_rates;
    char **sample_formats = NULL;
    int32_t num_sample_formats;
    int32_t rc = -1;
    pa_format_info *new_format;
    int rate_idx = 0;
    int sample_format_idx = 0;

    pa_assert(format);
    pa_assert(ss);
    pa_assert(map);
    pa_assert(default_ss);
    pa_assert(default_map);

    new_format = pa_format_info_copy(format);

    /* if sample rate is an array then overwite default sample rate with first sample rate in array */
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

    /* if sample format is an array then overwite default sample format with first sample format in array */
    if (pa_format_info_get_prop_type(format, PA_PROP_FORMAT_SAMPLE_FORMAT) == PA_PROP_TYPE_STRING_ARRAY) {
        pa_log_info("%s: sample format is in an array", __func__);
        pa_format_info_get_prop_string_array(format, PA_PROP_FORMAT_SAMPLE_FORMAT, &sample_formats, &num_sample_formats);

        if (sample_format_idx >= num_sample_formats) {
            pa_log_error("%s: invalid sample format index %d", __func__, sample_format_idx);
            goto exit;
        }

        pa_format_info_set_sample_format(new_format, pa_parse_sample_format(sample_formats[sample_format_idx]));
        pa_format_info_free_string_array(sample_formats, num_sample_formats);
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
    { PA_CHANNEL_POSITION_FRONT_LEFT_WIDE, QAHW_PCM_CHANNEL_LW },
    { PA_CHANNEL_POSITION_FRONT_RIGHT_WIDE, QAHW_PCM_CHANNEL_RW },
    { PA_CHANNEL_POSITION_TOP_SIDE_LEFT, QAHW_PCM_CHANNEL_TSL },
    { PA_CHANNEL_POSITION_TOP_SIDE_RIGHT, QAHW_PCM_CHANNEL_TSR },
    /*
     * Use AUX0 - AUX3 for Dolby Atmos 1.7.1 Ltf,Rtf,Ltr,Rtr
     */
    { PA_CHANNEL_POSITION_AUX0, QAHW_PCM_CHANNEL_TFL },
    { PA_CHANNEL_POSITION_AUX1, QAHW_PCM_CHANNEL_TFR },
    { PA_CHANNEL_POSITION_AUX2, QAHW_PCM_CHANNEL_TBL },
    { PA_CHANNEL_POSITION_AUX3, QAHW_PCM_CHANNEL_TBR },
    { PA_CHANNEL_POSITION_SURROUND_LEFT, QAHW_PCM_CHANNEL_SL },
    { PA_CHANNEL_POSITION_SURROUND_RIGHT, QAHW_PCM_CHANNEL_SR },
    /*
     * Use AUX27-AUX30 channel to support Lmix_d, Rmix_d, Lmix and
     * Rmix channels. AUX31 is used for dummy mapping to round for
     * an even number of channels in mainzone.
     * Custom channels are mapped from bottom to top for Lmix and
     * RMix channels.
     */
    { PA_CHANNEL_POSITION_AUX27, QAHW_PCM_CUSTOM_CHANNEL_MAP_12 },
    { PA_CHANNEL_POSITION_AUX28, QAHW_PCM_CUSTOM_CHANNEL_MAP_13 },
    { PA_CHANNEL_POSITION_AUX29, QAHW_PCM_CUSTOM_CHANNEL_MAP_14 },
    { PA_CHANNEL_POSITION_AUX30, QAHW_PCM_CUSTOM_CHANNEL_MAP_15 },
    { PA_CHANNEL_POSITION_AUX31, QAHW_PCM_CUSTOM_CHANNEL_MAP_16 },

    /*
     * Use AUX14 for Dolby Atmos 1.7.1 single height channel.
     */
    { PA_CHANNEL_POSITION_AUX14, QAHW_PCM_CHANNEL_TSL }
    /* FIXME: mapping for is missing in PA
       #define QAHW_PCM_CHANNEL_LFE2 17
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

/*
 * This Function will parse the gap channels received from input source
 */
pa_channel_map *pa_channel_map_parse_wrapper(pa_channel_map *rmap, const char *s) {
    const char *state;
    pa_channel_position_t f;
    char *p, *r;
    int count = 0;
    pa_assert(rmap);
    pa_assert(s);

    r = pa_replace(s,",gap","");
    if (!pa_channel_map_parse(rmap, r)) {
        pa_xfree(r);
        return NULL;
    }
    pa_xfree(r);
    state = NULL;
    while ((p = pa_split(s, ",", &state))) {
        if (pa_streq(p, "gap")) {
            f = rmap->map[count];
            for(int i = count ; i < 8; i++ ) {
                f = f + rmap->map[i+1];
                rmap->map[i+1] = f - rmap->map[i+1];
                f = f - rmap->map[i+1];
            }
            rmap->map[count] = PA_CHANNEL_POSITION_INVALID;
        }
        count++;
        pa_xfree(p);
    }

    return rmap;
}

/*
 * This Function will remove the invalid channels in case of invalid channels passed
 * in hdmi non-2ch and non-ch usecases ultimately to derive map w.r.t be channel map.
 */
pa_channel_map pa_map_remove_invalid_channels(pa_channel_map *def_map_with_inval_ch) {
    uint32_t i=0, j=0;
    pa_channel_map pa_map;
    pa_assert(def_map_with_inval_ch);

    while(i < ARRAY_SIZE(pa_qahw_be_channel_map)) {
        if (def_map_with_inval_ch->map[i] != PA_CHANNEL_POSITION_INVALID) {
            pa_map.map[j] = def_map_with_inval_ch->map[i];
            j++;
        }
        i++;
    }
    pa_map.channels = def_map_with_inval_ch->channels;
    return pa_map;
}

/*
 * This Function will check PA channels and assign corresponding QAHW BE channel in qahw_be_map
 */
void pa_qahw_channel_map_to_be_qahw(pa_channel_map *pa_map, struct qahw_in_channel_map_param *qahw_be_map) {
    uint32_t i=0, j=0;

    pa_assert(pa_map);
    pa_assert(qahw_be_map);

    while(i < ARRAY_SIZE(pa_qahw_be_channel_map)) {
        if (pa_map->map[i] != PA_CHANNEL_POSITION_INVALID) {
            qahw_be_map->channel_map[j] = pa_qahw_be_channel_map[i];
            j++;
        }
        i++;
    }
    qahw_be_map->channels = j;
}

void pa_qahw_util_channel_allocation_to_pa_channel_map(pa_channel_map *m, uint32_t channel_allocation) {
    pa_assert(m);

    switch (channel_allocation) {
        case (0x01):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x00):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            break;
        case (0x03):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x02):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            break;
        case (0x05):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x04):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_REAR_CENTER;
            break;
        case (0x07):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x06):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_REAR_CENTER;
            break;
        case (0x09):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x08):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            break;
        case (0x0B):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x0A):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            break;
        case (0x0D):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x0C):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_REAR_CENTER;
            break;
        case (0x0F):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x0E):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_REAR_CENTER;
            break;
        case (0x11):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x10):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_REAR_LEFT;
            m->map[7] = PA_CHANNEL_POSITION_REAR_RIGHT;
            break;
        case (0x13):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x12):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_REAR_LEFT;
            m->map[7] = PA_CHANNEL_POSITION_REAR_RIGHT;
            break;
        case (0x15):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x14):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
            break;
        case (0x17):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x16):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
            break;
        case (0x19):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x18):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_REAR_CENTER;
            m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
            break;
        case (0x1B):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x1A):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_REAR_CENTER;
            m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
            break;
        case (0x1D):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x1C):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
            break;
        case (0x1F):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x1E):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
            break;
        case (0x21):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x20):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_TOP_FRONT_CENTER;
            break;
        case (0x23):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x22):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[7] = PA_CHANNEL_POSITION_TOP_CENTER;
            break;
        case (0x25):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x24):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_TOP_FRONT_LEFT;
            m->map[7] = PA_CHANNEL_POSITION_TOP_FRONT_RIGHT;
            break;
        case (0x27):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x26):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_WIDE;
            m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_WIDE;
            break;
        case (0x29):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x28):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_REAR_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_TOP_CENTER;
            break;
        case (0x2B):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x2A):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_REAR_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_TOP_FRONT_CENTER;
            break;
        case (0x2D):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x2C):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_TOP_FRONT_CENTER;
            m->map[7] = PA_CHANNEL_POSITION_TOP_CENTER;
            break;
        case (0x2F):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x2E):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_TOP_FRONT_LEFT;
            m->map[7] = PA_CHANNEL_POSITION_TOP_FRONT_RIGHT;
            break;
        case (0x31):
            m->map[2] = PA_CHANNEL_POSITION_LFE;
            /* fall through */
        case (0x30):
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_WIDE;
            m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_WIDE;
            break;
        default:
            pa_log_error("%s: Channel mapping for %x allocation not supported", __func__, channel_allocation);
            break;
    }
}

void pa_qahw_util_get_jack_sys_path(pa_qahw_card_port_config *config_port, pa_qahw_jack_in_config *jack_in_config) {
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

/* With reference to the translation table from "Dolby Atmos to Sound Bar Product
 * System Development Manual" */
pa_channel_map* pa_qahw_util_channel_map_init(pa_channel_map *m, unsigned channels) {
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

int pa_qahw_util_set_qahw_metadata_from_pa_format(const pa_format_info *format) {
    int rc = 0;
    char *stream_format;

    pa_assert(format);

    switch (format->encoding) {
        case PA_ENCODING_AAC:
            rc = pa_format_info_get_prop_string(format,
                 PA_QAHW_SINK_PROP_FORMAT_STREAM_FORMAT, &stream_format);
            if (rc) {
                pa_log_error("%s: Failed to obtain AAC stream format", __func__);
            } else {
               if (pa_streq(stream_format, "adts")) {
                   pa_log_debug("%s: adts format", __func__);
                   compress_metadata.aac.stream_format = AUDIO_FORMAT_AAC_ADTS;
               } else {
                   pa_log_debug("%s: raw format", __func__);
                   compress_metadata.aac.stream_format = AUDIO_FORMAT_AAC;
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

audio_channel_mask_t pa_qahw_util_in_mask_from_count(uint32_t channel_count)
{
    audio_channel_mask_t channel_mask = AUDIO_CHANNEL_INVALID;

    switch(channel_count) {
    case 10:
        channel_mask = AUDIO_CHANNEL_INDEX_MASK_10;
        break;
    case 11:
        channel_mask = AUDIO_CHANNEL_INDEX_MASK_11;
        break;
    case 12:
        channel_mask = AUDIO_CHANNEL_INDEX_MASK_12;
        break;
    case 13:
        channel_mask = AUDIO_CHANNEL_INDEX_MASK_13;
        break;
    case 14:
        channel_mask = AUDIO_CHANNEL_INDEX_MASK_14;
        break;
    default:
        pa_log_error("%s: Invalid channel count %d", __func__, channel_count);
        break;
    }

    return channel_mask;
}

pa_qahw_card_avoid_processing_config_id_t pa_qahw_utils_get_config_id_from_string(const char *config_str) {
    pa_qahw_card_avoid_processing_config_id_t config_id = PA_QAHW_CARD_AVOID_PROCESSING_FOR_NONE;

    if (pa_streq(config_str, "all") || pa_streq(config_str, "true"))
        config_id = PA_QAHW_CARD_AVOID_PROCESSING_FOR_ALL;
    else if (pa_streq(config_str, "rate"))
        config_id = PA_QAHW_CARD_AVOID_PROCESSING_FOR_SAMPLE_RATE;
    else if (pa_streq(config_str, "bitwidth"))
        config_id = PA_QAHW_CARD_AVOID_PROCESSING_FOR_BIT_WIDTH;
    else if (pa_streq(config_str, "channels"))
        config_id = PA_QAHW_CARD_AVOID_PROCESSING_FOR_CHANNELS;
    else
        pa_log_error("%s: Unsupported config %s", __func__, config_str);

    return config_id;
}

pa_qahw_card_qahw_processing_id_t pa_qahw_utils_get_qahw_processing_id_from_string(const char *config_str) {
    pa_qahw_card_qahw_processing_id_t id = PA_QAHW_CARD_QAHW_PROCESSING_NONE;

    if (pa_streq(config_str, "fluence"))
        id = PA_QAHW_CARD_QAHW_PROCESSING_FLUENCE;
    else if (pa_streq(config_str, "ffecns"))
        id = PA_QAHW_CARD_QAHW_PROCESSING_FFECNS;

    return id;
}
