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

#ifndef fooqahwpasourceextnfoo
#define fooqahwpasourceextnfoo

#include <pulsecore/core.h>

#include <qahw_defs.h>

typedef size_t pa_qahw_source_extn_handle_t;

int pa_qahw_source_extn_create(pa_core *core, qahw_stream_handle_t *in_handle, int pa_source_index, pa_qahw_source_extn_handle_t **handle);
int pa_qahw_source_extn_free(pa_qahw_source_extn_handle_t *handle);
int pa_qahw_source_extn_source_handle_update(pa_qahw_source_extn_handle_t *handle, qahw_stream_handle_t *in_handle);

#endif
