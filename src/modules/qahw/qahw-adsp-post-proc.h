/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/protocol-dbus.h>
#include <qahw_api.h>

typedef void* pa_qahw_post_proc_handle_t;
typedef void qahw_post_proc_module_handle_t;
typedef void qahw_post_proc_stream_handle_t;

typedef struct {
    char *name;
    char *description;
    uint32_t topology_id;
    uint32_t app_type;
    char **effect_conf_string;
    pa_hashmap *effect_configs;
    uint32_t latency_us;
} pa_qahw_topology_config;

#ifdef QAHW_AUDIO_ADSP_PP_ENABLED
pa_qahw_post_proc_handle_t pa_qahw_post_proc_module_init (pa_core *core, pa_dbus_protocol *dbus_protocol,
                                                            pa_hashmap *topologies);
void pa_qahw_post_proc_module_deinit (pa_qahw_post_proc_handle_t post_proc_handle);
#else
#define pa_qahw_post_proc_module_init(core, dbus_protocol, topologies) (0)
#define pa_qahw_post_proc_module_deinit(post_proc_handle) ((void) 0)
#endif
