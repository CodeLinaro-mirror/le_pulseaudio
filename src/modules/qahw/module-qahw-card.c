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

#include <string.h>

#include <qahw_api.h>
#include <qahw_defs.h>

#include "qahw-source.h"
#include "qahw-utils.h"
#include "qahw-jack.h"
#include "qahw-loopback.h"
#include "qahw-card-extn.h"
#include "qahw-effect.h"
#include "qahw-card.h"

#define CONC(A,B) (A B)
#define QAHW_MODULE_ID_PREFIX "audio."
#define QAHW_MODULE_PRIMARY "primary"

#ifndef QAHW_MODULE_ID_PRIMARY
#define QAHW_MODULE_ID_PRIMARY CONC(QAHW_MODULE_ID_PREFIX, QAHW_MODULE_PRIMARY)
#endif

#define QAHW_CARD_NAME_PREFIX "qahw."
#define DEFAULT_PROFILE "default"

#define PA_DEFAULT_SINK_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_SINK_RATE 48000
#define PA_DEFAULT_SINK_CHANNELS 2

#define PA_DEFAULT_SINK_DEVICE AUDIO_DEVICE_OUT_SPEAKER

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

typedef struct {
    pa_device_port_new_data data;
    unsigned priority;
    audio_devices_t device; /* qahw device */
} pa_qahw_card_port;

typedef struct {
    const char *profile_name;
    int flags; /* sink or src flags */
    pa_sample_spec ss;
    uint32_t default_device;
} pa_qahw_card_profile_usecase;

typedef struct {
    pa_qahw_card_usecase_id_t usecase_id;
    pa_qahw_card_usecase_type_t usecase_type;
    int flags; /* sink or src flags */
    pa_encoding_t encoding;
    pa_sample_spec ss;
    uint32_t default_device;
} pa_qahw_card_usecase_info;

typedef struct {
    char *port_name;
    char *profile_name;
} pa_qahw_card_port_to_profile_mapping;

typedef struct {
    pa_qahw_card_usecase_id_t usecase_id;
    const char *profile_name;
} pa_qahw_usecase_id_profile_mapping;

typedef struct {
    const char *port_name;
    pa_qahw_card_usecase_id_t usecase_ids;
} pa_qahw_card_port_to_usecase_id_mapping;

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
    uint32_t sink_devices;
    pa_qahw_sink_handle_t **sink_handle;
    pa_qahw_source_handle_t **source_handle;
    pa_qahw_effect_handle_t effect_handle;
    pa_qahw_effect_status *effect_status;
    uint32_t src_devices;
    uint32_t max_supported_sinks;
    uint32_t max_supported_sources;

    pa_qahw_jack_handle_t *jack_handle;
};

/* internal functions */
static int pa_qahw_card_get_usecase_id_from_sink_port(char *port_name, pa_qahw_card_sink_usecase_id_t *sink_id);
static int pa_qahw_card_get_sink_idx_from_usecase(pa_qahw_card_sink_usecase_id_t sink_id);
static int pa_qahw_card_get_source_idx_from_usecase(pa_qahw_card_source_usecase_id_t source_id);
static int pa_qahw_card_get_usecase_id_from_source_port(char *port_name, pa_qahw_card_source_usecase_id_t *source_id);

/* FIXME: this will have to come from configuration at some point */
static const pa_card_profile qahw_card_profiles[] = {
    {(pa_card *)NULL, (char *)"default", (char *)"Default qahw profile", NULL, NULL, 10, PA_AVAILABLE_YES, 3, 2, 8, 8},
};

static const pa_qahw_card_port qahw_ports[] = {
    { {(char *)"speaker", (char *)"speaker", PA_AVAILABLE_YES, PA_DIRECTION_OUTPUT}, 100, AUDIO_DEVICE_OUT_SPEAKER },
    { {(char *)"headset", (char *)"wired headset", PA_AVAILABLE_NO, PA_DIRECTION_OUTPUT}, 500, AUDIO_DEVICE_OUT_WIRED_HEADSET },
    { {(char *)"headphone", (char *)"wired headphone", PA_AVAILABLE_NO, PA_DIRECTION_OUTPUT}, 300,  AUDIO_DEVICE_OUT_WIRED_HEADPHONE },
    { {(char *)"lineout", (char *)"lineout", PA_AVAILABLE_NO, PA_DIRECTION_OUTPUT}, 200, AUDIO_DEVICE_OUT_LINE },
    { {(char *)"headset-mic", (char *)"wired headset mic", PA_AVAILABLE_NO, PA_DIRECTION_INPUT}, 500, AUDIO_DEVICE_IN_WIRED_HEADSET },
    { {(char *)"builtin-mic", (char *)"builtin mic", PA_AVAILABLE_YES, PA_DIRECTION_INPUT}, 100, AUDIO_DEVICE_IN_BUILTIN_MIC },
    { {(char *)"hdmi-in", (char *)"hdmi input", PA_AVAILABLE_NO, PA_DIRECTION_INPUT}, 50, AUDIO_DEVICE_IN_HDMI },
};

/* FIXME: this will have to come from configuration at some point */
static const pa_qahw_card_port_to_profile_mapping port_profile[] = {
    { (char *)"speaker", (char* ) "default"},
    { (char *)"headset", (char* ) "default"},
    { (char *)"headphone", (char* ) "default"},
    { (char *)"lineout", (char* ) "default"},
    { (char *)"headset-mic", (char* ) "default"},
    { (char *)"builtin-mic", (char* ) "default"},
    { (char *)"hdmi-in", (char* ) "default"},
};

static pa_qahw_card_usecase_info supported_sinks[] = {
    { { PA_QAHW_CARD_SINK_OFFLOAD_0}, PA_QAHW_CARD_USECASE_TYPE_STATIC, AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_NON_BLOCKING, PA_ENCODING_PCM, {PA_DEFAULT_SINK_FORMAT, PA_DEFAULT_SINK_RATE, PA_DEFAULT_SINK_CHANNELS}, PA_DEFAULT_SINK_DEVICE },
    { { PA_QAHW_CARD_SINK_ULL_0},  PA_QAHW_CARD_USECASE_TYPE_STATIC, AUDIO_OUTPUT_FLAG_FAST, PA_ENCODING_PCM, {PA_DEFAULT_SINK_FORMAT, PA_DEFAULT_SINK_RATE, PA_DEFAULT_SINK_CHANNELS}, PA_DEFAULT_SINK_DEVICE },
    { { PA_QAHW_CARD_SINK_LL_0}, PA_QAHW_CARD_USECASE_TYPE_STATIC, AUDIO_OUTPUT_FLAG_RAW, PA_ENCODING_PCM, {PA_DEFAULT_SINK_FORMAT, PA_DEFAULT_SINK_RATE, PA_DEFAULT_SINK_CHANNELS}, PA_DEFAULT_SINK_DEVICE },
};

static pa_qahw_card_usecase_info supported_sources[] = {
    { { PA_QAHW_CARD_SOURCE_REGULAR_0 }, PA_QAHW_CARD_USECASE_TYPE_STATIC, AUDIO_INPUT_FLAG_NONE, PA_ENCODING_PCM, {PA_DEFAULT_SOURCE_FORMAT, PA_DEFAULT_SOURCE_RATE, PA_DEFAULT_SOURCE_CHANNELS}, PA_DEFAULT_SOURCE_DEVICE },
    { { PA_QAHW_CARD_SOURCE_LL_0}, PA_QAHW_CARD_USECASE_TYPE_STATIC, AUDIO_INPUT_FLAG_FAST, PA_ENCODING_PCM, {PA_DEFAULT_SOURCE_FORMAT, PA_DEFAULT_SOURCE_RATE, PA_DEFAULT_SOURCE_CHANNELS}, PA_DEFAULT_SOURCE_DEVICE },
};

pa_qahw_effect_data sink_effects_info[] = {
    {AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_NON_BLOCKING, {true, true, true, true, false}},
    {AUDIO_OUTPUT_FLAG_FAST, {false, false, false, false, false}},
    {AUDIO_OUTPUT_FLAG_RAW, {false, false, false, false, false}},
};

pa_qahw_port_effect_data port_effects_info[] = {
    {"speaker", {false, false, false, false, true}},
    {"headset", {false, false, false, false, false}},
    {"headphone", {false, false, false, false, false}},
    {"lineout", {false, false, false, false, false}},
    {"headset-mic", {false, false, false, false, false}},
    {"builtin-mic", {false, false, false, false, false}},
    {"hdmi-in", {false, false, false, false, false}},
};

static const pa_qahw_usecase_id_profile_mapping profile_sinks[] = {
    { { PA_QAHW_CARD_SINK_OFFLOAD_0 }, "default" },
    { { PA_QAHW_CARD_SINK_ULL_0 }, "default" },
    { { PA_QAHW_CARD_SINK_LL_0 }, "default" },
};

static const pa_qahw_usecase_id_profile_mapping profile_sources[] = {
    { { PA_QAHW_CARD_SOURCE_REGULAR_0 }, "default" },
    { { PA_QAHW_CARD_SOURCE_LL_0 }, "default" },
};

static const pa_qahw_card_port_to_usecase_id_mapping sink_port_to_usecase_mapping[] = {
    { "speaker", { PA_QAHW_CARD_SINK_OFFLOAD_0| PA_QAHW_CARD_SINK_LL_0 | PA_QAHW_CARD_SINK_ULL_0 } },
    { "headset", { PA_QAHW_CARD_SINK_OFFLOAD_0| PA_QAHW_CARD_SINK_LL_0 | PA_QAHW_CARD_SINK_ULL_0 } },
    { "headphone", { PA_QAHW_CARD_SINK_OFFLOAD_0| PA_QAHW_CARD_SINK_LL_0 | PA_QAHW_CARD_SINK_ULL_0 } },
    { "lineout", { PA_QAHW_CARD_SINK_OFFLOAD_0| PA_QAHW_CARD_SINK_LL_0 | PA_QAHW_CARD_SINK_ULL_0 } },
};

static const pa_qahw_card_port_to_usecase_id_mapping source_port_to_usecase_mapping[] = {
    { "headset-mic", { PA_QAHW_CARD_SOURCE_REGULAR_0| PA_QAHW_CARD_SOURCE_LL_0 } },
    { "builtin-mic", { PA_QAHW_CARD_SOURCE_REGULAR_0| PA_QAHW_CARD_SOURCE_LL_0 } },
};

static void pa_qahw_card_fill_sink_effect_status(pa_qahw_effect_status *effect_status, pa_qahw_sink_handle_t *handle) {
    int i = 0;

    pa_assert(effect_status);
    pa_assert(handle);

    effect_status->handle = handle;
    effect_status->sink_id = pa_qahw_sink_get_index(handle);
    for (i = 0; i < PA_QAHW_EFFECT_MAX; i++)
        effect_status->effect_loaded[i] = false;
}

static void pa_qahw_card_jack_callback(pa_qahw_jack_event_t event, pa_qahw_jack_event_data_t *event_data, void *prv_data) {
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

    port_name = pa_qahw_util_jack_type_to_port_name(event_data->jack_type);
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

static void pa_qahw_card_disable_jack_detection(pa_qahw_jack_handle_t *jhandle) {
    pa_assert(jhandle);

    pa_qahw_jack_disable(jhandle);

    return;
}

static void pa_qahw_card_enable_jack_detection(struct userdata *u) {
    int rc;
    pa_qahw_jack_handle_t *jack_handle;
    pa_qahw_jack_type_t jack_types = PA_QAHW_JACK_TYPE_INVALID;

    if (pa_hashmap_get(u->card->ports,"headset") || pa_hashmap_get(u->card->ports,"headphone") || pa_hashmap_get(u->card->ports,"headset-mic"))
        jack_types = PA_QAHW_JACK_TYPE_WIRED_HEADSET | PA_QAHW_JACK_TYPE_WIRED_HEADSET_BUTTONS | PA_QAHW_JACK_TYPE_WIRED_HEADPHONE;

    if (pa_hashmap_get(u->card->ports,"lineout"))
        jack_types |= PA_QAHW_JACK_TYPE_LINEOUT;

    /*TODO: Add HDMI/SPDIF later */
    if (pa_hashmap_get(u->card->ports,"hdmi-in"))
        jack_types |= PA_QAHW_JACK_TYPE_HDMI;

    if (jack_types == PA_QAHW_JACK_TYPE_INVALID)
        pa_log_error("skipping jack enable as PA_QAHW_JACK_TYPE_INVALID");

    rc = pa_qahw_jack_enable(u->module, jack_types, pa_qahw_card_jack_callback, &jack_handle, (void *)u->card);
    if (rc) {
        pa_log_error("enable qahw jack failed %d", rc);
        u->jack_handle = NULL;
    } else {
        u->jack_handle = jack_handle;
    }
}

static void pa_qahw_card_profiles_free(struct userdata *u, pa_hashmap *profiles) {
    pa_card_profile *p;
    void *state;

    PA_HASHMAP_FOREACH(p, profiles, state) {
        pa_hashmap_remove_and_free(profiles, p->name);
    }
}

static void pa_qahw_card_create_profiles(struct userdata *u, pa_hashmap *profiles) {
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

static void pa_qahw_card_create_ports(struct userdata *u, pa_hashmap *ports, pa_hashmap *profiles) {
    pa_device_port *port;
    pa_device_port_new_data port_data;
    pa_card_profile *profile = NULL;
    pa_qahw_card_port_device_data *port_device_data = NULL;

    uint32_t port_idx;
    uint32_t profile_idx;
    int rc;

    pa_assert(u);
    pa_assert(ports);
    pa_assert(profiles);

    for (port_idx = 0; port_idx < ARRAY_SIZE(qahw_ports); port_idx++) {
        pa_device_port_new_data_init(&port_data);

        pa_device_port_new_data_set_name(&port_data, qahw_ports[port_idx].data.name);
        pa_device_port_new_data_set_description(&port_data, qahw_ports[port_idx].data.description);
        pa_device_port_new_data_set_direction(&port_data, qahw_ports[port_idx].data.direction);
        pa_device_port_new_data_set_available(&port_data, qahw_ports[port_idx].data.available);

        port = pa_device_port_new(u->core, &port_data, sizeof(pa_qahw_card_port_device_data));

        port_device_data = PA_DEVICE_PORT_DATA(port);

        port_device_data->device = qahw_ports[port_idx].device;

        /* get list of sinks support for a port */
        if (qahw_ports[port_idx].data.direction == PA_DIRECTION_OUTPUT)
            rc = pa_qahw_card_get_usecase_id_from_sink_port(qahw_ports[port_idx].data.name, &port_device_data->usecase_id.sink_id);
        else
            rc = pa_qahw_card_get_usecase_id_from_source_port(qahw_ports[port_idx].data.name, &port_device_data->usecase_id.source_id);
        if (rc) {
            pa_log_error("%s:port %s doesn't belong any src/sink direction %u", __func__, qahw_ports[port_idx].data.name, qahw_ports[port_idx].data.direction);
        }

        port->priority = qahw_ports[port_idx].priority;

        /* Sanity check that we don't have duplicates */
        pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);

        pa_device_port_new_data_done(&port_data);

        /* Add port to profiles */
        for (profile_idx = 0; profile_idx < ARRAY_SIZE(port_profile); profile_idx++) {
            if (pa_streq(qahw_ports[port_idx].data.name, port_profile[profile_idx].port_name)) {
                /*check if its valid profile */
                if (!(profile = pa_hashmap_get(profiles, port_profile[profile_idx].profile_name))) {
                    /* Skip adding port if profile is not yet created */
                    pa_log_debug("Skipping port %s for non-existent profile %s", qahw_ports[port_idx].data.name,
                            port_profile[profile_idx].profile_name);
                    continue;
                }
                pa_hashmap_put(port->profiles, profile->name, profile);
            }
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

    /* TODO: Do we need to add a proplist? */
    pa_qahw_card_create_profiles(u, data.profiles);
    pa_qahw_card_create_ports(u, data.ports, data.profiles);

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

static int pa_qahw_card_get_source_idx_from_usecase(pa_qahw_card_source_usecase_id_t source_id) {
    int idx = -1;

    for (idx =0; idx < (int)ARRAY_SIZE(supported_sources); idx++)
        if (supported_sources[idx].usecase_id.source_id == source_id)
            break;

    pa_log_debug("%s: found source index = %d for usecase 0x%x", __func__, idx, source_id);

    return idx;
}

static int pa_qahw_card_get_usecase_id_from_source_port(char *port_name, pa_qahw_card_source_usecase_id_t *source_id) {
    uint32_t idx;
    int rc = -1;

    pa_assert(port_name);
    pa_assert(source_id);

    for (idx = 0; idx < ARRAY_SIZE(source_port_to_usecase_mapping); idx++) {
        if (pa_streq(source_port_to_usecase_mapping[idx].port_name, port_name)) {
            *source_id = source_port_to_usecase_mapping[idx].usecase_ids.source_id;
            rc = 0;
            break;
        }
    }

    if (rc) {
        pa_log_error("%s: no source supported for port %s", __func__, port_name);
        *source_id = PA_QAHW_CARD_SOURCE_NONE;
        rc = -1;
    }

    return rc;
}

static int pa_qahw_card_add_source(pa_module *module, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, char *module_name,
                                 const char *profile_name, pa_qahw_card_usecase_info *usecase_info, pa_qahw_card_source_usecase_id_t source_id,
                                 pa_qahw_source_handle_t **source_handle) {
    uint32_t rc = 0;

    pa_channel_map map;

    pa_assert(module);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module);
    pa_assert(module_handle);
    pa_assert(module_name);
    pa_assert(profile_name);
    pa_assert(driver);
    pa_assert(usecase_info);

    pa_log_info("%s: ss.format %d ss.rate %d ss.channels %d", __func__, usecase_info->ss.format, usecase_info->ss.rate, usecase_info->ss.channels);

    pa_channel_map_init_auto(&map, usecase_info->ss.channels, PA_CHANNEL_MAP_DEFAULT);

    rc = pa_qahw_source_create(module, card, driver, module_handle, module_name, profile_name, usecase_info->encoding, &usecase_info->ss, &map,
            usecase_info->default_device, usecase_info->flags, source_id, source_handle);
    if (rc) {
        pa_log_error("%s: source %d create failed for profile %s, error %d ", __func__, source_id, profile_name, rc);
    }

    return rc;
}

static int pa_qahw_card_create_sources(struct userdata *u, const char *profile_name, pa_qahw_card_usecase_type_t usecase_type) {
    uint32_t rc = 0;
    int32_t source_idx;
    uint32_t profile_idx;

    pa_qahw_source_handle_t *handle;

    pa_log_info("%s: ss.format %d ss.rate %d ss.channels %d", __func__, u->ss.format, u->ss.rate, u->ss.channels);

    for (profile_idx = 0; profile_idx < ARRAY_SIZE(profile_sources); profile_idx++) {
        if (!pa_streq(profile_sources[profile_idx].profile_name, profile_name))
            continue;

        source_idx = pa_qahw_card_get_source_idx_from_usecase(profile_sources[profile_idx].usecase_id.source_id);
        if ((source_idx < 0) || source_idx >= (int)ARRAY_SIZE(supported_sources)) {
            pa_log_error("%s: unsupported source source_idx %d ", __func__, source_idx);
            continue;
        }

        if (supported_sources[source_idx].usecase_type != usecase_type)
            continue;

        rc = pa_qahw_card_add_source(u->module, u->card, u->driver, u->module_handle, u->module_name, profile_name, &supported_sources[source_idx], profile_sources[profile_idx].usecase_id.source_id, &handle);
        if (rc) {
            pa_log_error("%s: source %d create failed for profile %s, error %d ", __func__, profile_sources[profile_idx].usecase_id.source_id,
                         profile_sources[profile_idx].profile_name, rc);
            handle = NULL;
        }

        u->source_handle[source_idx] = handle;
    }

    return rc;
}

static void pa_qahw_card_remove_source(pa_qahw_source_handle_t *source_handle) {
    pa_assert(source_handle);

    pa_qahw_source_close(source_handle);
}

static void pa_qahw_card_free_sources(struct userdata *u, const char *profile_name) {
    int source_idx;
    uint32_t profile_idx;

    for (profile_idx = 0; profile_idx < ARRAY_SIZE(profile_sources); profile_idx++) {
        if (!pa_streq(profile_sources[source_idx].profile_name, profile_name))
            continue;

        source_idx = pa_qahw_card_get_source_idx_from_usecase(profile_sources[profile_idx].usecase_id.source_id);
        if ((source_idx < 0) || source_idx >= (int32_t)ARRAY_SIZE(supported_sources)) {
            pa_log_error("%s: unsupported source source_idx %d ",__func__, source_idx);
            continue;
        }

        if (u->source_handle[source_idx]) {
            pa_qahw_card_remove_source(u->source_handle[source_idx]);
            u->source_handle[source_idx] = NULL;
        }
    }
}

static int pa_qahw_card_get_usecase_id_from_sink_port(char *port_name, pa_qahw_card_sink_usecase_id_t *sink_id) {
    uint32_t idx;
    int rc = -1;

    pa_assert(port_name);
    pa_assert(sink_id);

    for (idx = 0; idx < ARRAY_SIZE(sink_port_to_usecase_mapping); idx++) {
        if (pa_streq(sink_port_to_usecase_mapping[idx].port_name, port_name)) {
            *sink_id = sink_port_to_usecase_mapping[idx].usecase_ids.sink_id;
            rc = 0;
            break;
        }
    }

    if (rc) {
        pa_log_error("%s: no sink supported for port %s", __func__, port_name);
        *sink_id = PA_QAHW_CARD_SINK_NONE;
        rc = -1;
    }

    return rc;
}

static int pa_qahw_card_get_sink_idx_from_usecase(pa_qahw_card_sink_usecase_id_t sink_id) {
    int idx = -1;

    for (idx =0; idx < (int)ARRAY_SIZE(supported_sinks); idx++)
        if (supported_sinks[idx].usecase_id.sink_id == sink_id)
            break;

    pa_log_debug("%s: found sink index = %d for usecase 0x%x", __func__, idx, sink_id);

    return idx;
}

static int pa_qahw_card_add_sink(pa_module *module, pa_card *card, const char *driver, qahw_module_handle_t *module_handle, char *module_name,
                                 const char *profile_name, pa_qahw_card_usecase_info *usecase_info, pa_qahw_card_sink_usecase_id_t sink_id,
                                 pa_qahw_sink_handle_t **sink_handle) {
    uint32_t rc = 0;

    pa_channel_map map;

    pa_assert(module);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module);
    pa_assert(module_handle);
    pa_assert(module_name);
    pa_assert(profile_name);
    pa_assert(driver);
    pa_assert(usecase_info);

    pa_log_info("%s: ss.format %d ss.rate %d ss.channels %d", __func__, usecase_info->ss.format, usecase_info->ss.rate, usecase_info->ss.channels);

    pa_channel_map_init_auto(&map, usecase_info->ss.channels, PA_CHANNEL_MAP_DEFAULT);

    rc = pa_qahw_sink_create(module, card, driver, module_handle, module_name, profile_name, &usecase_info->ss, &map,
            usecase_info->default_device, usecase_info->flags, sink_id, sink_handle);
    if (rc) {
        pa_log_error("%s: sink %d create failed for profile %s, error %d ", __func__, sink_id, profile_name, rc);
    }

    return rc;
}

static int pa_qahw_card_create_sinks(struct userdata *u, const char *profile_name, pa_qahw_card_usecase_type_t usecase_type) {
    uint32_t rc = 0;
    int32_t sink_idx;
    uint32_t profile_idx;

    pa_qahw_sink_handle_t *handle;

    pa_log_info("%s: ss.format %d ss.rate %d ss.channels %d", __func__, u->ss.format, u->ss.rate, u->ss.channels);

    for (profile_idx = 0; profile_idx < ARRAY_SIZE(profile_sinks); profile_idx++) {
        if (!pa_streq(profile_sinks[profile_idx].profile_name, profile_name))
            continue;

        sink_idx = pa_qahw_card_get_sink_idx_from_usecase(profile_sinks[profile_idx].usecase_id.sink_id);
        if ((sink_idx < 0) || sink_idx >= (int)ARRAY_SIZE(supported_sinks)) {
            pa_log_error("%s: unsupported sink sink_idx %d ", __func__, sink_idx);
            continue;
        }

        if (supported_sinks[sink_idx].usecase_type != usecase_type)
            continue;

        rc = pa_qahw_card_add_sink(u->module, u->card, u->driver, u->module_handle, u->module_name, profile_name, &supported_sinks[sink_idx], profile_sinks[profile_idx].usecase_id.sink_id, &handle);
        if (rc) {
            pa_log_error("%s: sink %d create failed for profile %s, error %d ", __func__, profile_sinks[profile_idx].usecase_id.sink_id,
                         profile_sinks[profile_idx].profile_name, rc);
            handle = NULL;
        }

        u->sink_handle[sink_idx] = handle;
        pa_qahw_card_fill_sink_effect_status(&u->effect_status[sink_idx], handle);
    }

    return rc;
}

static void pa_qahw_card_remove_sink(pa_qahw_sink_handle_t *sink_handle) {
    pa_assert(sink_handle);

    pa_qahw_sink_close(sink_handle);
}

static void pa_qahw_card_free_sinks(struct userdata *u, const char *profile_name) {
    int sink_idx;
    uint32_t profile_idx;

    for (profile_idx = 0; profile_idx < ARRAY_SIZE(profile_sinks); profile_idx++) {
        if (!pa_streq(profile_sinks[sink_idx].profile_name, profile_name))
            continue;

        sink_idx = pa_qahw_card_get_sink_idx_from_usecase(profile_sinks[profile_idx].usecase_id.sink_id);
        if ((sink_idx < 0) || sink_idx >= (int32_t)ARRAY_SIZE(supported_sinks)) {
            pa_log_error("%s: unsupported sink sink_idx %d ",__func__, sink_idx);
            continue;
        }

        if (u->sink_handle[sink_idx]) {
            pa_qahw_free_sink_effects(u->effect_handle, pa_qahw_sink_get_index(u->sink_handle[sink_idx]));
            pa_qahw_card_remove_sink(u->sink_handle[sink_idx]);
            u->sink_handle[sink_idx] = NULL;
        }
    }
    pa_xfree(u->effect_status);
}

int pa__init(pa_module *m) {
    struct userdata *u;
    pa_modargs *ma;
    char *dbus_path;
    pa_dbus_protocol *dbus_protocol = NULL;

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

    u->module_handle = qahw_load_module(u->module_name);
    if (PA_UNLIKELY(u->module_handle == NULL)) {
        pa_log_error("module %s load failed", u->module_name);
        goto fail;
    }

    pa_qahw_card_create(u);

    u->max_supported_sinks = ARRAY_SIZE(profile_sinks);
    u->sink_handle = pa_xnew0(pa_qahw_sink_handle_t *, u->max_supported_sinks);

    pa_qahw_card_enable_jack_detection(u);

    u->effect_status = (pa_qahw_effect_status *)pa_xnew0(pa_qahw_effect_status, ARRAY_SIZE(profile_sinks));

    if (PA_UNLIKELY(pa_qahw_card_create_sinks(u, DEFAULT_PROFILE, PA_QAHW_CARD_USECASE_TYPE_STATIC)))
        goto fail;

    u->max_supported_sources = ARRAY_SIZE(profile_sources);;
    u->source_handle = pa_xnew0(pa_qahw_source_handle_t *, u->max_supported_sources);

    if (PA_UNLIKELY(pa_qahw_card_create_sources(u, DEFAULT_PROFILE, PA_QAHW_CARD_USECASE_TYPE_STATIC)))
        goto fail;

    pa_qahw_module_extn_init(u->core, u ->card, u->module_handle);
    pa_qahw_loopback_init(u->module_handle, u->core, u->card);

    pa_log_debug("module %s loaded handle %p", u->module_name, u->module_handle);

    dbus_path = pa_sprintf_malloc("%s/%s", QAHW_EFFECT_OBJECT_PATH, QAHW_MODULE_PRIMARY);
    dbus_protocol = pa_dbus_protocol_get(u->core);
    u->effect_handle = pa_qahw_init_effect(dbus_path, dbus_protocol, sink_effects_info, port_effects_info,
                                           u->effect_status, u->card, u->max_supported_sinks, u->max_supported_sources);

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

    pa_qahw_module_extn_deinit();
    pa_qahw_loopback_deinit();

    if (u->sink_handle) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            pa_qahw_card_free_sinks(u, profile->name);

        pa_xfree(u->sink_handle);
    }

    pa_qahw_deinit_effect(u->effect_handle);

    if (u->source_handle) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            pa_qahw_card_free_sources(u, profile->name);

        pa_xfree(u->source_handle);
    }

    if (u->module_handle)
        qahw_unload_module(u->module_handle);

    if (u->jack_handle)
        pa_qahw_card_disable_jack_detection(u->jack_handle);

    pa_qahw_card_free(u);

    pa_log_debug("module %s unloaded", u->module_name);

    pa_xfree(u->module_name);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    pa_xfree(u);
}
