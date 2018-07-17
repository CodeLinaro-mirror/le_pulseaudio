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

#ifndef fooqahwpasourcehfoo
#define fooqahwpasourcehfoo

#include <pulse/sample.h>
#include <pulsecore/card.h>
#include <pulsecore/core.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-card.h"

typedef size_t pa_qahw_source_handle_t;

/*create qahw session and pa source */
int pa_qahw_source_create(pa_module *m, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, const char *module_name,
                 const char *profile_name, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, uint32_t source_devices, int32_t flags,
                 pa_qahw_card_source_usecase_id_t source_id, pa_qahw_source_handle_t **handle);
void pa_qahw_source_close(pa_qahw_source_handle_t *handle);

#endif
