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

#include "qahw-utils.h"

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

const char* pa_qahw_util_jack_type_to_port_name(pa_qahw_jack_type_t jack_type) {

    switch (jack_type) {
        case PA_QAHW_JACK_TYPE_WIRED_HEADSET:
            return "headset";
        case PA_QAHW_JACK_TYPE_WIRED_HEADPHONE:
            return "headphone";
        case PA_QAHW_JACK_TYPE_LINEOUT:
            return "lineout";
        case PA_QAHW_JACK_TYPE_HDMI:
            return "hdmi-in";
        default:
            return NULL;
    }
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
    audio_devices_t device;

    if (pa_streq(port_name, "speaker")) {
        device = AUDIO_DEVICE_OUT_SPEAKER;
    } else if (pa_streq(port_name, "headset")) {
        device = AUDIO_DEVICE_OUT_WIRED_HEADSET;
    } else if (pa_streq(port_name, "headphone")) {
        device = AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    } else if (pa_streq(port_name, "lineout")) {
        device = AUDIO_DEVICE_OUT_LINE;
    } else if (pa_streq(port_name,"headset-mic")) {
        device = AUDIO_DEVICE_IN_WIRED_HEADSET;
    } else if (pa_streq(port_name, "builtin-mic")) {
        device = AUDIO_DEVICE_IN_BUILTIN_MIC;
    } else if (pa_streq(port_name, "hdmi-in")) {
        device = AUDIO_DEVICE_IN_HDMI;
    } else if (pa_streq(port_name, "linein")) {
        device = AUDIO_DEVICE_IN_LINE;
    } else if (pa_streq(port_name, "spdif-in")) {
        device = AUDIO_DEVICE_IN_SPDIF;
    } else {
        device = AUDIO_DEVICE_NONE;
        pa_log_error("%s: No qahw device mapping for  port %s", __func__, port_name);
    }

    pa_log_debug("%s: port %s qahw device %u", __func__, port_name, device);

    return device;
}

audio_devices_t pa_qahw_util_device_name_convert_string_to_enum(const char *device_name) {
    audio_devices_t device;

    if (pa_streq(device_name, "AUDIO_DEVICE_OUT_SPEAKER")) {
        device = AUDIO_DEVICE_OUT_SPEAKER;
    } else if (pa_streq(device_name, "AUDIO_DEVICE_OUT_WIRED_HEADSET")) {
        device = AUDIO_DEVICE_OUT_WIRED_HEADSET;
    } else if (pa_streq(device_name, "AUDIO_DEVICE_OUT_WIRED_HEADPHONE")) {
        device = AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    } else if (pa_streq(device_name, "AUDIO_DEVICE_OUT_LINE")) {
        device = AUDIO_DEVICE_OUT_LINE;
    } else if (pa_streq(device_name,"AUDIO_DEVICE_IN_WIRED_HEADSET")) {
        device = AUDIO_DEVICE_IN_WIRED_HEADSET;
    } else if (pa_streq(device_name, "AUDIO_DEVICE_IN_BUILTIN_MIC")) {
        device = AUDIO_DEVICE_IN_BUILTIN_MIC;
    } else if (pa_streq(device_name, "AUDIO_DEVICE_IN_HDMI")) {
        device = AUDIO_DEVICE_IN_HDMI;
    } else if (pa_streq(device_name, "AUDIO_DEVICE_IN_LINE")) {
        device = AUDIO_DEVICE_IN_LINE;
    } else if (pa_streq(device_name, "AUDIO_DEVICE_IN_SPDIF")) {
        device = AUDIO_DEVICE_IN_SPDIF;
    } else {
        device = AUDIO_DEVICE_NONE;
        pa_log_error("%s: No qahw device mapping for port %s", __func__, device_name);
    }

    pa_log_debug("%s: port %s qahw device %u", __func__, device_name, device);

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
#if 0
    if (is_prop_array) {
        if (pa_format_info_get_channel_map(format, map)) {
            pa_log_error("%s: channel map not present", __func__);
            goto exit;
        }
        ss->rate = sample_rates[rate_idx];
        ss->format = sample_formats[sample_format_idx];
        ss->channels = map->channels;
    } else {
        rc = pa_format_info_to_sample_spec2(format, ss, map, default_ss, default_map);
        if (rc) {
            pa_log_error("%s: pa_format_info_to_sample_spec2 failed %d", __func__, rc);
            goto exit;
        }
    }
#endif
    rc = 0;

exit:
    pa_format_info_free(new_format);
    return rc;
}
