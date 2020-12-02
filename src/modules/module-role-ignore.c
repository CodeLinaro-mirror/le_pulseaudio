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

PA_MODULE_DESCRIPTION("Mute & ignore streams with certain roles while others exist");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "ignore_roles=<Comma separated list of roles which will be ignored> "
        "active_roles=<Comma separated list of active roles> "
        "global=<Should we operate globally or only inside the same device?>");

static const char* const valid_modargs[] = {
    "ignore_roles",
    "active_roles",
    "global",
    NULL
};

struct group {
    char *name;
    pa_idxset *active_roles;
    pa_idxset *ignore_roles;
};

struct userdata {
    pa_core *core;
    uint32_t n_groups;
    struct group **groups;
    bool global:1;
    pa_hook_slot
        *sink_input_put_slot,
        *sink_input_state_changed_slot,
        *sink_input_mute_changed_slot;
};

static void process_on_sink(struct userdata *u, pa_sink *s, pa_sink_input *i, struct group *g){
    pa_sink_input *si;
    const char *role, *active_role;
    uint32_t role_idx, idx;
    bool ignore = false;

    PA_IDXSET_FOREACH(si, s->inputs, idx) {
        if (si == i) {
            pa_log_debug("same sink input");
            continue;
        }

        if (!(role = pa_proplist_gets(si->proplist, PA_PROP_MEDIA_ROLE)))
            role = "no_role";

        PA_IDXSET_FOREACH(active_role, g->active_roles, role_idx) {
            if ((ignore = pa_streq(role, active_role))) {
                /* Check if the stream with active role is playing/unmuted */
                if ((pa_sink_input_get_state(si) == PA_SINK_INPUT_RUNNING) && !(si->muted)) {
                    pa_log_debug("There is a active stream with active role %s and alive", role);
                    /* Set the ignore stream status as mute as there is a active stream */
                    pa_sink_input_set_mute(i, true, false);
                    return;
                }
            }
        }
    }
}

static pa_hook_result_t process(struct userdata *u, pa_sink_input *i) {
    const char *role, *ignore_role;
    uint32_t j, role_idx, idx;

    /* Check if there is a sink input of any of the ignore types */
    if ( !(role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE)))
        role = "no role";

    for (j = 0; j < u->n_groups; j++) {
        PA_IDXSET_FOREACH(ignore_role, u->groups[j]->ignore_roles, role_idx) {
            if (pa_streq(role, ignore_role)) {
                struct group *g = u->groups[j];
                pa_sink *s = i->sink;

                pa_log_debug("The stream is with matching ignore role %s", ignore_role);
                /* Check on all available sinks for the streams matching the active role */
                if (u->global) {
                    PA_IDXSET_FOREACH(s, u->core->sinks, idx)
                        process_on_sink(u, s, i, g);
                } else {
                    process_on_sink(u, s, i, g);
                }
                break;
            }
        }
    }
    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (!i->sink || i->muted)
        return PA_HOOK_OK;

    return process(u, i);
}

static pa_hook_result_t sink_input_state_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(i)) && !(i->muted))
        return process(u, i);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_mute_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(i)) && !(i->muted))
        return process(u, i);

    return PA_HOOK_OK;
}

static int parse_group_roles(const char *roles_in_group, pa_idxset *role_set) {
    const char *split_state = NULL;
    char *n = NULL;

    while ((n = pa_split(roles_in_group, ",", &split_state))) {
        if (n[0] != '\0')
            pa_idxset_put(role_set, n, NULL);
        else {
            pa_log("empty role");
            pa_xfree(n);
            return -1;
        }
    }
    return 0;
}

static int get_group_count(const char *roles) {
    int grp_cnt = 0;
    if (roles) {
        const char *split_state = NULL;
        char *n = NULL;
        while ((n = pa_split(roles, "/", &split_state))) {
            grp_cnt++;
            pa_xfree(n);
        }
    }
    return grp_cnt;
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *roles;
    char *roles_in_group = NULL;
    bool global = false;
    uint32_t i = 0;
	int group_count_ac = 0, group_count_ig = 0;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;

    /* Parse args to groups */
    u->n_groups = 1;
    roles = pa_modargs_get_value(ma, "active_roles", NULL);
    if (roles) {
        group_count_ac = get_group_count(roles);
    }

    roles = pa_modargs_get_value(ma, "ignore_roles", NULL);
    if (roles) {
        group_count_ig = get_group_count(roles);
    }

    if ((group_count_ac > 1 || group_count_ig > 1) && (group_count_ac != group_count_ig)) {
        pa_log("Invalid number of groups");
        goto fail;
    }

    if (group_count_ig > 0)
        u->n_groups = group_count_ig;

    /* Allocate memory for the groups identified */
    u->groups = pa_xnew0(struct group*, u->n_groups);

    for (i = 0; i < u->n_groups; i++) {
        u->groups[i] = pa_xnew0(struct group, 1);
        u->groups[i]->active_roles = pa_idxset_new(NULL, NULL);
        u->groups[i]->ignore_roles = pa_idxset_new(NULL, NULL);
        u->groups[i]->name = pa_sprintf_malloc("ignore_group_%u", i);
    }

    roles = pa_modargs_get_value(ma, "active_roles", NULL);
    if (roles) {
        const char *group_split_state = NULL;
        i = 0;

        while ((roles_in_group = pa_split(roles, "/", &group_split_state))) {
            if (roles_in_group[0] != '\0') {
                if (parse_group_roles(roles_in_group, u->groups[i]->active_roles) < 0)
                    goto fail;
                i++;
            } else {
                pa_log("empty active roles");
                goto fail;
            }
            pa_xfree(roles_in_group);
        }
    }

    if (pa_idxset_isempty(u->groups[0]->active_roles)) {
        pa_log_debug("no active role provided");
        goto fail;
    }

    roles = pa_modargs_get_value(ma, "ignore_roles", NULL);
    if (roles) {
        const char *group_split_state = NULL;
        i = 0;

        while ((roles_in_group = pa_split(roles, "/", &group_split_state))) {
            if (roles_in_group[0] != '\0') {
                if (parse_group_roles(roles_in_group, u->groups[i]->ignore_roles) < 0)
                    goto fail;
                i++;
            } else {
                pa_log("empty ignore roles");
                goto fail;
            }
            pa_xfree(roles_in_group);
        }
    }

    if (pa_idxset_isempty(u->groups[0]->ignore_roles)) {
        pa_log_debug("no ignore roles provided");
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
    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(m);
    if (ma)
        pa_modargs_free(ma);
    if (roles_in_group)
        pa_xfree(roles_in_group);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->groups) {
        uint32_t j;
        for (j = 0; j < u->n_groups; j++) {
            pa_idxset_free(u->groups[j]->active_roles, pa_xfree);
            pa_idxset_free(u->groups[j]->ignore_roles, pa_xfree);
            pa_xfree(u->groups[j]->name);
            pa_xfree(u->groups[j]);
        }
        pa_xfree(u->groups);
    }

    if (u->sink_input_put_slot)
        pa_hook_slot_free(u->sink_input_put_slot);
    if (u->sink_input_state_changed_slot)
        pa_hook_slot_free(u->sink_input_state_changed_slot);
    if (u->sink_input_mute_changed_slot)
        pa_hook_slot_free(u->sink_input_mute_changed_slot);
    pa_xfree(u);
}
