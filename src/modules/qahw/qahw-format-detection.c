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

#include <fcntl.h>
#include <stdbool.h>

#include "qahw-jack-format.h"
#include "qahw-utils.h"

#define DEFAULT_NUM_CHANNELS 2

typedef enum {
    PA_QAHW_JACK_INPUT_MODE_PCM = 0,
    PA_QAHW_JACK_INPUT_MODE_COMPRESS = 1,
    PA_QAHW_JACK_INPUT_MODE_DSD = 2,
} pa_qahw_jack_input_mode_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t bitwidth;
    uint32_t channels;
    uint32_t layout;
    uint32_t channel_allocation;
    pa_qahw_jack_input_mode_t mode;
    int32_t preemph_status;
} pa_qahw_jack_sys_node_config_t;

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

static int pa_qahw_format_detection_config_to_jack_config(pa_qahw_jack_sys_node_config_t *sys_config, pa_qahw_jack_out_config *jack_config) {
    int rc = -1;

    pa_assert(sys_config);
    pa_assert(jack_config);

    if (sys_config->sample_rate == 0) {
        if (sys_config->mode != PA_QAHW_JACK_INPUT_MODE_DSD)
            sys_config->sample_rate = 48000;
        else
            sys_config->sample_rate = 44100;
    }

    if (sys_config->channels == 0)
        sys_config->channels = DEFAULT_NUM_CHANNELS;

    if (sys_config->mode != PA_QAHW_JACK_INPUT_MODE_COMPRESS)
        jack_config->preemph_status = sys_config->preemph_status;
    else
        jack_config->preemph_status = 0;

    jack_config->ss.rate = sys_config->sample_rate;

    if (sys_config->mode != PA_QAHW_JACK_INPUT_MODE_DSD)
        jack_config->ss.format =  PA_SAMPLE_S16LE; /* FIXME:assume format is 16bit for now */
    else
        jack_config->ss.format =  PA_SAMPLE_S32LE; /* DSD always uses 32bit format */

    pa_channel_map_init(&(jack_config->map));
    if (sys_config->mode != PA_QAHW_JACK_INPUT_MODE_DSD)
        pa_channel_map_init_auto(&(jack_config->map), 2, PA_CHANNEL_MAP_DEFAULT);
    else
        pa_channel_map_init_auto(&(jack_config->map), 6, PA_CHANNEL_MAP_DEFAULT);
    jack_config->ss.channels = jack_config->map.channels;

    if (sys_config->layout != 0 && sys_config->layout !=1)
        goto exit;

    if ((sys_config->mode == PA_QAHW_JACK_INPUT_MODE_COMPRESS) && (sys_config->layout == 0)) {
        if (sys_config->sample_rate == 192000 || sys_config->sample_rate == 176400) {
            jack_config->encoding = PA_ENCODING_UNKNOWN_4X_IEC61937; /* EAC3 */

            /* convert to transmission to media rate, as pa expects same
               for PA_ENCODING_UNKNOWN_4X_IEC61937 media_rate = transmission_rate/4 */
            jack_config->ss.rate = sys_config->sample_rate / 4;
        } else {
            jack_config->encoding = PA_ENCODING_UNKNOWN_IEC61937; /* Non HBR */
        }
    } else if (sys_config->mode == PA_QAHW_JACK_INPUT_MODE_COMPRESS && sys_config->layout == 1) {
        jack_config->encoding = PA_ENCODING_UNKNOWN_HBR_IEC61937; /* HBR */
        pa_qahw_util_channel_map_init(&(jack_config->map), 8);
        jack_config->ss.channels = jack_config->map.channels;
    } else if (sys_config->mode == PA_QAHW_JACK_INPUT_MODE_PCM) {
        jack_config->encoding = PA_ENCODING_PCM;
        if (sys_config->layout == 1) {
            pa_qahw_util_channel_map_init(&(jack_config->map), 8);
            jack_config->ss.channels = jack_config->map.channels;
            /* FIXME: get channel map from channel allocation and update map with correct channel count. For multichannel pcm transmission rate will be 8
               and 2 for other uscasese,
             */
        }
    } else if (sys_config->mode == PA_QAHW_JACK_INPUT_MODE_DSD) {
        jack_config->encoding = PA_ENCODING_DSD;
        pa_qahw_util_channel_map_init(&(jack_config->map), sys_config->channels);
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

static int pa_qahw_format_detection_read_from_fd(const char* path) {
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

static int pa_qahw_format_detection_get_num_channels(int infoframe_channels) {
    if (infoframe_channels > 0 && infoframe_channels <= 8) {
        /* refer CEA-861-D Table 17 Audio InfoFrame Data Byte 1 */
        return (infoframe_channels);
    }

    /* Return default value when infoframe channels is out of bound */
    return DEFAULT_NUM_CHANNELS;
}

bool pa_qahw_format_detection_get_value_from_path(const char* path, int *node_value) {
    bool rc = true;
    int value = -1;

    if (path) {
        if ((value = pa_qahw_format_detection_read_from_fd(path)) == -1) {
            pa_log_error("%s: Unable to read %s path", __func__, path);
            rc = false;
        }
    }

    *node_value = value;

    return rc;
}

int pa_qahw_hdmi_jack_get_config(pa_qahw_jack_type_t jack_type, pa_qahw_jack_sys_path sys_path,
                                                          pa_qahw_jack_out_config *jack_config) {
    int rc = -1;
    pa_qahw_jack_sys_node_config_t new_config = {0, 16, DEFAULT_NUM_CHANNELS, 0, 0, -1, 0};
    int audio_state_value;
    int audio_format_value;
    int audio_rate_value;
    int audio_channel_value;
    int audio_channel_alloc_value;
    int audio_layout_value;
    int arc_enable_value;
    int arc_audio_state_value;
    int arc_audio_format_value;
    int arc_audio_rate_value;
    int arc_audio_preemph_value;
    int audio_preemph_value;
    int dsd_rate_value;

    jack_config->active_jack = PA_QAHW_JACK_TYPE_INVALID;

    if (!pa_qahw_format_detection_get_value_from_path(sys_path.audio_state, &audio_state_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.audio_format, &audio_format_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.audio_rate, &audio_rate_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.audio_channel, &audio_channel_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.audio_layout, &audio_layout_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.audio_channel_alloc, &audio_channel_alloc_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.arc_enable, &arc_enable_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.arc_audio_state, &arc_audio_state_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.arc_audio_format, &arc_audio_format_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.arc_audio_rate, &arc_audio_rate_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.audio_preemph, &audio_preemph_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.arc_audio_preemph, &arc_audio_preemph_value) ||
        !pa_qahw_format_detection_get_value_from_path(sys_path.dsd_rate, &dsd_rate_value))
        goto exit;

    if (audio_format_value != -1)
        new_config.mode = (pa_qahw_jack_input_mode_t)audio_format_value;

    if (audio_rate_value != -1)
        new_config.sample_rate = (uint32_t)audio_rate_value;

    if (audio_channel_value != -1) {
        if (new_config.mode != PA_QAHW_JACK_INPUT_MODE_DSD) {
            audio_channel_value = pa_qahw_format_detection_get_num_channels(audio_channel_value);
            new_config.channels = (uint32_t)audio_channel_value;
        } else {
            new_config.channels = (uint32_t)audio_channel_value;
        }
    }

    if (audio_layout_value != -1) {
        new_config.layout = (uint32_t)audio_layout_value;
        if (new_config.layout == 1) {
            if (new_config.mode == PA_QAHW_JACK_INPUT_MODE_DSD)
                audio_channel_value = 6;
            else if (new_config.mode == PA_QAHW_JACK_INPUT_MODE_COMPRESS)
                audio_channel_value = 8;
        } else {
            if (new_config.mode != PA_QAHW_JACK_INPUT_MODE_DSD)
                audio_channel_value = 2;
        }
    }

    if (audio_channel_alloc_value)
        new_config.channel_allocation = (uint32_t)audio_channel_alloc_value;

    /* Assign current active jack and based on current configs */
    if (arc_enable_value) {
        new_config.sample_rate = arc_audio_rate_value;
        new_config.channels = DEFAULT_NUM_CHANNELS;
        new_config.layout = 0;
        new_config.mode = arc_audio_format_value;
        new_config.preemph_status = arc_audio_preemph_value;
        if ((jack_type != PA_QAHW_JACK_TYPE_HDMI_ARC) || (arc_audio_state_value == 2)) {
            pa_log_debug("%s: HDMI audio interface SPDIF ARC", __func__);
            jack_config->active_jack = PA_QAHW_JACK_TYPE_HDMI_ARC;
        }
    } else if (audio_state_value && (audio_layout_value == 0) && (audio_format_value == 1)) {
        if ((jack_type != PA_QAHW_JACK_TYPE_HDMI_ARC) || (arc_audio_state_value == 2)) {
            pa_log_debug("%s: HDMI audio interface SPDIF ARC", __func__);
            jack_config->active_jack = PA_QAHW_JACK_TYPE_HDMI_ARC;

            new_config.channels = (uint32_t)audio_channel_value;
            new_config.mode = audio_format_value;
            new_config.sample_rate = audio_rate_value;
            new_config.preemph_status = audio_preemph_value;
        }
    } else if (audio_state_value) {
        pa_log_debug("%s: HDMI audio interface MI2S", __func__);
        jack_config->active_jack = PA_QAHW_JACK_TYPE_HDMI_IN;

        if (new_config.mode == PA_QAHW_JACK_INPUT_MODE_DSD) {
            if (dsd_rate_value == -1)
                dsd_rate_value = 64; /* On error, assign DSD rate to default value */

            jack_config->dsd_rate = (uint32_t)dsd_rate_value;
            new_config.sample_rate = (uint32_t)audio_rate_value;
        } else {
            new_config.sample_rate = (uint32_t)audio_rate_value;
        }

        new_config.channels = (uint32_t)audio_channel_value;
        new_config.preemph_status = audio_preemph_value;
    }

    rc = pa_qahw_format_detection_config_to_jack_config(&new_config, jack_config);

    if ((sys_path.audio_channel_alloc) && (arc_enable_value == 0)) {
        if (new_config.layout == 1 && jack_config->encoding == PA_ENCODING_PCM) {
            new_config.channels = (uint32_t)audio_channel_value;
            jack_config->ss.channels = new_config.channels;
            jack_config->map.channels = jack_config->ss.channels;
        }

        pa_qahw_util_channel_allocation_to_pa_channel_map(&(jack_config->map), new_config.channel_allocation);
    }

    /* When there is no device switch, set active jack as current jack */
    if (jack_config->active_jack == PA_QAHW_JACK_TYPE_INVALID)
        jack_config->active_jack = jack_type;

exit:
    return rc;
}

int pa_qahw_spdif_jack_get_config(pa_qahw_jack_type_t jack_type, pa_qahw_jack_sys_path sys_path,
                                                           pa_qahw_jack_out_config *jack_config) {
    int rc = -1;
    pa_qahw_jack_sys_node_config_t new_config = {0, 16, DEFAULT_NUM_CHANNELS, 0, 0, -1, 0};
    int audio_rate_value = -1;
    int audio_format_value = -1;
    int audio_state_value = -1;
    int audio_preemph_value = -1;

    jack_config->active_jack = PA_QAHW_JACK_TYPE_INVALID;

    if (sys_path.audio_state) {
        if ((audio_state_value = pa_qahw_format_detection_read_from_fd(sys_path.audio_state)) == -1) {
            pa_log_error("%s: Unable to read %s path", __func__, sys_path.audio_state);
            goto exit;
        }
    }

    if (sys_path.audio_format) {
        if ((audio_format_value = pa_qahw_format_detection_read_from_fd(sys_path.audio_format)) == -1) {
            pa_log_error("%s: Unable to read %s path", __func__, sys_path.audio_format);
            goto exit;
        }
    }

    if (sys_path.audio_rate) {
        if ((audio_rate_value = pa_qahw_format_detection_read_from_fd(sys_path.audio_rate)) == -1) {
            pa_log_error("%s: Unable to read %s path", __func__, sys_path.audio_rate);
            goto exit;
        }
    }

    if (sys_path.audio_preemph) {
        if ((audio_preemph_value = pa_qahw_format_detection_read_from_fd(sys_path.audio_preemph)) == -1) {
            pa_log_error("%s: Unable to read %s path", __func__, sys_path.audio_preemph);
            goto exit;
        }
    }

    if ((jack_type != PA_QAHW_JACK_TYPE_SPDIF) || (audio_state_value == 2))
        jack_config->active_jack = PA_QAHW_JACK_TYPE_SPDIF;

    new_config.mode = (uint32_t)audio_format_value;
    new_config.sample_rate = (uint32_t)audio_rate_value;
    new_config.channels = (uint32_t)DEFAULT_NUM_CHANNELS;
    new_config.preemph_status = audio_preemph_value;

    rc = pa_qahw_format_detection_config_to_jack_config(&new_config, jack_config);

exit:
    return rc;
}
