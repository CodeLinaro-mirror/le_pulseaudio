/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
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

#ifndef foopalconfparserfoo
#define foopalconfparserfoo

#include <pulsecore/conf-parser.h>

#include <PalApi.h>
#include <PalDefs.h>

#include "qal-card.h"

typedef struct {
    pa_hashmap *ports;
    pa_hashmap *profiles;
    pa_hashmap *sinks;
    pa_hashmap *sources;
    char *default_profile;
} pa_pal_config_data;

pa_pal_config_data* pa_pal_config_parse_new(char *dir, char *conf_file_name);
void pa_pal_config_parse_free(pa_pal_config_data *config_data);
#endif
