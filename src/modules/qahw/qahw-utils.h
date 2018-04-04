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

#ifndef fooqahwutilsfoo
#define fooqahwutilsfoo

#include <pulse/sample.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#define KV_PAIR_MAX_LENGTH 100
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

audio_format_t get_qahw_audio_format(pa_sample_format_t format);

#endif
