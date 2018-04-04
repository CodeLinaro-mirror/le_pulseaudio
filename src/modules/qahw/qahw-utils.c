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
