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

/******* Function definitions ********/
static void pa_qahw_format_detection_read_from_fd(const char* path, uint32_t *value) {
    int fd = -1;
    char buf[16];
    int ret;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        pa_log_error("Unable open fd for file %s\n", path);
        return;
    }

    ret = read(fd, buf, 15);
    if (ret < 0) {
        pa_log_error("File %s Data is empty\n", path);
        close(fd);
        return;
    }

    buf[ret] = '\0';
    *value = atoi(buf);
    close(fd);
}

static int pa_qahw_format_detection_get_num_channels(int infoframe_channels) {
    if (infoframe_channels > 0 && infoframe_channels < 8) {
      /* refer CEA-861-D Table 17 Audio InfoFrame Data Byte 1 */
        return (infoframe_channels + 1);
    }

    /* Return default value when infoframe channels is out of bound */
    return DEFAULT_NUM_CHANNELS;
}

bool pa_qahw_hdmi_jack_get_config(pa_qahw_jack_config_t *curr_config) {
    bool rc = false;

    pa_qahw_jack_config_t new_config = {0, 16, 0, 0, 0, -1};

    pa_qahw_format_detection_read_from_fd(hdmi_in_audio_sample_rate_sys_path,
                                                  &(new_config.sample_rate));
    pa_qahw_format_detection_read_from_fd(hdmi_in_audio_channel_sys_path,
                                                  &(new_config.channels));
    pa_qahw_format_detection_read_from_fd(hdmi_in_audio_format_sys_path,
                                                     &(new_config.mode));

    new_config.channels = pa_qahw_format_detection_get_num_channels(new_config.channels);

    if (memcmp(curr_config, &new_config, sizeof(pa_qahw_jack_config_t)))
        rc = true;

    memcpy(curr_config, &new_config, sizeof(pa_qahw_jack_config_t));

    return rc;
}



