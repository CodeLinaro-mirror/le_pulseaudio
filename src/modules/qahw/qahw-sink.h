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

#ifndef fooqahwpasinkfoo
#define fooqahwpasinkfoo

#include <pulsecore/device-port.h>
#include <pulse/sample.h>
#include <pulsecore/card.h>
#include <pulsecore/core.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-sink-extn.h"

typedef size_t pa_qahw_sink_handle_t;

audio_io_handle_t pa_qahw_sink_get_io_handle(pa_qahw_sink_handle_t *handle);
int pa_qahw_sink_get_index(pa_qahw_sink_handle_t *handle);
int pa_qahw_sink_get_flags(pa_qahw_sink_handle_t *handle);

/* create qahw session and pa sink */
int pa_qahw_sink_create(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name,
                 const char *profile_name, pa_sample_spec *ss, pa_channel_map *map, uint32_t sink_devices, int32_t flags,
                 int sink_idx, pa_qahw_sink_handle_t **handle);
void pa_qahw_sink_close(pa_qahw_sink_handle_t *handle);

#endif
