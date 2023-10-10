/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
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

#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-format.h>
#include <pulse/sample.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/protocol-dbus.h>

#include <string.h>

#include <PalApi.h>
#include <PalDefs.h>
#include <agm/agm_api.h>

#include "pal-source.h"
#include "pal-sink.h"
#include "pal-card.h"
#include "pal-config-parser.h"
#include "pal-loopback.h"

#define CONC(A,B) (A B)
#define PAL_MODULE_ID_PREFIX "audio."
#define PAL_MODULE_PRIMARY "primary"

#ifndef PAL_MODULE_ID_PRIMARY
#define PAL_MODULE_ID_PRIMARY CONC(PAL_MODULE_ID_PREFIX, PAL_MODULE_PRIMARY)
#endif

#define PAL_CARD_NAME_PREFIX "pal."
#define DEFAULT_PROFILE "default"

PA_MODULE_AUTHOR("QTI");
PA_MODULE_DESCRIPTION("pal card module");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

/* We don't have any module arguments */
PA_MODULE_USAGE(
        "module=audio.primary"
        "conf_dir_name= direct from pal conf is present"
        "conf_file_name= pal conf name is present in conf_dir_name"
);

static const char* const valid_modargs[] = {
    "module",
    "conf_dir_name",
    "conf_file_name",
    NULL
};

struct userdata {
    pa_core *core;
    pa_card *card;
    const char *driver;
    char *module_name;
    pa_module *module;
    pa_hashmap *profiles;
    pa_modargs *modargs;

    pa_sample_spec ss;
    pa_channel_map map;

    pa_hashmap *sinks;
    pa_hashmap *sources;

    pa_pal_config_data *config_data;
    char *conf_dir_name;
    char *conf_file_name;
};

/* internal functions */

static int pa_pal_card_add_source(pa_module *module, pa_card *card, const char *driver, char *module_name, pa_pal_source_config *source,
                                  pa_pal_source_handle_t **source_handle);
static int pa_pal_card_add_sink(pa_module *module, pa_card *card, const char *driver, char *module_name, pa_pal_sink_config *sink,
                                pa_pal_sink_handle_t **sink_handle);

static void pa_pal_card_profiles_free(struct userdata *u, pa_hashmap *profiles) {
    pa_card_profile *p;
    void *state;

    PA_HASHMAP_FOREACH(p, profiles, state) {
        pa_hashmap_remove_and_free(profiles, p->name);
    }
}

static void pa_pal_card_create_ports(struct userdata *u, pa_hashmap *ports, pa_hashmap *profiles) {
    pa_device_port *port;
    pa_pal_card_port_config *config_port;
    pa_device_port_new_data port_data;
    pa_pal_card_port_device_data *port_device_data = NULL;

    void *state;

    pa_log_debug("%s:\n", __func__);
    pa_assert(u);
    pa_assert(ports);
    pa_assert(profiles);

    PA_HASHMAP_FOREACH(config_port, u->config_data->ports, state) {
        pa_device_port_new_data_init(&port_data);

        pa_device_port_new_data_set_name(&port_data, config_port->name);

        pa_device_port_new_data_set_description(&port_data, config_port->description);
        pa_device_port_new_data_set_direction(&port_data, config_port->direction);
        pa_device_port_new_data_set_available(&port_data, config_port->available);

        port = pa_device_port_new(u->core, &port_data, sizeof(pa_pal_card_port_device_data));

        port_device_data = PA_DEVICE_PORT_DATA(port);

        port_device_data->device = config_port->device;
        port->priority = config_port->priority;
        port_device_data->default_map = config_port->default_map;
        port_device_data->default_spec.channels = config_port->default_map.channels;
        port_device_data->default_spec.rate = config_port->default_spec.rate;

        /* Sanity check that we don't have duplicates */
        pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);

        pa_device_port_new_data_done(&port_data);

    }
}

static void pa_pal_card_create_profiles_and_add_ports(struct userdata *u, pa_hashmap *profiles, pa_hashmap *ports) {
    pa_card_profile *profile = NULL;
    pa_pal_card_port_config *config_port;
    pa_device_port *card_port;
    pa_pal_card_profile_config *config_profile;

    void *state;
    void *state1;

    pa_log_debug("%s:\n", __func__);

    PA_HASHMAP_FOREACH(config_profile, u->config_data->profiles, state) {
        profile = pa_card_profile_new(config_profile->name, config_profile->description, 0);

        pa_log_debug("%s:profile %s created",__func__, profile->name);

        profile->priority = config_profile->priority;
        profile->n_sinks = config_profile->n_sinks;
        profile->n_sources = config_profile->n_sources;
        profile->available =  PA_AVAILABLE_YES;

        pa_hashmap_put(profiles, profile->name, profile);

        /* Add profile to port */
        /* get port list from config profile structure */
        PA_HASHMAP_FOREACH(config_port, config_profile->ports, state1) {
            card_port = pa_hashmap_get(ports, config_port->name);
            if (!card_port) {
                pa_log_error("%s, skipping port %s as doesn't belong to card", __func__, config_port->name);
                continue;
            }

            pa_log_debug("%s: adding profile %s for port %s", __func__, profile->name, config_port->name);
            pa_hashmap_put(card_port->profiles, profile->name, profile);
        }
    }
}


static int pa_pal_card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    pa_log_error("profile change not supported yet");
    return 0;
}

static void pa_pal_card_free(struct userdata *u) {
    pa_assert(u);

    if (u->card)
        pa_card_free(u->card);
}

/* create port and profile and adds it card */
static int pa_pal_card_create(struct userdata *u) {
    pa_card_new_data data;
    pa_card_profile *profile;

    pa_assert(u);
    pa_log_debug("%s:\n", __func__);
    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;
    data.name =  pa_sprintf_malloc("%s%s", PAL_CARD_NAME_PREFIX, u->module_name);
    data.namereg_fail = true;

    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Card for the %s HAL module", u->module_name);

    pa_pal_card_create_ports(u, data.ports, data.profiles);
    pa_pal_card_create_profiles_and_add_ports(u, data.profiles, data.ports);

    u->card = pa_card_new(u->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card) {
        pa_log_error("Failed to allocate card.");
        pa_pal_card_profiles_free(u, data.profiles);
        return -1;
    }

    u->card->userdata = u;
    u->card->set_profile = pa_pal_card_set_profile;

    profile = pa_hashmap_get(u->card->profiles, DEFAULT_PROFILE);
    if (!profile) {
        pa_log("profile not found");
        pa_pal_card_free(u);
        return -1;
    }

    u->card->active_profile = profile;

    pa_card_set_profile(u->card, profile, false);

    pa_card_put(u->card);

    return 0;
}

static int pa_pal_card_add_source(pa_module *module, pa_card *card, const char *driver, char *module_name, pa_pal_source_config *source,
                                  pa_pal_source_handle_t **source_handle) {
    uint32_t rc = 0;

    pa_assert(module);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module);
    pa_assert(module_name);
    pa_assert(source);

    rc = pa_pal_source_create(module, card, driver, module_name, source, source_handle);
    if (rc) {
        pa_log_error("%s: source %s create failed %d ", __func__, source->name, rc);
    }

    return rc;
}

static int pa_pal_card_create_sources(struct userdata *u, const char *profile_name, pa_pal_card_usecase_type_t usecase_type) {
    uint32_t rc = 0;
    pa_pal_source_config *source;

    void *state;

    pa_pal_card_source_info *source_info;

    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if (!(pa_hashmap_get(source->profiles, profile_name)) || source->usecase_type != usecase_type)
            continue;

        source_info = pa_xnew0(pa_pal_card_source_info, 1);
        rc = pa_pal_card_add_source(u->module, u->card, u->driver, u->module_name, source, &(source_info->handle));
        if (rc) {
            pa_log_error("%s: source %s create failed for profile %s, error %d ", __func__, source->name, profile_name, rc);
            source_info->handle = NULL;
            continue;
        }

        pa_hashmap_put(u->sources, source->name, source_info);
    }

    return rc;
}

static void pa_pal_card_free_sources(struct userdata *u, const char *profile_name) {
    pa_pal_source_config *source;
    void *state;
    pa_pal_card_source_info *source_info;

    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if (!(pa_hashmap_get(source->profiles, profile_name)))
            continue;

        source_info = pa_hashmap_get(u->sources, source->name);

        if (source_info) {
            pa_pal_source_close(source_info->handle);

            pa_hashmap_remove(u->sources, source->name);
            pa_xfree(source_info);
        }
    }
}

static int pa_pal_card_add_sink(pa_module *module, pa_card *card, const char *driver, char *module_name,
                                 pa_pal_sink_config *sink, pa_pal_sink_handle_t **sink_handle) {
    uint32_t rc = 0;

    pa_assert(module);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module);
    pa_assert(module_name);
    pa_assert(sink);

    rc = pa_pal_sink_create(module, card, driver, module_name, sink, sink_handle);
    if (rc) {
        pa_log_error("%s: sink %s create failed %d ", __func__, sink->name, rc);
    }

    return rc;
}

static int pa_pal_card_create_sinks(struct userdata *u, const char *profile_name, pa_pal_card_usecase_type_t usecase_type) {
    uint32_t rc = 0;
    pa_pal_sink_config *sink;

    void *state;

    pa_pal_card_sink_info *sink_info;

    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if (!(pa_hashmap_get(sink->profiles, profile_name)) || sink->usecase_type != usecase_type)
            continue;

        sink_info = pa_xnew0(pa_pal_card_sink_info, 1);
        rc = pa_pal_card_add_sink(u->module, u->card, u->driver, u->module_name, sink, &(sink_info->handle));
        if (rc) {
            pa_log_error("%s: sink %s create failed for profile %s, error %d ", __func__, sink->name, profile_name, rc);
            sink_info->handle = NULL;
            continue;
        }

        pa_hashmap_put(u->sinks, sink->name, sink_info);

    }

    return rc;
}

static void pa_pal_card_free_sinks(struct userdata *u, const char *profile_name) {
    pa_pal_sink_config *sink;
    void *state;
    pa_pal_card_sink_info *sink_info;

    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if (!(pa_hashmap_get(sink->profiles, profile_name)))
            continue;

        sink_info = pa_hashmap_get(u->sinks, sink->name);

        if (sink_info) {
            pa_pal_sink_close(sink_info->handle);

            pa_hashmap_remove(u->sinks, sink->name);
            pa_xfree(sink_info);
        }
    }
}

int pa__init(pa_module *m) {
    struct userdata *u;
    pa_modargs *ma;

    int ret = 0;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        ma = NULL;
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->modargs = ma;
    u->module = m;
    u->core = m->core;
    u->driver = __FILE__;

    u->module_name = pa_xstrdup(pa_modargs_get_value(ma, "module", PAL_MODULE_ID_PRIMARY));

    if (pa_streq(u->module_name, PAL_MODULE_ID_PRIMARY)) {
        pa_log_debug("Loading pal module %s ", u->module_name);
    } else {
        pa_log_error("Unsupported module_name %s", u->module_name);
        goto fail;
    }

    u->conf_dir_name = pa_xstrdup(pa_modargs_get_value(ma, "conf_dir_name", NULL));
    u->conf_file_name = pa_xstrdup(pa_modargs_get_value(ma, "conf_file_name", NULL));

    u->config_data = pa_pal_config_parse_new(u->conf_dir_name, u->conf_file_name);
    if (!u->config_data) {
        pa_log_error("%s: pa_pal_config_parse_new failed", __func__);
        goto fail;
    }

    ret = agm_init();
    if (ret) {
        pa_log_error("%s: agm init failed\n", __func__);
        goto fail;
    }

    ret = pal_init();
    if (ret) {
        pa_log_error("%s: pal init failed\n", __func__);
        goto fail;
    }

    pa_pal_card_create(u);

    if (!u->config_data->default_profile) {
        pa_log_info("%s: default profile not present in card conf", __func__);
        u->config_data->default_profile = (char *)DEFAULT_PROFILE;
    }

    pa_pal_sink_module_init();
    if (pa_hashmap_size(u->config_data->sinks)) {
        u->sinks = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

        if (PA_UNLIKELY(pa_pal_card_create_sinks(u, u->config_data->default_profile, PA_PAL_CARD_USECASE_TYPE_STATIC)))
            goto fail;

    }

    pa_log_info("%s: using default profile %s", __func__, u->config_data->default_profile);

    if (pa_hashmap_size(u->config_data->sources)) {
        u->sources = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        if (PA_UNLIKELY(pa_pal_card_create_sources(u, u->config_data->default_profile, PA_PAL_CARD_USECASE_TYPE_STATIC)))
            goto fail;
    }

    pa_log_debug("module %s loaded", u->module_name);

    ret = pa_pal_module_extn_init(u->core, u->card);
    if(ret)
        pa_log_error("pal extn init failed\n");
    pa_log_debug("Pal extn module loaded successfully\n", __func__);

    if (pa_hashmap_size(u->config_data->loopbacks)) {
        ret = pa_pal_loopback_init(u->core, u->card, u->config_data->loopbacks, (void *)u, m);
        if (ret)
            pa_log_error("Pal loopback init failed !!");
    }

    return ret;

fail:
    ret = -1;
    pa__done(m);
    return ret;
}

void pa__done(pa_module *m) {
    struct userdata *u;
    pa_card_profile *profile;
    void *state;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    pa_pal_module_extn_deinit();
    pa_pal_loopback_deinit();

    if (u->sinks) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            pa_pal_card_free_sinks(u, profile->name);

        pa_hashmap_free(u->sinks);
    }

    pa_pal_sink_module_deinit();

    if (u->sources) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            pa_pal_card_free_sources(u, profile->name);

        pa_hashmap_free(u->sources);
    }

    pal_deinit();

    agm_deinit();

    pa_pal_card_free(u);

    if (u->config_data)
        pa_pal_config_parse_free(u->config_data);

    pa_log_debug("module %s unloaded", u->module_name);

    if (u->module_name)
        pa_xfree(u->module_name);

    if (u->conf_dir_name)
        pa_xfree(u->conf_dir_name);

    if (u->conf_file_name)
        pa_xfree(u->conf_file_name);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    pa_xfree(u);
}
