/***
    This file is part of PulseAudio.

    Copyright 2010 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>
    Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/
#include "pulsecore_config.h"

#include "group_manager.h"

#include <fcntl.h>

#include <iomanip>
#include <sstream>

#include "clock.h"
#include "enums.h"
#include "group_sink_ctrl.h"

template <>
struct is_flags<pa_sink_input_flags> : std::true_type {};
template <>
struct is_flags<pa_sink_flags_t> : std::true_type {};

enum {
    GROUP_MANAGER_SINK_SET_MASTER_ID = PA_SINK_MESSAGE_MAX
};

/* Called from I/O thread context */
static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    GroupManager *u = reinterpret_cast<GroupManager *>(PA_SINK(o)->userdata);

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:

            /* The sink is _put() before the sink input is, so let's
             * make sure we don't access it in that time. Also, the
             * sink input is first shut down, the sink second. */
            if (!PA_SINK_IS_LINKED(u->sink_->thread_info.state)
                || !PA_SINK_INPUT_IS_LINKED(u->sink_input_->thread_info.state)) {
                *reinterpret_cast<int64_t *>(data) = 0;
                return 0;
            }
            if (u->timestamp_ == PA_NSEC_INVALID) {
                *reinterpret_cast<int64_t *>(data) = static_cast<int64_t>(
                    (u->sink_->thread_info.min_latency + u->sink_->thread_info.max_latency) / 2);
            } else {
                // cast to signed before the division to correctly extend the sign bit
                *reinterpret_cast<int64_t *>(data) =
                    static_cast<int64_t>(u->timestamp_ - ts_clock_now()) / static_cast<int64_t>(PA_NSEC_PER_USEC);
            }
            pa_log_debug("latency: %lld", *reinterpret_cast<int64_t *>(data));
            return 0;

        case PA_SINK_MESSAGE_ADD_INPUT: {
            // TODO(jbing): what if there are multiple inputs?
            pa_sink_input *i = PA_SINK_INPUT(data);

            const char *name = pa_proplist_gets(i->proplist, "media.name");
            if (name && (strcmp(name, "pulsesink probe") == 0)) {
                // Temporary stream => ignore
                break;
            }

            // Enable or not group sinks, so they use a high latency (lead) or
            // a low one (slave).
            // (Assumes one input, which is required to properly handle
            // timestamps)
            const char *client_str = pa_proplist_gets(i->proplist, "slave");
            for (auto sink : u->group_sinks_) {
                // TODO(jbing): support disabling some groups (e.g. multiroom)
                // while keeping others enable (e.g. multichannel). Requires
                // propert handling of latency (i.e. slaves must have
                // lower latency than lead) or fix for underrun.
                bool enable = ((client_str == nullptr)
                    /*|| (strcmp(client_str, sink->sink->name) != 0)*/);
                pa_log_info("%s %s group sink (client '%s')",
                    (enable ? "Enabling" : "Disabling"),
                    sink->sink->name, client_str);
                sink->enable(enable);
            }

            trace_open(&(u->ts_logging_));  // reopen in case tracing was disabled before
            trace_newstream(&(u->ts_logging_), u->sink_->name);

            break;
        }
        case PA_SINK_MESSAGE_REMOVE_INPUT: {
            // (Assumes one input, which is required to properly handle timestamps)
            pa_log_info("Disabling group sinks (remove input)");
            for (auto sink : u->group_sinks_) {
                sink->enable(false);
            }
            break;
        }

        case GROUP_MANAGER_SINK_SET_MASTER_ID:
            // TODO(hudas) We need to implement this later to support "multiple masters" in the group
            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state_cb(pa_sink *s, pa_sink_state_t state,
    PA_UNUSED pa_suspend_cause_t cause) {
    GroupManager *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));

    if (!PA_SINK_IS_LINKED(state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input_->state)) {
        return 0;
    }

    pa_sink_input_cork(u->sink_input_, state == PA_SINK_SUSPENDED);
    return 0;
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state,
    PA_UNUSED pa_suspend_cause_t new_suspend_cause) {
    GroupManager *u;

    pa_assert(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));

    /* When set to running or idle for the first time, request a rewind
     * of the master sink to make sure we are heard immediately */
    if (PA_SINK_IS_OPENED(new_state) && u->sink_->thread_info.state == PA_SINK_INIT) {
        pa_log_debug("Requesting rewind due to state change.");
        pa_sink_input_request_rewind(u->sink_input_, 0, false, true, true);
    }

    if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(u->sink_->thread_info.state)) {
        u->play();
    } else if (PA_SINK_IS_OPENED(u->sink_->thread_info.state) && !PA_SINK_IS_OPENED(new_state)) {
        u->stop();
    }

    // When resuming from non-RUNNING, since there has been a discontinuity,
    // restart the computation of the timestamps
    if ((new_state == PA_SINK_RUNNING) && (u->sink_->thread_info.state != PA_SINK_RUNNING)) {
        u->timestamp_ = PA_NSEC_INVALID;
    }

    return 0;
}

/* Called from I/O thread context */
static void sink_update_requested_latency(pa_sink *s) {
    GroupManager *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));

    if (!PA_SINK_IS_LINKED(u->sink_->thread_info.state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input_->thread_info.state)) {
        return;
    }

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
        u->sink_input_,
        pa_sink_get_requested_latency_within_thread(s));
}

/* Called from main context */
static void sink_set_mute_cb(pa_sink *s) {
    GroupManager *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));

    if (!PA_SINK_IS_LINKED(s->state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input_->state)) {
        return;
    }

    pa_sink_input_set_mute(u->sink_input_, s->muted, s->save_muted);
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t /*nbytes*/, pa_memchunk *chunk) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    if (!PA_SINK_IS_LINKED(u->sink_->thread_info.state)) {
        return -1;
    }

    /* Hmm, process any rewind request that might be queued up */
    pa_sink_process_rewind(u->sink_, 0);

    size_t block_size_max_sink = pa_frame_align(pa_mempool_block_size_max(i->core->mempool), &u->sink_->sample_spec);
    pa_sink_render(u->sink_, block_size_max_sink, chunk);
    if (!pa_memblock_is_silence(chunk->memblock)) {
        if (chunk->timestamp != PA_NSEC_INVALID) {
            u->timestamp_ = chunk->timestamp + chunk->duration;
        } else {
            pa_nsec_t now = ts_clock_now();
            pa_nsec_t target_latency = ((u->sink_->thread_info.min_latency + u->sink_->thread_info.max_latency) / 2) * PA_NSEC_PER_USEC;
            if (u->timestamp_ == PA_NSEC_INVALID) {
                // No timestamp yet => initialize it
                u->timestamp_ = now + target_latency;
            } else if ((u->timestamp_ - now) < (target_latency / 2)) {
                // We are getting the data too late, reset the timestamp
                u->timestamp_ = now + target_latency;
            }

            chunk->timestamp = u->timestamp_;
            chunk->duration = pa_bytes_to_nsec(chunk->length, &u->sink_->sample_spec);

            // The rounding error could accumulate in timestamp_ but as long as
            // everybody play the same data at the same timestamp that won't
            // affect the synchronization, and the time-to-play will smooth over
            // any gap/overlap (i.e. the user won't notice)
            u->timestamp_ += chunk->duration;
        }
        trace_ts(&(u->ts_logging_), u->sink_->name, chunk->timestamp, chunk->duration);
    }

    // TODO(jbing): recompute start time if "discontinuity flag" is set.

    return 0;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t /*nbytes*/) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    /* If the sink is not yet linked, there is nothing to rewind */
    if (!PA_SINK_IS_LINKED(u->sink_->thread_info.state))
        return;

    pa_sink_process_rewind(u->sink_, 0);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    /* (6) IF YOU NEED A FIXED BLOCK SIZE ROUND nbytes UP TO MULTIPLES
     * OF IT HERE. THE PA_ROUND_UP MACRO IS USEFUL FOR THAT. */

    pa_sink_set_max_request_within_thread(u->sink_, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    pa_sink_set_latency_range_within_thread(u->sink_, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void sink_input_update_sink_fixed_latency_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    /* (7) IF YOU NEED A FIXED BLOCK SIZE ADD THE LATENCY FOR ONE
     * BLOCK MINUS ONE SAMPLE HERE. pa_usec_to_bytes_round_up() IS
     * USEFUL FOR THAT. */

    pa_sink_set_fixed_latency_within_thread(u->sink_, i->sink->thread_info.fixed_latency);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    if (PA_SINK_IS_LINKED(u->sink_->thread_info.state))
        pa_sink_detach_within_thread(u->sink_);

    pa_sink_set_rtpoll(u->sink_, nullptr);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    pa_sink_set_rtpoll(u->sink_, i->sink->thread_info.rtpoll);
    pa_sink_set_latency_range_within_thread(u->sink_, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    /* (8.1) IF YOU NEED A FIXED BLOCK SIZE ADD THE LATENCY FOR ONE
     * BLOCK MINUS ONE SAMPLE HERE. SEE (7) */
    pa_sink_set_fixed_latency_within_thread(u->sink_, i->sink->thread_info.fixed_latency);

    /* (8.2) IF YOU NEED A FIXED BLOCK SIZE ROUND
     * pa_sink_input_get_max_request(i) UP TO MULTIPLES OF IT
     * HERE. SEE (6) */
    pa_sink_set_max_request_within_thread(u->sink_, pa_sink_input_get_max_request(i));

    if (PA_SINK_IS_LINKED(u->sink_->thread_info.state))
        pa_sink_attach_within_thread(u->sink_);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    /* The order here matters! We first kill the sink so that streams
     * can properly be moved away while the sink input is still connected
     * to the master. */
    pa_sink_input_cork(u->sink_input_, true);
    pa_sink_unlink(u->sink_);
    pa_sink_input_unlink(u->sink_input_);

    pa_sink_input_unref(u->sink_input_);
    u->sink_input_ = nullptr;

    pa_sink_unref(u->sink_);
    u->sink_ = nullptr;

    pa_module_unload_request(u->module_, true);
}

/* Called from main context */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    if (dest) {
        pa_sink_set_asyncmsgq(u->sink_, dest->asyncmsgq);
        pa_sink_update_flags(u->sink_,
            (pa_sink_flags_t)(PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY),
            dest->flags);
    } else {
        pa_sink_set_asyncmsgq(u->sink_, nullptr);
    }
}

GroupManager::~GroupManager() {
    /* See comments in sink_input_kill_cb() above regarding destruction order! */

    if (sink_input_) {
        pa_sink_input_cork(sink_input_, true);
    }

    if (sink_) {
        pa_sink_unlink(sink_);
    }

    if (sink_input_) {
        pa_sink_input_unlink(sink_input_);
        pa_sink_input_unref(sink_input_);
    }

    if (sink_) {
        pa_sink_unref(sink_);
    }

    trace_close(&ts_logging_);
}

std::unique_ptr<GroupManager> GroupManager::create(
    pa_module *m, pa_sink *master,
    std::set<GroupSinkCtrl *> groups,
    const pa_sample_spec &sample_spec, const pa_channel_map &channel_map) {
    pa_assert(m);
    pa_assert(master);

    auto u = std::unique_ptr<GroupManager>(new GroupManager);
    if (!u->init(m, master, std::move(groups),
            sample_spec, channel_map)) {
        return {};
    }
    return u;
}

bool GroupManager::init(pa_module *m, pa_sink *master,
    std::set<GroupSinkCtrl *> groups,
    const pa_sample_spec &sample_spec,
    const pa_channel_map &channel_map) {
    pa_sink_input_new_data sink_input_data;
    pa_sink_new_data sink_data;

    pa_assert(m);
    pa_assert(master);

    module_ = m;

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;

    sink_data.name = pa_xstrdup("group_manager");
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");

    {
        std::string group_names;
        for (auto group : groups) {
            group_names += group->sink->name;
            group_names += ',';
        }
        // Remove extra ','
        if (group_names.size() > 0) {
            group_names.resize(group_names.size() - 1);
        }
        pa_proplist_sets(sink_data.proplist, "group_manager.groups", group_names.c_str());
    }
    pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Group Manager");
    pa_sink_new_data_set_sample_spec(&sink_data, &sample_spec);
    pa_sink_new_data_set_channel_map(&sink_data, &channel_map);
    sink_ = pa_sink_new(m->core, &sink_data, (master->flags & (PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY)) | PA_SINK_SHARE_VOLUME_WITH_MASTER);
    pa_sink_new_data_done(&sink_data);

    if (!sink_) {
        pa_log("Failed to create sink.");
        return false;
    }

    sink_->parent.process_msg = sink_process_msg_cb;
    sink_->set_state_in_main_thread = sink_set_state_cb;
    sink_->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    sink_->update_requested_latency = sink_update_requested_latency;
    pa_sink_set_set_mute_callback(sink_, sink_set_mute_cb);
    sink_->userdata = this;

    pa_sink_set_asyncmsgq(sink_, master->asyncmsgq);
    pa_sink_set_max_rewind(sink_, 0);

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    pa_sink_input_new_data_set_sink(&sink_input_data, master, false, true);
    sink_input_data.origin_sink = sink_;
    pa_proplist_setf(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "Sink Input from %s", pa_proplist_gets(sink_->proplist, PA_PROP_DEVICE_DESCRIPTION));
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &sink_->sample_spec);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &sink_->channel_map);
    sink_input_data.flags |= PA_SINK_INPUT_START_CORKED;

    pa_sink_input_new(&sink_input_, m->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!sink_input_) {
        pa_log("Failed to create sink_input");
        return false;
    }

    sink_input_->pop = sink_input_pop_cb;
    sink_input_->process_rewind = sink_input_process_rewind_cb;
    sink_input_->update_max_request = sink_input_update_max_request_cb;
    sink_input_->update_sink_latency_range = sink_input_update_sink_latency_range_cb;
    sink_input_->update_sink_fixed_latency = sink_input_update_sink_fixed_latency_cb;
    sink_input_->kill = sink_input_kill_cb;
    sink_input_->attach = sink_input_attach_cb;
    sink_input_->detach = sink_input_detach_cb;
    sink_input_->moving = sink_input_moving_cb;
    sink_input_->volume_changed = nullptr;
    sink_input_->mute_changed = nullptr;
    sink_input_->userdata = this;

    sink_->input_to_master = sink_input_;

    group_sinks_ = std::move(groups);

    /* The order here is important. The input must be put first,
     * otherwise streams might attach to the sink before the sink
     * input is attached to the master. */
    pa_sink_input_put(sink_input_);
    pa_sink_put(sink_);
    pa_sink_input_cork(sink_input_, false);

    return true;
}

void GroupManager::play() {
    timestamp_ = PA_NSEC_INVALID;
}

void GroupManager::stop() {
}

void GroupManager::setMasterId(const std::string &master_id) {
    auto master_id_str = pa_xstrdup(master_id.c_str());
    pa_asyncmsgq_post(sink_->asyncmsgq, PA_MSGOBJECT(sink_), GROUP_MANAGER_SINK_SET_MASTER_ID,
        master_id_str, 0, nullptr, pa_xfree);
}
