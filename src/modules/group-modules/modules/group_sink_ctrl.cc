/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering
  Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.

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

#include "group_sink_ctrl.h"

#include <pthread.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
PA_C_DECL_BEGIN
#include <pulsecore/ltdl-helper.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sink.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/thread.h>
PA_C_DECL_END

#include "enums.h"

static constexpr pa_usec_t kMaxSilence = 1 * PA_USEC_PER_MSEC;  // 1ms

enum {
    GROUP_SINK_ENABLE = PA_SINK_MESSAGE_MAX,
};

template <>
struct is_flags<pa_sink_flags_t> : std::true_type {};

GroupSinkCtrl::~GroupSinkCtrl() {
    pa_sink_unlink(sink);

    if (thread.joinable()) {
        pa_asyncmsgq_send(thread_mq.inq, nullptr, PA_MESSAGE_SHUTDOWN, nullptr, 0, nullptr);
        thread.join();
    }

    pa_thread_mq_done(&thread_mq);

    if (dl != nullptr) {
        if (group_sink != nullptr) {
            group_sink_done_proto *done = reinterpret_cast<group_sink_done_proto *>(pa_load_sym(dl, nullptr, "group_sink_done"));
            if (done == nullptr) {
                pa_log("Failed to find 'group_sink_done' symbol");
            } else {
                done(group_sink);
            }
        }
        lt_dlclose(dl);
    }

    pa_sink_unref(sink);

    if (rtpoll != nullptr) {
        pa_rtpoll_free(rtpoll);
    }
}

void GroupSinkCtrl::setPeers(std::vector<std::string> peers) {
    std::vector<const char *> members;
    for (auto &peer : peers) {
        members.push_back(peer.c_str());
    }
    group_sink->setMembers(group_sink, members.data(), members.size());
}

void GroupSinkCtrl::updateInterfaces(const std::vector<GroupSinkInterfaces> &interfaces) {
    group_sink->updateInterfaces(group_sink, interfaces.data(), interfaces.size());
}

void GroupSinkCtrl::enable(bool enable) {
    pa_asyncmsgq_post(thread_mq.inq, PA_MSGOBJECT(sink), GROUP_SINK_ENABLE, reinterpret_cast<void *>(enable), 0, nullptr, nullptr);
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause PA_UNUSED) {
    auto u = reinterpret_cast<GroupSinkCtrl *>(s->userdata);
    if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(s->thread_info.state)) {
        u->group_sink->setState(u->group_sink, GROUP_SINK_PLAYING);
    } else if (!PA_SINK_IS_OPENED(new_state)) {
        u->group_sink->setState(u->group_sink, GROUP_SINK_STOPPED);
    }

    return 0;
}

/* Called from I/O thread context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    auto u = reinterpret_cast<GroupSinkCtrl *>(PA_SINK(o)->userdata);
    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:
            /* The sink is _put() before the sink input is, so let's
             * make sure we don't access it yet */
            if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)) {
                *reinterpret_cast<int64_t *>(data) = 0;
                return 0;
            }

            *reinterpret_cast<int64_t *>(data) = static_cast<int64_t>(u->group_sink->getCurrentLatency(u->group_sink));
            return 0;

        case GROUP_SINK_ENABLE: {
            bool enable = (data != nullptr);
            u->group_sink->enable(u->group_sink, enable);

            pa_usec_t latency = u->group_sink->getTargetLatency(u->group_sink);
            pa_sink_set_latency_range_within_thread(u->sink, latency, latency);
            return 0;
        }
    }
    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void thread_func(GroupSinkCtrl *u) {
    if (u->sink->core->realtime_scheduling) {
        pa_make_realtime(u->sink->core->realtime_priority);
    }

    pa_thread_mq_install(&u->thread_mq);

    pa_rtpoll_set_timer_disabled(u->rtpoll);

    for (;;) {
        // There is no event for new audio packet so don't block if we expect one
        int ret = pa_rtpoll_run(u->rtpoll, true);
        if (ret < 0) {
            goto fail;
        } else if (ret == 0) {
            goto finish;
        }

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested)) {
            pa_sink_process_rewind(u->sink, 0);
        }

        if (PA_SINK_IS_RUNNING(u->sink->thread_info.state) == 0) {
            pa_rtpoll_set_timer_disabled(u->rtpoll);
            continue;
        }

        // Expire chunks from queue
        pa_usec_t queue_latency = u->group_sink->getCurrentLatency(u->group_sink);
        pa_usec_t target_latency = u->group_sink->getTargetLatency(u->group_sink);

        if (queue_latency >= target_latency) {
            // Queue latency is maxed out, sleep a little bit
            pa_rtpoll_set_timer_absolute(u->rtpoll,
                pa_rtclock_now() + queue_latency - target_latency);
            continue;
        }

        pa_memchunk chunk;
        if (!pa_sink_render_one(u->sink, &chunk)) {
            pa_rtpoll_set_timer_relative(u->rtpoll, kMaxSilence);
            continue;
        }

        u->group_sink->send(u->group_sink, &chunk);

        // Run again immediately (to fill the queue until it's full)
        pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->module->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, nullptr, nullptr);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

std::shared_ptr<GroupSinkCtrl> GroupSinkCtrl::create(pa_module *_module,
    const char *name, const char *library,
    pa_usec_t lead_latency, pa_usec_t slave_latency,
    const pa_sample_spec &sample_spec, const pa_channel_map &channel_map) {
    pa_sink_new_data data;

    auto u = std::shared_ptr<GroupSinkCtrl>(new GroupSinkCtrl);
    u->module = _module;
    u->rtpoll = pa_rtpoll_new();

    u->dl = lt_dlopenext(library);
    if (u->dl == nullptr) {
        pa_log("Failed to open support library for '%s': %s", name, lt_dlerror());
        goto fail;
    }

    {
        group_sink_init_proto *init = reinterpret_cast<group_sink_init_proto *>(pa_load_sym(u->dl, nullptr, "group_sink_init"));
        if (init == nullptr) {
            pa_log("Failed to find 'group_sink_init' symbol");
            goto fail;
        }
        u->group_sink = (*init)(name, &sample_spec, &channel_map, lead_latency, slave_latency);
    }

    if (pa_thread_mq_init(&u->thread_mq, u->module->core->mainloop, u->rtpoll) < 0) {
        pa_log("Failed to init message queue");
        goto fail;
    }

    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;
    pa_sink_new_data_set_name(&data, name);

    pa_sink_new_data_set_sample_spec(&data, &sample_spec);
    pa_sink_new_data_set_channel_map(&data, &channel_map);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("Group sink"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");

    u->sink = pa_sink_new(u->module->core, &data, PA_SINK_LATENCY | PA_SINK_NETWORK | PA_SINK_DYNAMIC_LATENCY);
    u->sink->userdata = u.get();

    pa_sink_new_data_done(&data);

    if (u->sink == nullptr) {
        pa_log("Failed to create sink");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;

    // sink->update_requested_latency = sink_update_requested_latency_cb;
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    pa_sink_set_latency_range(u->sink, u->group_sink->getTargetLatency(u->group_sink), u->group_sink->getTargetLatency(u->group_sink));

    // size_t block_bytes = pa_usec_to_bytes(d->block_usec, &sink->sample_spec);
    // pa_sink_set_max_rewind(sink, block_bytes);
    // pa_sink_set_max_request(sink, block_bytes);

    {
        auto thread_u = u.get();
        u->thread = std::thread([thread_u]() { thread_func(thread_u); });
        pthread_setname_np(u->thread.native_handle(), u->sink->name);
    }

    pa_sink_put(u->sink);

    pa_log_warn("group sink created");

    return u;

fail:
    return {};
}
