/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
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
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <stdbool.h>

#include "qal-jack-format.h"
#include "qal-utils.h"

#define DEFAULT_NUM_CHANNELS 2

typedef enum {
    PA_PAL_JACK_INPUT_MODE_PCM = 0,
    PA_PAL_JACK_INPUT_MODE_COMPRESS = 1,
    PA_PAL_JACK_INPUT_MODE_DSD = 2,
} pa_qal_jack_input_mode_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t bitwidth;
    uint32_t channels;
    uint32_t layout;
    uint32_t channel_allocation;
    pa_qal_jack_input_mode_t mode;
    int32_t preemph_status;
} pa_pal_jack_sys_node_config_t;

int supported_pcm_sample_rates[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};

/******* Function definitions ********/
static bool is_pcm_sample_rate_valid(int rate) {
    bool rc = false;
    uint32_t i = 0;

    for (i = 0; i < ARRAY_SIZE(supported_pcm_sample_rates); i++) {
        if (rate == supported_pcm_sample_rates[i]) {
            rc = true;
            break;
        }
    }

    return rc;
}

static int pa_pal_format_detection_config_to_jack_config(pa_pal_jack_sys_node_config_t *sys_config, pa_pal_jack_out_config *jack_config) {
    int rc = -1;

    pa_assert(sys_config);
    pa_assert(jack_config);

    if (sys_config->sample_rate == 0) {
        if (sys_config->mode != PA_PAL_JACK_INPUT_MODE_DSD)
            sys_config->sample_rate = 48000;
        else
            sys_config->sample_rate = 44100;
    }

    if (sys_config->channels == 0)
        sys_config->channels = DEFAULT_NUM_CHANNELS;

    if (sys_config->mode != PA_PAL_JACK_INPUT_MODE_COMPRESS)
        jack_config->preemph_status = sys_config->preemph_status;
    else
        jack_config->preemph_status = 0;

    jack_config->ss.rate = sys_config->sample_rate;

    if (sys_config->mode != PA_PAL_JACK_INPUT_MODE_DSD)
        jack_config->ss.format =  PA_SAMPLE_S16LE; /* FIXME:assume format is 16bit for now */
    else
        jack_config->ss.format =  PA_SAMPLE_S32LE; /* DSD always uses 32bit format */

    pa_channel_map_init(&(jack_config->map));
    if (sys_config->mode != PA_PAL_JACK_INPUT_MODE_DSD)
        pa_channel_map_init_auto(&(jack_config->map), 2, PA_CHANNEL_MAP_DEFAULT);
    else
        pa_channel_map_init_auto(&(jack_config->map), 6, PA_CHANNEL_MAP_DEFAULT);
    jack_config->ss.channels = jack_config->map.channels;

    if (sys_config->layout != 0 && sys_config->layout !=1)
        goto exit;

    if ((sys_config->mode == PA_PAL_JACK_INPUT_MODE_COMPRESS) && (sys_config->layout == 0)) {
        if (sys_config->sample_rate == 192000 || sys_config->sample_rate == 176400) {
            jack_config->encoding = PA_ENCODING_UNKNOWN_4X_IEC61937; /* EAC3 */

            /* convert to transmission to media rate, as pa expects same
               for PA_ENCODING_UNKNOWN_4X_IEC61937 media_rate = transmission_rate/4 */
            jack_config->ss.rate = sys_config->sample_rate / 4;
        } else {
            jack_config->encoding = PA_ENCODING_UNKNOWN_IEC61937; /* Non HBR */
        }
    } else if (sys_config->mode == PA_PAL_JACK_INPUT_MODE_COMPRESS && sys_config->layout == 1) {
        jack_config->encoding = PA_ENCODING_UNKNOWN_HBR_IEC61937; /* HBR */
        pa_pal_util_channel_map_init(&(jack_config->map), 8);
        jack_config->ss.channels = jack_config->map.channels;
    } else if (sys_config->mode == PA_PAL_JACK_INPUT_MODE_PCM) {
        jack_config->encoding = PA_ENCODING_PCM;
        if (sys_config->layout == 1) {
            pa_channel_map_init(&(jack_config->map));
            jack_config->map.channels = sys_config->channels;
            jack_config->ss.channels = jack_config->map.channels;
            /* FIXME: get channel map from channel allocation and update map with correct channel count. For multichannel pcm transmission rate will be 8
               and 2 for other uscasese,
             */
        }
    } else if (sys_config->mode == PA_PAL_JACK_INPUT_MODE_DSD) {
        jack_config->encoding = PA_ENCODING_DSD;
        pa_pal_util_channel_map_init(&(jack_config->map), sys_config->channels);
        jack_config->ss.channels = jack_config->map.channels;
    } else {
        pa_log_error("%s: not a valid jack configure mode %d", __func__, sys_config->mode);
        goto exit;
    }

    /* Check if sample rate is valid for corresponding encoding */
    switch (jack_config->encoding) {
        case PA_ENCODING_UNKNOWN_IEC61937:
            if ((sys_config->sample_rate != 32000) && (sys_config->sample_rate != 44100) && (sys_config->sample_rate != 48000)) {
                pa_log_error("%s: Unsupported sample rate %d for encoding %d", __func__, sys_config->sample_rate, jack_config->encoding);
                goto exit;
            }
            break;
        case PA_ENCODING_UNKNOWN_4X_IEC61937:
        case PA_ENCODING_UNKNOWN_HBR_IEC61937:
            if ((sys_config->sample_rate != 176400) && (sys_config->sample_rate != 192000)) {
                pa_log_error("%s: Unsupported sample rate %d for encoding %d", __func__, sys_config->sample_rate, jack_config->encoding);
                goto exit;
            }
            break;
        case  PA_ENCODING_PCM:
            if (!is_pcm_sample_rate_valid(sys_config->sample_rate)) {
                pa_log_error("%s: Unsupported sample rate %d for encoding %d", __func__, sys_config->sample_rate, jack_config->encoding);
                goto exit;
            }
            break;
        case PA_ENCODING_DSD:
            if ((sys_config->sample_rate != 44100) && (sys_config->sample_rate != 88200)) {
                pa_log_error("%s: Unsupported sample rate %d for encoding %d", __func__, sys_config->sample_rate, jack_config->encoding);
                goto exit;
            }
            break;
        default:
            pa_log_error("%s: Unsupported encoding %d", __func__, jack_config->encoding);
            goto exit;
    }

    rc = 0;

exit:
    return rc;
}

static int pa_pal_format_detection_read_from_fd(const char* path) {
    int fd = -1;
    char buf[16];
    int ret;
    int value;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        pa_log_error("Unable open fd for file %s\n", path);
        return -1;
    }

    ret = read(fd, buf, 15);
    if (ret < 0) {
        pa_log_error("File %s Data is empty\n", path);
        close(fd);
        return -1;
    }

    buf[ret] = '\0';
    value = atoi(buf);
    close(fd);

    return value;
}

static int pa_pal_format_detection_get_num_channels(int infoframe_channels) {
    if (infoframe_channels > 0 && infoframe_channels <= 8) {
        /* refer CEA-861-D Table 17 Audio InfoFrame Data Byte 1 */
        return (infoframe_channels);
    }

    /* Return default value when infoframe channels is out of bound */
    return DEFAULT_NUM_CHANNELS;
}

bool pa_pal_format_detection_get_value_from_path(const char* path, int *node_value) {
    bool rc = true;
    int value = -1;

    if (path) {
        if ((value = pa_pal_format_detection_read_from_fd(path)) == -1) {
            pa_log_error("%s: Unable to read %s path", __func__, path);
            rc = false;
        }
    }

    *node_value = value;

    return rc;
}
