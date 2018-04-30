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

#include "pulsecore/log.h"
#include "qahw-utils.h"

audio_format_t get_qahw_audio_format(pa_sample_format_t format) {
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

const char* pa_qahw_jack_type_to_port_name(pa_qahw_jack_type_t jack_type) {

    switch (jack_type) {
        case PA_QAHW_JACK_TYPE_WIRED_HEADSET:
            return "headset";
        case PA_QAHW_JACK_TYPE_WIRED_HEADPHONE:
            return "headphone";
        case PA_QAHW_JACK_TYPE_LINEOUT:
            return "lineout";
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
