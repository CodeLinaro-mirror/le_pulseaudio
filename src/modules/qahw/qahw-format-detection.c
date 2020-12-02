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

#include <fcntl.h>
#include <stdbool.h>

#include "qahw-jack.h"
#include "qahw-jack-format.h"

#define DEFAULT_NUM_CHANNELS 2

/* Specific for HDMI */
static const char hdmi_in_audio_sample_rate_sys_path[] = "/sys/devices/virtual/switch/sample_rate/state";
static const char hdmi_in_audio_channel_sys_path[] = "/sys/devices/virtual/switch/channels/state";
static const char hdmi_in_audio_format_sys_path[] = "/sys/devices/virtual/switch/audio_format/state";

typedef enum {
    PA_QAHW_JACK_INPUT_MODE_PCM = 0,
    PA_QAHW_JACK_INPUT_MODE_COMPRESS = 1,
} pa_qahw_jack_input_mode_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t bitwidth;
    uint32_t channels;
    uint32_t layout;
    uint32_t channel_allocation;
    pa_qahw_jack_input_mode_t mode;
} pa_qahw_jack_sys_node_config_t;

static int pa_qahw_format_detection_config_to_jack_config(pa_qahw_jack_sys_node_config_t *sys_config, pa_qahw_jack_config_t *jack_config) {
    int rc  = 0;

    pa_assert(sys_config);
    pa_assert(jack_config);

    if (sys_config->sample_rate == 0)
        sys_config->sample_rate = 48000;
    jack_config->ss.rate = sys_config->sample_rate;
    jack_config->ss.format =  PA_SAMPLE_S16LE; /* FIXME:assume format is 16bit for now */

    pa_channel_map_init(&(jack_config->map));
    pa_channel_map_init_auto(&(jack_config->map), 2, PA_CHANNEL_MAP_DEFAULT);
    jack_config->ss.channels = jack_config->map.channels;

    if (sys_config->layout != 0 && sys_config->layout !=1) {
        rc = -1;
        goto exit;
    }

    if ((sys_config->mode == PA_QAHW_JACK_INPUT_MODE_COMPRESS) && (sys_config->layout == 0)) {
        if (sys_config->sample_rate == 192000) {
            jack_config->encoding = PA_ENCODING_UNKNOWN_4X_IEC61937; /* EAC3 */

            /* convert to transmission to media rate, as pa expects same
               for PA_ENCODING_UNKNOWN_4X_IEC61937 media_rate = transmission_rate/4 */
            jack_config->ss.rate = sys_config->sample_rate / 4;
        } else {
            jack_config->encoding = PA_ENCODING_UNKNOWN_IEC61937; /* Non HBR */
        }
    } else if (sys_config->mode == PA_QAHW_JACK_INPUT_MODE_COMPRESS && sys_config->layout == 1) {
        jack_config->encoding = PA_ENCODING_UNKNOWN_HBR_IEC61937; /* HBR */
        pa_channel_map_init_auto(&(jack_config->map), 8, PA_CHANNEL_MAP_DEFAULT);
        jack_config->ss.channels = jack_config->map.channels;
    } else if (sys_config->mode == PA_QAHW_JACK_INPUT_MODE_PCM) {
        jack_config->encoding = PA_ENCODING_PCM;
        if (sys_config->layout == 1) {
            pa_channel_map_init_auto(&(jack_config->map), 8, PA_CHANNEL_MAP_DEFAULT);
            jack_config->ss.channels = jack_config->map.channels;
            /* FIXME: get channel map from channel allocation and update map with correct channel count. For multichannel pcm transmission rate will be 8
               and 2 for other uscasese,
             */
        }
    } else {
        pa_log_error("%s: not a valid jack configure mode %d", __func__, sys_config->mode);
        rc = -1;
    }

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
    if (infoframe_channels > 0 && infoframe_channels < 8) {
        /* refer CEA-861-D Table 17 Audio InfoFrame Data Byte 1 */
        return (infoframe_channels + 1);
    }

    /* Return default value when infoframe channels is out of bound */
    return DEFAULT_NUM_CHANNELS;
}

int pa_qahw_hdmi_jack_get_config(pa_qahw_jack_config_t *jack_config) {
    int rc = -1;
    int mode;
    int sample_rate;
    int channels;
    pa_qahw_jack_sys_node_config_t new_config = {0, 16, 0, 0, 0, -1};

    if (((sample_rate = pa_qahw_format_detection_read_from_fd(hdmi_in_audio_sample_rate_sys_path)) == -1) ||
         ((channels = pa_qahw_format_detection_read_from_fd(hdmi_in_audio_channel_sys_path)) == -1) ||
         ((mode = pa_qahw_format_detection_read_from_fd(hdmi_in_audio_format_sys_path)) == -1)) {
        pa_log_error("Not able to read sys path");
        goto exit;
    }

    new_config.mode = (pa_qahw_jack_input_mode_t)mode;
    new_config.sample_rate = (uint32_t)sample_rate;
    new_config.channels = (uint32_t)channels;
    new_config.channels = pa_qahw_format_detection_get_num_channels(new_config.channels);

    rc = pa_qahw_format_detection_config_to_jack_config(&new_config, jack_config);

exit:
    return rc;
}
