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
#include <pulse/volume.h>

#include <pulsecore/macro.h>
#include <pulsecore/core.h>

#include <pulsecore/hook-list.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/modargs.h>

PA_MODULE_DESCRIPTION("pause and/or stops active streams with certain roles when on new stream with same role");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "exclusive_roles=<Comma separated list of roles which will be exclusive> "
        "global=<Should we operate globally or only inside the same device?>");

static const char* const valid_modargs[] = {
    "exclusive_roles",
    "global",
    NULL
};

struct userdata {
    pa_core *core;
    uint32_t n_groups;
    pa_idxset *exclusive_roles;
    bool global;
    pa_hashmap *inputs_states;
    pa_hook_slot
        *sink_input_put_slot,
        *sink_input_state_changed_slot,
        *sink_input_mute_changed_slot,
        *sink_input_unlink_slot;
};

static void process_on_sink(struct userdata *u, pa_sink *s, pa_sink_input *i, const char *exclusive_role){
    pa_sink_input *si;
    const char *role;
    uint32_t idx;

    PA_IDXSET_FOREACH(si, s->inputs, idx) {
        if (si == i) {
            /* same sink input */
            continue;
        }

        if (!(role = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_ROLE)))
            role = "no_role";

        if (pa_streq(role, exclusive_role)) {
            /* Check if the stream with active role is playing/unmuted */
            if ((si->state == PA_SINK_INPUT_RUNNING) && !(si->muted)) {
                pa_log_debug("There is a active stream with exclusive role %s alive", role);
                /* Set the active stream status as mute and pause the stream */
                pa_sink_input_set_mute(si, true, false);
                pa_hashmap_put(u->inputs_states, si, PA_INT_TO_PTR(1));
                pa_sink_input_send_event(si, PA_STREAM_EVENT_REQUEST_CORK, NULL);
                return;
            }
        }
    }
}

static pa_hook_result_t process(struct userdata *u, pa_sink_input *i) {
    const char *role, *exclusive_role;
    uint32_t role_idx, idx;

    /* Check if there is a sink input of any of the exclusive types */
    if ( !(role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE)))
        role = "no role";

    PA_IDXSET_FOREACH(exclusive_role, u->exclusive_roles, role_idx) {
        if (pa_streq(role, exclusive_role)) {
            pa_sink *s = i->sink;
            pa_log_debug("The stream is with matching exclusive role %s", exclusive_role);

            if (u->global) {
                PA_IDXSET_FOREACH(s, u->core->sinks, idx)
                    process_on_sink(u, s, i, role);
            } else {
                process_on_sink(u, s, i, role);
            }
            break;
        }
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (!i->sink)
        return PA_HOOK_OK;

    return process(u, i);
}

static pa_hook_result_t sink_input_state_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (i->state == PA_SINK_INPUT_CORKED) {
        if(pa_hashmap_get(u->inputs_states, i)) {
            pa_sink_input_set_mute(i, false, false);
            pa_hashmap_remove(u->inputs_states, i);
        }
        return PA_HOOK_OK;
    }

    if (PA_SINK_INPUT_IS_LINKED(i->state) && !(i->muted))
        return process(u, i);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_mute_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (!(i->muted)) {
        if(pa_hashmap_get(u->inputs_states, i)) {
            pa_hashmap_remove(u->inputs_states, i);
        }
    }

    if (PA_SINK_INPUT_IS_LINKED(i->state) && (i->state != PA_SINK_INPUT_CORKED) && !(i->muted))
        return process(u, i);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if(pa_hashmap_get(u->inputs_states, i)) {
        pa_hashmap_remove(u->inputs_states, i);
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *roles;
    bool global = false;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->exclusive_roles = pa_idxset_new(NULL, NULL);
    u->inputs_states = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    roles = pa_modargs_get_value(ma, "exclusive_roles", NULL);
    if (roles) {
        char *n = NULL;
        const char *split_state = NULL;

        while ((n = pa_split(roles, ",", &split_state))) {
            if (n[0] != '\0')
                pa_idxset_put(u->exclusive_roles, n, NULL);
            else {
                pa_log("empty excusive role");
                pa_xfree(n);
                goto fail;
            }
        }
    } else {
        pa_log("No excusive_roles arg provided");
        goto fail;
    }

    if (pa_idxset_isempty(u->exclusive_roles)) {
        pa_log_debug("no exclusive roles provided");
        goto fail;
    }
    if (pa_modargs_get_value_boolean(ma, "global", &global) < 0) {
        pa_log("Invalid boolean parameter: global");
        goto fail;
    }
    u->global = global;

    u->sink_input_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_put_cb, u);
    u->sink_input_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_state_changed_cb, u);
    u->sink_input_mute_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MUTE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_mute_changed_cb, u);
    u->sink_input_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);
    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);
    return -1;
}

void pa__done(pa_module *m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    pa_hashmap_free(u->inputs_states);
    pa_idxset_free(u->exclusive_roles, pa_xfree);

    if (u->sink_input_put_slot)
        pa_hook_slot_free(u->sink_input_put_slot);
    if (u->sink_input_state_changed_slot)
        pa_hook_slot_free(u->sink_input_state_changed_slot);
    if (u->sink_input_mute_changed_slot)
        pa_hook_slot_free(u->sink_input_mute_changed_slot);
    if (u->sink_input_unlink_slot)
        pa_hook_slot_free(u->sink_input_unlink_slot);
    pa_xfree(u);
}
