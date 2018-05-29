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
#include <pulsecore/modargs.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-sink.h"
#include "qahw-source.h"
#include "qahw-utils.h"
#include "qahw-jack.h"

#define QAHW_MODULE_ID_PRIMARY "audio.primary"
#define QAHW_CARD_NAME_PREFIX "qahw."
#define DEFAULT_PROFILE "default"

#define PA_DEFAULT_SINK_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_SINK_RATE 48000
#define PA_DEFAULT_SINK_CHANNELS 2

#define PA_DEFAULT_SINK_DEVICE AUDIO_DEVICE_OUT_WIRED_HEADPHONE

#define PA_DEFAULT_SOURCE_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_SOURCE_RATE 48000
#define PA_DEFAULT_SOURCE_CHANNELS 2
#define PA_DEFAULT_SOURCE_DEVICE AUDIO_DEVICE_IN_BUILTIN_MIC

PA_MODULE_AUTHOR("QTI");
PA_MODULE_DESCRIPTION("qahw card module");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

/* We don't have any module arguments */
static const char* const valid_modargs[] = {
    "module",
    NULL
};

struct qahw_card_ports {
    const char *profile_name;
    pa_device_port_new_data data;
    unsigned priority;
    audio_devices_t qahw_port; /* qahw device */
};

struct qahw_card_profile_usecases {
    const char *profile_name;
    int flags; /* sink or src flags */
    pa_sample_spec ss;
    uint32_t default_device;
};

struct userdata {
    pa_core *core;
    pa_card *card;
    char *module_name;
    pa_module *module;
    pa_hashmap *profiles;
    pa_modargs *modargs;

    pa_sample_spec ss;
    pa_channel_map map;

    struct qahw_card_ports *qahw_ports;
    qahw_module_handle_t *module_handle;
    uint32_t sink_devices;
    sink_handle_t **sink_handle;
    source_handle_t **source_handle;
    uint32_t src_devices;
    int max_supported_sinks;
    int max_supported_sources;

    pa_qahw_jack_handle_t *jack_handle;
};

/* FIXME: this will have to come from configuration at some point */
static const pa_card_profile qahw_card_profiles[] = {
    {(pa_card *)NULL, (char *)"default", (char *)"Default qahw profile", NULL, NULL, 10, PA_AVAILABLE_YES, 3, 2, 8, 8},
};

/* FIXME: this will have to come from configuration at some point */
static const struct qahw_card_ports qahw_ports[] = {
    {"default", {(char *)"speaker", (char *)"speaker", PA_AVAILABLE_YES, PA_DIRECTION_OUTPUT}, 100, AUDIO_DEVICE_OUT_SPEAKER},
    {"default", {(char *)"headset", (char *)"wired headset", PA_AVAILABLE_NO, PA_DIRECTION_OUTPUT}, 500, AUDIO_DEVICE_OUT_WIRED_HEADSET},
    {"default", {(char *)"headphone", (char *)"wired headphone", PA_AVAILABLE_NO, PA_DIRECTION_OUTPUT}, 300,  AUDIO_DEVICE_OUT_WIRED_HEADPHONE},
    {"default", {(char *)"lineout", (char *)"lineout", PA_AVAILABLE_NO, PA_DIRECTION_OUTPUT}, 200, AUDIO_DEVICE_OUT_LINE},
    {"default", {(char *)"headset-mic", (char *)"wired headset mic", PA_AVAILABLE_NO, PA_DIRECTION_INPUT}, 500, AUDIO_DEVICE_IN_WIRED_HEADSET},
    {"default", {(char *)"builtin-mic", (char *)"builtin mic", PA_AVAILABLE_YES, PA_DIRECTION_INPUT}, 100, AUDIO_DEVICE_IN_BUILTIN_MIC},
    {"default", {(char *)"hdmi-in", (char *)"hdmi input", PA_AVAILABLE_NO, PA_DIRECTION_INPUT}, 50, AUDIO_DEVICE_IN_HDMI},
};

struct qahw_card_profile_usecases profile_sinks[] = {
    {"default", AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_NON_BLOCKING, {PA_DEFAULT_SINK_FORMAT, PA_DEFAULT_SINK_RATE, PA_DEFAULT_SINK_CHANNELS}, PA_DEFAULT_SINK_DEVICE},
    {"default", AUDIO_OUTPUT_FLAG_FAST, {PA_DEFAULT_SINK_FORMAT, PA_DEFAULT_SINK_RATE, PA_DEFAULT_SINK_CHANNELS}, PA_DEFAULT_SINK_DEVICE},
    {"default", AUDIO_OUTPUT_FLAG_RAW, {PA_DEFAULT_SINK_FORMAT, PA_DEFAULT_SINK_RATE, PA_DEFAULT_SINK_CHANNELS}, PA_DEFAULT_SINK_DEVICE},
};

struct qahw_card_profile_usecases profile_sources[] = {
    {"default", AUDIO_INPUT_FLAG_FAST, {PA_DEFAULT_SOURCE_FORMAT, PA_DEFAULT_SOURCE_RATE, PA_DEFAULT_SOURCE_CHANNELS}, PA_DEFAULT_SOURCE_DEVICE},
    {"default", AUDIO_INPUT_FLAG_NONE, {PA_DEFAULT_SOURCE_FORMAT, PA_DEFAULT_SOURCE_RATE, PA_DEFAULT_SOURCE_CHANNELS}, PA_DEFAULT_SOURCE_DEVICE},
};

static void pa_qahw_jack_callback(pa_qahw_jack_event_t event, pa_qahw_jack_event_data_t *event_data, void *prv_data) {
    const char *port_name = NULL;
    pa_available_t status = PA_AVAILABLE_UNKNOWN;
    pa_device_port *port;
    pa_card *card;

    pa_assert(prv_data);

    card = (pa_card *)prv_data;

    if ((event != PA_QAHW_JACK_AVAILABLE) && (event != PA_QAHW_JACK_UNAVAILABLE)) {
        pa_log_error("unsupport qahw jack event");
        return;
    }

    pa_assert(event_data);

    if (event_data->jack_type == PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS) {
        pa_log_info("PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS not supported currently");
        return;
    }

    status = (event == PA_QAHW_JACK_AVAILABLE) ? PA_AVAILABLE_YES: PA_AVAILABLE_NO;

    port_name = pa_qahw_jack_type_to_port_name(event_data->jack_type);
    if (port_name != NULL) {
        pa_log_info("port %s satus %d", port_name, status);
        port = pa_hashmap_get(card->ports, port_name);
        if (port)
            pa_device_port_set_available(port, status);
        else
            pa_log_error("unsupported port %s", port_name);

        /* for headset, change status of headset-mic as well */
        if (pa_streq(port_name, "headset")) {
            port = pa_hashmap_get(card->ports, "headset-mic");
            if (port)
                pa_device_port_set_available(port, status);
        }
    } else {
        pa_log_error("unsupport jack type %d", event_data->jack_type);
    }

    return;
}

static void jack_detection_disable(pa_qahw_jack_handle_t *jhandle) {
    pa_assert(jhandle);

    pa_qahw_jack_disable(jhandle);

    return;
}

static void jack_detection_enable(struct userdata *u) {
    int rc;
    pa_qahw_jack_handle_t *jack_handle;
    pa_qahw_jack_type_t jack_types = PA_QAHW_JACK_TYPE_INVALID;

    if (pa_hashmap_get(u->card->ports,"headset") || pa_hashmap_get(u->card->ports,"headphone") || pa_hashmap_get(u->card->ports,"headset-mic"))
        jack_types = PA_QAHW_JACK_TYPE_WIRED_HEADSET | PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS | PA_QAHW_JACK_TYPE_WIRED_HEADPHONE;

    if (pa_hashmap_get(u->card->ports,"lineout"))
        jack_types |= PA_QAHW_JACK_TYPE_LINEOUT;

    /*TODO: Add HDMI/SPDIF later */

    if (jack_types == PA_QAHW_JACK_TYPE_INVALID)
        pa_log_error("skipping jack enable as PA_QAHW_JACK_TYPE_INVALID");

    rc = pa_qahw_jack_enable(u->module, jack_types, pa_qahw_jack_callback, &jack_handle, (void *)u->card);
    if (rc) {
        pa_log_error("enable qahw jack failed %d", rc);
        u->jack_handle = NULL;
    } else {
        u->jack_handle = jack_handle;
    }

}

static void free_qahw_card_profiles(struct userdata *u, pa_hashmap *profiles) {
    pa_card_profile *p;
    void *state;

    PA_HASHMAP_FOREACH(p, profiles, state) {
        pa_hashmap_remove_and_free(profiles, p->name);
    }
}

static void create_qahw_card_profiles(struct userdata *u, pa_hashmap *profiles) {
    int32_t profile_num;
    int32_t idx;
    pa_card_profile *p = NULL;

    profile_num = sizeof(qahw_card_profiles)/sizeof(qahw_card_profiles[0]);

    for (idx = 0; idx < profile_num; idx++) {
        p = pa_card_profile_new(qahw_card_profiles[idx].name, qahw_card_profiles[idx].description, 0);

        pa_log_debug("profile created %s", p->name);

        p->priority = qahw_card_profiles[idx].priority;
        p->n_sinks = qahw_card_profiles[idx].n_sinks; /* low latency and pcm offload */
        p->n_sources = qahw_card_profiles[idx].n_sources;
        p->max_sink_channels = qahw_card_profiles[idx].max_sink_channels;
        p->max_source_channels = qahw_card_profiles[idx].max_source_channels;
        p->available =  qahw_card_profiles[idx].available;

        pa_hashmap_put(profiles, p->name, p);
    }
}

static void free_qahw_card_ports(struct userdata *u) {
    if (u->qahw_ports)
        pa_xfree(u->qahw_ports);
}

static void create_qahw_card_ports(struct userdata *u, pa_hashmap *ports, pa_hashmap *profiles) {
    pa_device_port *port;
    pa_device_port_new_data port_data;
    pa_card_profile *profile = NULL;
    audio_devices_t *qahw_port = NULL;
    int idx;
    int port_count;

    pa_assert(u);
    pa_assert(ports);
    pa_assert(profiles);

    port_count = sizeof(qahw_ports) / sizeof(qahw_ports[0]);
    u->qahw_ports =  pa_xnew0(struct qahw_card_ports, port_count);
    memcpy(u->qahw_ports, &qahw_ports[0], sizeof(struct qahw_card_ports) * port_count);

    for (idx = 0; idx < port_count; idx++) {
        if (!(profile = pa_hashmap_get(profiles, u->qahw_ports[idx].profile_name))) {
            /* Skip adding port if profile is not yet created */
            pa_log_debug("Skipping port %s for non-existent profile %s", u->qahw_ports[idx].data.name,
                    u->qahw_ports[idx].profile_name);
            continue;
        }

        pa_device_port_new_data_init(&port_data);

        pa_device_port_new_data_set_name(&port_data, u->qahw_ports[idx].data.name);
        pa_device_port_new_data_set_description(&port_data, u->qahw_ports[idx].data.description);
        pa_device_port_new_data_set_direction(&port_data, u->qahw_ports[idx].data.direction);
        pa_device_port_new_data_set_available(&port_data, u->qahw_ports[idx].data.available);

        port = pa_device_port_new(u->core, &port_data, sizeof(audio_devices_t));

        qahw_port = PA_DEVICE_PORT_DATA(port);
        *qahw_port = u->qahw_ports[idx].qahw_port;

        port->priority = u->qahw_ports[idx].priority;

        /* Sanity check that we don't have duplicates */
        pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);

        pa_device_port_new_data_done(&port_data);

        /* Add port to a profile */
        pa_hashmap_put(port->profiles, profile->name, profile);
    }
}

static int card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    pa_log_error("profile change not supported yet");
    return 0;
}

static void free_qahw_card(struct userdata *u) {
    pa_assert(u);

    free_qahw_card_ports(u);

    if (u->card)
        pa_card_free(u->card);
}

/* create port and profile and adds it card */
static int create_qahw_card(struct userdata *u) {
    pa_card_new_data data;
    pa_card_profile *profile;

    pa_assert(u);

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;
    data.name =  pa_sprintf_malloc("%s%s", QAHW_CARD_NAME_PREFIX, u->module_name);
    data.namereg_fail = true;

    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Card for the %s HAL module", u->module_name);

    /* TODO: Do we need to add a proplist? */
    create_qahw_card_profiles(u, data.profiles);
    create_qahw_card_ports(u, data.ports, data.profiles);

    u->card = pa_card_new(u->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card) {
        pa_log_error("Failed to allocate card.");
        free_qahw_card_profiles(u, data.profiles);
        return -1;
    }

    u->card->userdata = u;
    u->card->set_profile = card_set_profile;

    profile = pa_hashmap_get(u->card->profiles, DEFAULT_PROFILE);
    if (!profile) {
        pa_log("profile not found");
        free_qahw_card(u);
        return -1;
    }

    pa_card_set_profile(u->card, profile, false);

    pa_card_put(u->card);

    return 0;
}

static int create_card_sources(struct userdata *u, const char *driver, const char *profile_name) {
    int32_t source_idx, rc = -1;
    pa_channel_map map;
    source_handle_t *handle;

    for (source_idx = 0; source_idx < u->max_supported_sources; source_idx++) {
        if (!pa_streq(profile_sources[source_idx].profile_name, profile_name))
            continue;

        pa_channel_map_init_auto(&map, profile_sources[source_idx].ss.channels, PA_CHANNEL_MAP_DEFAULT);

        rc = create_source(u->module, u->card, driver, u->module_handle, u->module_name, profile_name, &(profile_sources[source_idx].ss), &map,
                profile_sources[source_idx].default_device, profile_sources[source_idx].flags, source_idx, &handle);
        if (PA_UNLIKELY(rc)) {
            pa_log_error("source create failed for profile %s, error %d ", profile_sources[source_idx].profile_name, rc);
            handle = NULL;
        }

        u->source_handle[source_idx] = handle;
    }

    return rc;
}

static void close_card_sources(struct userdata *u, const char *profile_name) {
    int source_idx;

    for (source_idx = 0; source_idx < u->max_supported_sources; source_idx++) {
        if (!pa_streq(profile_sources[source_idx].profile_name, profile_name))
            continue;

        if (u->source_handle[source_idx]) {
            close_source(u->source_handle[source_idx]);
            u->source_handle[source_idx] = NULL;
        }
    }
}

static int create_card_sinks(struct userdata *u, const char *driver, const char *profile_name) {
    int32_t sink_idx, rc = 0;
    pa_channel_map map;
    sink_handle_t *handle;

    pa_log_info("ss.format %d ss.rate %d ss.channels %d",u->ss.format, u->ss.rate, u->ss.channels);

    for (sink_idx = 0; sink_idx < u->max_supported_sinks; sink_idx++) {
        if (!pa_streq(profile_sinks[sink_idx].profile_name, profile_name))
            continue;

        pa_channel_map_init_auto(&map, PA_DEFAULT_SINK_CHANNELS, PA_CHANNEL_MAP_DEFAULT);

        rc = create_sink(u->module, u->card, driver, u->module_handle, u->module_name, profile_name, &(profile_sinks[sink_idx].ss), &map,
                         profile_sinks[sink_idx].default_device, profile_sinks[sink_idx].flags, sink_idx, &handle);
        if (PA_UNLIKELY(rc)) {
            pa_log_error("sink create failed for profile %s, error %d ", profile_sinks[sink_idx].profile_name, rc);
            handle = NULL;
        }

        u->sink_handle[sink_idx] = handle;
    }

    return rc;
}

static void close_card_sinks(struct userdata *u, const char *profile_name) {
    int sink_idx;

    for (sink_idx = 0; sink_idx < u->max_supported_sinks; sink_idx++) {
        if (!pa_streq(profile_sinks[sink_idx].profile_name, profile_name))
            continue;

        if (u->sink_handle[sink_idx]) {
            close_sink(u->sink_handle[sink_idx]);
            u->sink_handle[sink_idx] = NULL;
        }
    }
}

int pa__init(pa_module *m) {
    struct userdata *u;
    pa_modargs *ma;

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

    u->module_name = pa_xstrdup(pa_modargs_get_value(ma, "module", QAHW_MODULE_ID_PRIMARY));

    if (pa_streq(u->module_name, QAHW_MODULE_ID_PRIMARY)) {
        pa_log_debug("Loading qahw module %s ", u->module_name);
    } else {
        pa_log_error("Unsupported module_name %s", u->module_name);
        goto fail;
    }

    u->module_handle = qahw_load_module(u->module_name);
    if (PA_UNLIKELY(u->module_handle == NULL)) {
        pa_log_error("module %s load failed", u->module_name);
        goto fail;
    }

    create_qahw_card(u);

    u->max_supported_sinks = ARRAY_SIZE(profile_sinks);
    u->sink_handle = pa_xnew0(sink_handle_t *, u->max_supported_sinks);

    if (PA_UNLIKELY(create_card_sinks(u, __FILE__, DEFAULT_PROFILE)))
        goto fail;

    u->max_supported_sources = ARRAY_SIZE(profile_sources);;
    u->source_handle = pa_xnew0(source_handle_t *, u->max_supported_sources);

    if (PA_UNLIKELY(create_card_sources(u, __FILE__, DEFAULT_PROFILE)))
        goto fail;

    jack_detection_enable(u);

    pa_log_debug("module %s loaded handle %p", u->module_name, u->module_handle);

    return 0;

fail:
    pa__done(m);
    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;
    pa_card_profile *profile;
    void *state;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_handle) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            close_card_sinks(u, profile->name);

        pa_xfree(u->sink_handle);
    }

    if (u->source_handle) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            close_card_sources(u, profile->name);

        pa_xfree(u->source_handle);
    }

    if (u->module_handle)
        qahw_unload_module(u->module_handle);

    if (u->jack_handle)
        jack_detection_disable(u->jack_handle);

    free_qahw_card(u);

    pa_log_debug("module %s unloaded", u->module_name);

    pa_xfree(u->module_name);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    pa_xfree(u);
}
