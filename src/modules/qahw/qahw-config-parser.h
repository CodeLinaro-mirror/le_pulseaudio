/*
 * Copyright (c) 2018,2020, The Linux Foundation. All rights reserved.
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

#ifndef fooqahwconfparserfoo
#define fooqahwconfparserfoo

#include <pulsecore/conf-parser.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-card.h"

typedef struct {
    pa_hashmap *ports;
    pa_hashmap *profiles;
    pa_hashmap *sinks;
    pa_hashmap *sources;
    pa_hashmap *effects;
    pa_hashmap *loopbacks;
    char *default_profile;
    bool use_dolby_hw_loopback;
    bool dsd_setup;
} pa_qahw_config_data;

pa_qahw_config_data* pa_qahw_config_parse_new(char *dir, char *conf_file_name);
void pa_qahw_config_parse_free(pa_qahw_config_data *config_data);
#endif
