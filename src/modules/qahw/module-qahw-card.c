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

#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-format.h>
#include <pulse/sample.h>
#include <pulsecore/modargs.h>

#include <string.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-source.h"
#include "qahw-utils.h"
#include "qahw-jack.h"
#include "qahw-jack-format.h"
#include "qahw-loopback.h"
#include "qahw-card-extn.h"
#include "qahw-effect.h"
#include "qahw-card.h"
#include "qahw-config-parser.h"

#define CONC(A,B) (A B)
#define QAHW_MODULE_ID_PREFIX "audio."
#define QAHW_MODULE_PRIMARY "primary"

#ifndef QAHW_MODULE_ID_PRIMARY
#define QAHW_MODULE_ID_PRIMARY CONC(QAHW_MODULE_ID_PREFIX, QAHW_MODULE_PRIMARY)
#endif

#define QAHW_CARD_NAME_PREFIX "qahw."
#define DEFAULT_PROFILE "default"


PA_MODULE_AUTHOR("QTI");
PA_MODULE_DESCRIPTION("qahw card module");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

/* We don't have any module arguments */
PA_MODULE_USAGE(
        "module=audio.primary"
        "conf_dir_name= direct from qahw conf is present"
        "conf_file_name= qahw conf name is present in conf_dir_name"
);

static const char* const valid_modargs[] = {
    "module",
    "conf_dir_name",
    "conf_file_name",
    NULL
};

struct jack_handle_list {
    pa_qahw_jack_handle_t *handle;
    pa_qahw_jack_type_t jack_type;
    PA_LLIST_FIELDS(struct jack_handle_list);
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

    qahw_module_handle_t *module_handle;

    pa_hashmap *sink_handles;
    pa_hashmap *source_handles;

    pa_qahw_effect_handle_t effect_handle;

    PA_LLIST_HEAD(struct jack_handle_list, jack_handle_list_head);

    pa_qahw_config_data *config_data;
    char *conf_dir_name;
    char *conf_file_name;
};

/* internal functions */

static int pa_qahw_card_add_source(pa_module *module, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, char *module_name,
                                   pa_qahw_source_config *source, pa_qahw_source_handle_t **source_handle);

static int pa_qahw_card_convert_config_ports_to_card_ports(pa_hashmap *src_ports, pa_hashmap *dest_ports, pa_card *card) {
    pa_qahw_card_port_config *config_port;
    pa_device_port *card_port;
    void *state;

    PA_HASHMAP_FOREACH(config_port, src_ports, state) {
        if ((card_port = pa_hashmap_get(card->ports, config_port->name)))
            pa_hashmap_put(dest_ports, config_port->name, card_port);
    }
    return 0;
}

static bool pa_qahw_card_is_dynamic_source_supported_for_port(pa_device_port *port, struct userdata *u) {
    pa_assert(u);
    pa_assert(port);

    /* FIXME: update this once spdif and arc support is added */
    if (pa_streq(port->name, "hdmi-in") && !u->config_data->use_dolby_hw_loopback)
        return true;

    return false;
}

static void pa_qahw_card_remove_dynamic_source(pa_device_port *port, struct userdata *u) {
    pa_qahw_source_config *source = NULL;
    pa_qahw_source_handle_t *handle;
    void *state;

    pa_assert(port);

    pa_log_debug("%s:", __func__);

    /*find a dynamic source which supports give a port, currently assumption is that one dynamic source is supported for a port */
    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if ((source->usecase_type == PA_QAHW_CARD_USECASE_TYPE_DYNAMIC) && (pa_hashmap_get(source->ports, port->name))) {
            /* check if this source supports required encoding */
            pa_log_info("%s: Found a dynamic source %s for port %s", __func__, source->name, port->name);
            handle = pa_hashmap_get(u->source_handles, source->name);
            break;
        }
    }


    if (source && !handle) {
        pa_log_error("%s: no dynamic usecase present, skip removal of source ", __func__);
        goto exit;
    }

    pa_qahw_source_close(handle);

    pa_hashmap_remove(u->source_handles, source->name);

exit:
    return;
}

static void pa_qahw_card_add_dynamic_source(pa_device_port *port, pa_qahw_jack_config_t *config, struct userdata *u) {
    int rc;
    bool reconfigure = false;

    pa_qahw_source_handle_t *handle = NULL;

    pa_qahw_source_config *source;
    pa_qahw_source_config new_source;
    pa_hashmap *ports;

    pa_format_info *requested_format;
    pa_format_info *current_format;
    pa_format_info *config_format;
    pa_idxset *current_formats;
    pa_idxset *requested_formats;
    pa_sample_spec ss;
    pa_channel_map map;

    uint32_t requested_sample_rate;
    uint32_t requested_channels;
    char fmt[PA_FORMAT_INFO_SNPRINT_MAX];

    void *state;
    uint32_t i = 0;
    bool source_found = false;

    pa_assert(port);
    pa_assert(config);
    pa_assert(u);

    pa_log_debug("%s:", __func__);

    requested_format = pa_format_info_from_sample_spec(&config->ss, &config->map);
    if (!requested_format) {
        pa_log_error("%s: Invalid jack format", __func__);
        goto exit;
    }

    requested_format->encoding = config->encoding;

    pa_log_info("%s: format = %s", __func__, pa_format_info_snprint(fmt, sizeof(fmt), requested_format));

    /*find a dynamic source which supports give a port, currently assumption is that one dynamic source is supported for a port */
    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if ((source->usecase_type == PA_QAHW_CARD_USECASE_TYPE_DYNAMIC) && (pa_hashmap_get(source->ports, port->name))) {
            /* check if this source supports required encoding */
            PA_IDXSET_FOREACH(config_format, source->formats, i) {
                if (config_format->encoding == requested_format->encoding) {
                    source_found = true;
                    break;
                }
            }

            if (source_found) {
                pa_log_info("%s: Found a dynamic source %s for port %s", __func__, source->name, port->name);
                handle = pa_hashmap_get(u->source_handles, source->name);
                break;
            }
        }
    }

    if (!source_found) {
        pa_log_error("%s: dynamic source not supported for port %s", __func__, port->name);
        goto exit;
    }

    /* check if reconfigure is needed if yes then recreate the source */
    if (handle) {
        current_formats = pa_qahw_source_get_config(handle);
        if (!current_formats) {
            pa_log_error("%s: pa_qahw_source_get_config failed", __func__);
            goto exit;
        }

        current_format = pa_idxset_first(source->formats, NULL);
        pa_log_info("%s: current_format = %s requested format %s", __func__, pa_format_info_snprint(fmt, sizeof(fmt), current_format),
                     pa_format_info_snprint(fmt, sizeof(fmt), requested_format));

        pa_format_info_to_sample_spec(current_format, &ss, &map);

        if (!pa_format_info_get_rate(requested_format, &requested_sample_rate)) {
            pa_log_error("%s: sample_rate not published by jack", __func__);
            goto exit;
        }

        if (!pa_format_info_get_rate(requested_format, &requested_channels)) {
            pa_log_error("%s: channels not published by jack", __func__);
            goto exit;
        }

        if (requested_format->encoding != current_format->encoding)
            reconfigure = true;
        else if ((requested_format->encoding == PA_ENCODING_UNKNOWN_4X_IEC61937) && (requested_sample_rate != ss.rate))
            reconfigure = true;
        else if ((requested_format->encoding == PA_ENCODING_PCM) && ((requested_sample_rate != ss.rate || requested_channels != ss.channels)))
            reconfigure = true;

        if (reconfigure) {
            pa_log_info("%s: source reconfiguraiton needed, closing current source and createing new one", __func__);
            pa_qahw_card_remove_dynamic_source(port, u);
        } else {
            pa_log_info("%s: source already exits", __func__);
            goto exit;
        }
    }

    ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    pa_qahw_card_convert_config_ports_to_card_ports(source->ports, ports, u->card);

    requested_formats = pa_idxset_new(NULL, NULL);
    pa_idxset_put(requested_formats, requested_format, NULL);

    new_source = *source;
    new_source.formats = requested_formats;

    rc = pa_qahw_card_add_source(u->module, u->card, u->driver, u->module_handle, u->module_name, &new_source, &handle);
    if (rc) {
        pa_log_error("%s: source %s create failed for port %s, error %d ", __func__, source->name, port->name, rc);
        handle = NULL;
    }

    pa_hashmap_put(u->source_handles, new_source.name, handle);

    pa_hashmap_free(ports);

    pa_idxset_free(requested_formats, (pa_free_cb_t) pa_format_info_free);
exit:
   return;
}

static pa_hook_result_t pa_qahw_jack_callback(void *dummy __attribute__((unused)), pa_qahw_jack_event_data_t *event_data, void *prv_data) {
    const char *port_name = NULL;
    pa_available_t status = PA_AVAILABLE_UNKNOWN;
    pa_device_port *port;
    struct userdata *u;
    pa_qahw_jack_event_t event;

    pa_assert(event_data);
    pa_assert(prv_data);

    u  = (struct userdata *)prv_data;

    event = event_data->event;
    if ((event != PA_QAHW_JACK_AVAILABLE) && (event != PA_QAHW_JACK_UNAVAILABLE) && (event != PA_QAHW_JACK_CONFIG_UPDATE)) {
        pa_log_error("%s: unsupport qahw jack event %d",__func__, event);
        return PA_HOOK_CANCEL;
    }


    if (event_data->jack_type == PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS) {
        pa_log_info("PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS not supported currently");
        return PA_HOOK_CANCEL;
    }

    if (event == PA_QAHW_JACK_AVAILABLE)
        status = PA_AVAILABLE_YES;
    else if (event == PA_QAHW_JACK_UNAVAILABLE)
        status = PA_AVAILABLE_NO;

    port_name = pa_qahw_util_get_port_name_from_jack_type(event_data->jack_type);
    if (port_name != NULL) {
        pa_log_info("port %s satus %d event %x", port_name, status, event);
        port = pa_hashmap_get(u->card->ports, port_name);
        if (port) {
            if (event == PA_QAHW_JACK_AVAILABLE) {
                pa_device_port_set_available(port, status);
             } else if (event == PA_QAHW_JACK_UNAVAILABLE) {
                 pa_device_port_set_available(port, status);

                if ((port->direction == PA_DIRECTION_INPUT) && pa_qahw_card_is_dynamic_source_supported_for_port(port, u)) {
                     pa_qahw_card_remove_dynamic_source(port, u);
                 }
             } else if ((event == PA_QAHW_JACK_CONFIG_UPDATE) && (port->available == PA_AVAILABLE_YES)) {
                 if ((port->direction == PA_DIRECTION_INPUT) && (pa_qahw_card_is_dynamic_source_supported_for_port(port, u))) {
                     pa_qahw_card_add_dynamic_source(port, (pa_qahw_jack_config_t *)event_data->pa_qahw_jack_info, u);
                 }
             } else {
                pa_log_error("unsupported event %d", event);
            }
        } else {
            pa_log_error("unsupported port %s", port_name);
        }
        /* for headset, change status of headset-mic as well */
        if (pa_streq(port_name, "headset")) {
            port = pa_hashmap_get(u->card->ports, "headset-mic");
            if (port)
                pa_device_port_set_available(port, status);
        }
    } else {
        pa_log_error("unsupport jack type %d", event_data->jack_type);
    }

    return PA_HOOK_OK;
}

static void pa_qahw_card_disable_jack_detection(struct jack_handle_list *list_head, pa_module *m) {
    struct jack_handle_list *i, *n;

    pa_assert(list_head);

    PA_LLIST_FOREACH_SAFE(i, n, list_head) {
        if (pa_qahw_jack_deregister_event_callback(i->handle, m))
            pa_log_info("Jack event callback deregister successful for jack %d\n", i->jack_type);
        else
            pa_log_error("Jack event callback deregister failed for jack %d\n",  i->jack_type);

        PA_LLIST_REMOVE(struct jack_handle_list, list_head, i);
        pa_xfree(i);
    }
}

static void pa_qahw_card_enable_jack_detection(struct userdata *u) {
    pa_qahw_jack_handle_t *jack_handle;
    pa_qahw_jack_type_t jack_types = PA_QAHW_JACK_TYPE_INVALID;
    struct jack_handle_list *jack_handle_list_entry = NULL;
    pa_device_port *port;
    void *state;

   /* register for jack detection for dynamic port, PA_AVAILABLE_NO means its dynamic port */
    PA_HASHMAP_FOREACH(port, u->card->ports, state) {
        if (port->available == PA_AVAILABLE_NO)
            jack_types = pa_qahw_util_get_jack_type_from_port_name(port->name);
        else
            continue;

        jack_handle = pa_qahw_jack_register_event_callback(jack_types, pa_qahw_jack_callback, u->module, (void *)u);
        if (!jack_handle) {
            pa_log_error("%s: Enable qahw jack failed for port %s\n", __func__, port->name);
        } else {
            jack_handle_list_entry = pa_xnew0(struct jack_handle_list, 1);
            jack_handle_list_entry->handle = jack_handle;
            jack_handle_list_entry->jack_type = jack_types;
            PA_LLIST_INIT(struct jack_handle_list, jack_handle_list_entry);
            PA_LLIST_PREPEND(struct jack_handle_list, u->jack_handle_list_head, jack_handle_list_entry);
            jack_handle_list_entry = NULL;
        }
    }
}

static void pa_qahw_card_profiles_free(struct userdata *u, pa_hashmap *profiles) {
    pa_card_profile *p;
    void *state;

    PA_HASHMAP_FOREACH(p, profiles, state) {
        pa_hashmap_remove_and_free(profiles, p->name);
    }
}

static void pa_qahw_card_create_ports(struct userdata *u, pa_hashmap *ports, pa_hashmap *profiles) {
    pa_device_port *port;
    pa_qahw_card_port_config *config_port;
    pa_device_port_new_data port_data;
    pa_qahw_card_port_device_data *port_device_data = NULL;

    void *state;

    pa_assert(u);
    pa_assert(ports);
    pa_assert(profiles);

    PA_HASHMAP_FOREACH(config_port, u->config_data->ports, state) {
        pa_device_port_new_data_init(&port_data);

        pa_device_port_new_data_set_name(&port_data, config_port->name);

        pa_device_port_new_data_set_description(&port_data, config_port->description);
        pa_device_port_new_data_set_direction(&port_data, config_port->direction);
        pa_device_port_new_data_set_available(&port_data, config_port->available);

        port = pa_device_port_new(u->core, &port_data, sizeof(pa_qahw_card_port_device_data));

        port_device_data = PA_DEVICE_PORT_DATA(port);

        port_device_data->device = config_port->device;
        port->priority = config_port->priority;

        /* Sanity check that we don't have duplicates */
        pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);

        pa_device_port_new_data_done(&port_data);

    }
}

static void pa_qahw_card_create_profiles_and_add_ports(struct userdata *u, pa_hashmap *profiles, pa_hashmap *ports) {
    pa_card_profile *profile = NULL;
    pa_qahw_card_port_config *config_port;
    pa_device_port *card_port;
    pa_qahw_card_profile_config *config_profile;

    void *state;
    void *state1;

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


static int pa_qahw_card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    pa_log_error("profile change not supported yet");
    return 0;
}

static void pa_qahw_card_free(struct userdata *u) {
    pa_assert(u);

    if (u->card)
        pa_card_free(u->card);
}

/* create port and profile and adds it card */
static int pa_qahw_card_create(struct userdata *u) {
    pa_card_new_data data;
    pa_card_profile *profile;

    pa_assert(u);

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;
    data.name =  pa_sprintf_malloc("%s%s", QAHW_CARD_NAME_PREFIX, u->module_name);
    data.namereg_fail = true;

    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Card for the %s HAL module", u->module_name);

    pa_qahw_card_create_ports(u, data.ports, data.profiles);
    pa_qahw_card_create_profiles_and_add_ports(u, data.profiles, data.ports);

    u->card = pa_card_new(u->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card) {
        pa_log_error("Failed to allocate card.");
        pa_qahw_card_profiles_free(u, data.profiles);
        return -1;
    }

    u->card->userdata = u;
    u->card->set_profile = pa_qahw_card_set_profile;

    profile = pa_hashmap_get(u->card->profiles, DEFAULT_PROFILE);
    if (!profile) {
        pa_log("profile not found");
        pa_qahw_card_free(u);
        return -1;
    }

    pa_card_set_profile(u->card, profile, false);

    pa_card_put(u->card);

    return 0;
}

static int pa_qahw_card_add_source(pa_module *module, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, char *module_name,
                                   pa_qahw_source_config *source, pa_qahw_source_handle_t **source_handle) {
    uint32_t rc = 0;

    pa_assert(module);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module);
    pa_assert(module_handle);
    pa_assert(module_name);
    pa_assert(source);

    rc = pa_qahw_source_create(module, card, driver, module_handle, module_name, source, source_handle);
    if (rc) {
        pa_log_error("%s: source %s create failed %d ", __func__, source->name, rc);
    }

    return rc;
}

static int pa_qahw_card_create_sources(struct userdata *u, const char *profile_name, pa_qahw_card_usecase_type_t usecase_type) {
    uint32_t rc = 0;
    pa_qahw_source_config *source;

    void *state;

    pa_qahw_source_handle_t *handle;

    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if (!(pa_hashmap_get(source->profiles, profile_name)) || source->usecase_type != usecase_type)
            continue;

        rc = pa_qahw_card_add_source(u->module, u->card, u->driver, u->module_handle, u->module_name, source, &handle);
        if (rc) {
            pa_log_error("%s: source %s create failed for profile %s, error %d ", __func__, source->name, profile_name, rc);
            handle = NULL;
            continue;
        }

        pa_hashmap_put(u->source_handles, source->name, handle);

    }

    return rc;
}

static void pa_qahw_card_free_sources(struct userdata *u, const char *profile_name) {
    pa_qahw_source_config *source;
    void *state;
    pa_qahw_source_handle_t *source_handle;

    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if (!(pa_hashmap_get(source->profiles, profile_name)))
            continue;

        source_handle = pa_hashmap_get(u->source_handles, source->name);

        if (source_handle) {
            pa_qahw_source_close(source_handle);
            pa_hashmap_remove(u->source_handles, source->name);
        }
    }
}

static int pa_qahw_card_add_sink(pa_module *module, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, char *module_name,
                                 pa_qahw_sink_config *sink, pa_qahw_sink_handle_t **sink_handle) {
    uint32_t rc = 0;

    pa_assert(module);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module);
    pa_assert(module_handle);
    pa_assert(module_name);
    pa_assert(sink);

    rc = pa_qahw_sink_create(module, card, driver, module_handle, module_name, sink, sink_handle);
    if (rc) {
        pa_log_error("%s: sink %s create failed %d ", __func__, sink->name, rc);
    }

    return rc;
}

static int pa_qahw_card_create_sinks(struct userdata *u, const char *profile_name, pa_qahw_card_usecase_type_t usecase_type) {
    uint32_t rc = 0;
    pa_qahw_sink_config *sink;

    void *state;

    pa_qahw_sink_handle_t *handle;

    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if (!(pa_hashmap_get(sink->profiles, profile_name)) || sink->usecase_type != usecase_type)
            continue;

        rc = pa_qahw_card_add_sink(u->module, u->card, u->driver, u->module_handle, u->module_name, sink, &handle);
        if (rc) {
            pa_log_error("%s: sink %s create failed for profile %s, error %d ", __func__, sink->name, profile_name, rc);
            handle = NULL;
            continue;
        }

        pa_hashmap_put(u->sink_handles, sink->name, handle);

    }

    return rc;
}

static void pa_qahw_card_free_sinks(struct userdata *u, const char *profile_name) {
    pa_qahw_sink_config *sink;
    void *state;
    pa_qahw_sink_handle_t *sink_handle;

    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if (!(pa_hashmap_get(sink->profiles, profile_name)))
            continue;

        sink_handle = pa_hashmap_get(u->sink_handles, sink->name);

        if (sink_handle) {
            if (u->effect_handle != NULL)
                pa_qahw_free_sink_effects(u->effect_handle, pa_qahw_sink_get_index(sink_handle));
            pa_qahw_sink_close(sink_handle);
            pa_hashmap_remove(u->sink_handles, sink->name);
        }
    }
}

int pa__init(pa_module *m) {
    struct userdata *u;
    pa_modargs *ma;
    char *dbus_path;
    pa_dbus_protocol *dbus_protocol = NULL;

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

    u->module_name = pa_xstrdup(pa_modargs_get_value(ma, "module", QAHW_MODULE_ID_PRIMARY));

    if (pa_streq(u->module_name, QAHW_MODULE_ID_PRIMARY)) {
        pa_log_debug("Loading qahw module %s ", u->module_name);
    } else {
        pa_log_error("Unsupported module_name %s", u->module_name);
        goto fail;
    }

    u->conf_dir_name = pa_xstrdup(pa_modargs_get_value(ma, "conf_dir_name", NULL));
    u->conf_file_name = pa_xstrdup(pa_modargs_get_value(ma, "conf_file_name", NULL));

    u->config_data = pa_qahw_config_parse_new(u->conf_dir_name, u->conf_file_name);
    if (!u->config_data) {
        pa_log_error("%s: pa_qahw_config_parse_new failed", __func__);
        goto fail;
    }

    u->module_handle = qahw_load_module(u->module_name);
    if (PA_UNLIKELY(u->module_handle == NULL)) {
        pa_log_error("module %s load failed", u->module_name);
        goto fail;
    }

    pa_qahw_card_create(u);

    if (!u->config_data->default_profile) {
        pa_log_info("%s: default profile not present in card conf", __func__);
        u->config_data->default_profile = (char *)DEFAULT_PROFILE;
    }

    pa_qahw_sink_module_init();
    if (pa_hashmap_size(u->config_data->sinks)) {
        u->sink_handles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

        if (PA_UNLIKELY(pa_qahw_card_create_sinks(u, u->config_data->default_profile, PA_QAHW_CARD_USECASE_TYPE_STATIC)))
            goto fail;

    }

    pa_qahw_card_enable_jack_detection(u);

    pa_log_info("%s: using default profile %s", __func__, u->config_data->default_profile);
    pa_log_info("%s: use_dolby_hw_loopback %d", __func__, u->config_data->use_dolby_hw_loopback);

    if (pa_hashmap_size(u->config_data->sources)) {
        u->source_handles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        if (PA_UNLIKELY(pa_qahw_card_create_sources(u, u->config_data->default_profile, PA_QAHW_CARD_USECASE_TYPE_STATIC)))
            goto fail;
    }

    pa_qahw_module_extn_init(u->core, u->card, u->module_handle);
    pa_qahw_loopback_init(u->module_handle, u->core, u->card, u->config_data->loopbacks);

    pa_log_debug("module %s loaded handle %p", u->module_name, u->module_handle);

    dbus_path = pa_sprintf_malloc("%s/%s", QAHW_EFFECT_OBJECT_PATH, QAHW_MODULE_PRIMARY);
    dbus_protocol = pa_dbus_protocol_get(u->core);
    u->effect_handle = pa_qahw_init_effect(dbus_path, dbus_protocol, u->config_data->effects, u->card);

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

    pa_qahw_module_extn_deinit();

    if (u->effect_handle) {
        pa_qahw_deinit_effect(u->effect_handle);
        u->effect_handle = NULL;
    }

    pa_qahw_loopback_deinit();

    if (u->sink_handles) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            pa_qahw_card_free_sinks(u, profile->name);

        pa_hashmap_free(u->sink_handles);
    }

    pa_qahw_sink_module_deinit();

    if (u->source_handles) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            pa_qahw_card_free_sources(u, profile->name);

        pa_hashmap_free(u->source_handles);
    }

    if (u->module_handle)
        qahw_unload_module(u->module_handle);

    if (u->jack_handle_list_head)
        pa_qahw_card_disable_jack_detection(u->jack_handle_list_head, u->module_handle);

    pa_qahw_card_free(u);

    if (u->config_data)
        pa_qahw_config_parse_free(u->config_data);

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
