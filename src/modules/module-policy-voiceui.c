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

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/modargs.h>

#include "qsthw-util.h"

#define DEFAULT_SINK_NAME "offload0"
#define NULL_SOURCE_NAME "qsthw_null_src"
#define QSTHW_ROLE "voice-ui-session"

PA_MODULE_DESCRIPTION("Detects the start of a qsthw LAB session and applies the respective policy for incoming stream request");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE("");

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_module *null_src_module;
    pa_module *loop_back_module;
    pa_qsthw_hooks *qsthw;
    pa_hook_slot *qsthw_detection_session_start_slot;
    pa_hook_slot *qsthw_detection_session_stop_slot;
    char *def_sink_name;
    char *null_src_name;
    char *role;
    uint32_t null_src_index;
    uint32_t loopback_index;
};

static pa_hook_result_t qsthw_detection_session_start_cb(pa_core *core,
               void *call_data,
               struct userdata *u) {
    char *args;
    u->def_sink_name = DEFAULT_SINK_NAME;
    u->role = QSTHW_ROLE;
    pa_source_output *source_output;
    pa_source *src;
    uint32_t idx;

    if (u->loopback_index != PA_INVALID_INDEX) {
        pa_log_debug("Already a LoopBack with role voice-ui-session created ");
        return PA_HOOK_OK;
    }

    /* Load module-loopback */
    args = pa_sprintf_malloc(
            "source=\"%s\" sink=\"%s\"  "
            "sink_input_properties=\"media.role=%s\"",
            u->null_src_name, u->def_sink_name, u->role);

    pa_module_load(&u->loop_back_module, u->core, "module-loopback", args);
    u->loopback_index = u->loop_back_module ? u->loop_back_module->index : PA_INVALID_INDEX;

    if (u->loopback_index != PA_INVALID_INDEX) {
        PA_IDXSET_FOREACH(source_output, u->core->source_outputs, idx) {
            src = pa_idxset_get_by_index (u->core->sources, source_output->source->index);
            if (src) {
                if (strcmp(u->null_src_name, source_output->source->name) == 0) {
                    pa_source_set_mute(src, true , false);
                }
            }
        }
    }

    pa_xfree(args);

    return PA_HOOK_OK;
}

static pa_hook_result_t qsthw_detection_session_stop_cb(pa_core *core, void *call_data,
        struct userdata *u) {

    if (u->loopback_index != PA_INVALID_INDEX) {
        pa_module_unload_by_index(u->core, u->loopback_index, true);
        u->loopback_index = PA_INVALID_INDEX;
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module *m) {
    struct userdata *u;
    char *args;

    pa_assert(m);

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->qsthw = pa_xnew0(pa_qsthw_hooks, 1);
    u->module = m;
    u->core = m->core;
    u->null_src_index = PA_INVALID_INDEX;
    u->loopback_index = PA_INVALID_INDEX;

    u->qsthw = pa_shared_get(m->core, "voice-ui-session");

    u->qsthw_detection_session_start_slot = pa_hook_connect(
            &u->qsthw->hooks[PA_HOOK_QSTHW_START_DETECTION], PA_HOOK_NORMAL,
            (pa_hook_cb_t)qsthw_detection_session_start_cb, u);
    u->qsthw_detection_session_stop_slot = pa_hook_connect(
            &u->qsthw->hooks[PA_HOOK_QSTHW_STOP_DETECTION], PA_HOOK_NORMAL,
            (pa_hook_cb_t)qsthw_detection_session_stop_cb, u);

    u->null_src_name = NULL_SOURCE_NAME;
    args = pa_sprintf_malloc("source_name=\"%s\"", u->null_src_name);

    if (pa_module_exists("module-null-source")) {
        pa_module_load(&u->null_src_module, m->core, "module-null-source", args);
        if (u->null_src_module)
            u->null_src_index = u->null_src_module->index;
    }

    pa_xfree(args);
    return 0;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata)) return;

    if (u->loopback_index != PA_INVALID_INDEX) {
        pa_module_unload_by_index(u->core, u->loopback_index, true);
        u->loopback_index = PA_INVALID_INDEX;
    }

    if (u->null_src_index != PA_INVALID_INDEX) {
        pa_module_unload_by_index(u->core, u->null_src_index, false);
        u->null_src_index = PA_INVALID_INDEX;
    }

    if (u->qsthw_detection_session_start_slot)
        pa_hook_slot_free(u->qsthw_detection_session_start_slot);

    if (u->qsthw_detection_session_stop_slot)
        pa_hook_slot_free(u->qsthw_detection_session_stop_slot);

    pa_xfree(u->qsthw);
    pa_xfree(u);
}
