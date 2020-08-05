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
PA_C_DECL_BEGIN
#include <pulsecore/dbus-shared.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/ltdl-helper.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sink.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/thread.h>
PA_C_DECL_END

#include "enums.h"

constexpr char kIpcNamespace[] = "pulse.groupsink";

static constexpr pa_usec_t kMaxSilence = 1 * PA_USEC_PER_MSEC;  // 1ms

enum {
    GROUP_SINK_IO_MSG_ENABLE = PA_SINK_MESSAGE_MAX,
};
enum {
    GROUP_SINK_MAIN_MSG_SIGNAL_MINIMUM_LATENCY_UPDATE
};

template <>
struct is_flags<pa_sink_flags_t> : std::true_type {};

template <typename T, size_t S>
constexpr size_t arraySize(T (&)[S]) {
    return S;
}

struct GroupSinkMsg {
    pa_msgobject parent;
    GroupSinkCtrl *group_sink;

    // Message parameters
    union {
        // GROUP_SINK_MAIN_MSG_SIGNAL_MINIMUM_LATENCY_UPDATE
        struct {
            pa_usec_t latency;
        } signalMinimumLatencyUpdate;
    };
};
PA_DEFINE_PRIVATE_CLASS(GroupSinkMsg, pa_msgobject);

static void handleDbusGetMinimumLatency(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handleDbusSetAllocatedLatency(DBusConnection *conn, DBusMessage *msg, void *userdata);

static constexpr const char kGroupSinkDbusPathPrefix[] = "/org/pulseaudio/ext/latency/sink";
static constexpr const char kGroupSinkDbusIntf[] = "org.PulseAudio.Ext.Latency.Sink";

static pa_dbus_arg_info dbus_get_minimum_latency_args[] = {
    {"latency", DBUS_TYPE_UINT64_AS_STRING, "out"},
    {"name", DBUS_TYPE_STRING_AS_STRING, "out"}};

static pa_dbus_arg_info dbus_set_allocated_latency_args[] = {
    {"latency", DBUS_TYPE_UINT64_AS_STRING, "in"}};

static pa_dbus_arg_info dbus_minimum_latency_update_args[] = {
    {"latency", DBUS_TYPE_UINT64_AS_STRING, nullptr},
    {"name", DBUS_TYPE_STRING_AS_STRING, nullptr}};

static pa_dbus_method_handler method_handlers[] = {
    {"GetMinimumLatency",
        dbus_get_minimum_latency_args, arraySize(dbus_get_minimum_latency_args),
        handleDbusGetMinimumLatency},
    {"SetAllocatedLatency",
        dbus_set_allocated_latency_args, arraySize(dbus_set_allocated_latency_args),
        handleDbusSetAllocatedLatency}};

static constexpr const char kDbusMinimumLatencyUpdateSignal[] = "MinimumLatencyUpdate";
static pa_dbus_signal_info signals[] = {
    {kDbusMinimumLatencyUpdateSignal,
        dbus_minimum_latency_update_args, arraySize(dbus_minimum_latency_update_args)}};

static pa_dbus_interface_info interface_info = {
    kGroupSinkDbusIntf,
    method_handlers, arraySize(method_handlers),
    nullptr, 0,
    nullptr,
    signals, arraySize(signals)};

static std::weak_ptr<adk::msg::AdkMessageService> message_service_weak_ptr;

GroupSinkCtrl::~GroupSinkCtrl() {
    pa_sink_unlink(sink);

    if (dbus_protocol_) {
        pa_assert_se(pa_dbus_protocol_remove_interface(dbus_protocol_, dbus_path_.c_str(), interface_info.name) >= 0);
        pa_dbus_protocol_unref(dbus_protocol_);
        dbus_protocol_ = nullptr;
    }

    if (thread.joinable()) {
        pa_asyncmsgq_send(thread_mq.inq, nullptr, PA_MESSAGE_SHUTDOWN, nullptr, 0, nullptr);
        thread.join();
    }

    if (main_msg_) {
        GroupSinkMsg_unref(main_msg_);
        main_msg_ = nullptr;
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

/* Called from main thread */
void GroupSinkCtrl::updateLowLatencyMode(pa_sink_state_t state) {
    adk::msg::AdkMessage message;
    bool enable = (getPeersCount() > 0) && PA_SINK_IS_OPENED(state);
    pa_log_debug("Group Sync:: Low Latency %s", enable ? "Enable" : "Disable");

    if (enable) {
        message.mutable_system_low_latency_mode_enable()->set_source(sink->name);
    } else {
        message.mutable_system_low_latency_mode_disable()->set_source(sink->name);
    }
    message_service_->Send(message);
}

/* Called from main thread */
void GroupSinkCtrl::setPeers(std::vector<std::string> peers) {
    std::vector<const char *> members;
    for (auto &peer : peers) {
        members.push_back(peer.c_str());
    }
    setPeersCount(members.size());
    updateLowLatencyMode(sink->state);
    group_sink->setMembers(group_sink, members.data(), members.size());
}

void GroupSinkCtrl::updateInterfaces(const std::vector<GroupSinkInterfaces> &interfaces) {
    group_sink->updateInterfaces(group_sink, interfaces.data(), interfaces.size());
}

void GroupSinkCtrl::enable(bool enable) {
    pa_asyncmsgq_post(thread_mq.inq, PA_MSGOBJECT(sink), GROUP_SINK_IO_MSG_ENABLE, reinterpret_cast<void *>(enable), 0, nullptr, nullptr);
}

/* Called from main context */
static int sink_set_state_in_main_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t suspend_cause PA_UNUSED) {
    auto u = reinterpret_cast<GroupSinkCtrl *>(s->userdata);

    if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(s->state)) {
        u->updateLowLatencyMode(new_state);
    } else if (!PA_SINK_IS_OPENED(new_state) && PA_SINK_IS_OPENED(s->state)) {
        u->updateLowLatencyMode(new_state);
    }

    return 0;
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

/* Called from main context */
static int sink_process_main_msg(pa_msgobject *o, int code, void * /*userdata*/, int64_t /*offset*/, pa_memchunk * /*chunk*/) {
    pa_assert(o);
    pa_assert_ctl_context();

    struct GroupSinkMsg *msg = GroupSinkMsg_cast(o);
    switch (code) {
        case GROUP_SINK_MAIN_MSG_SIGNAL_MINIMUM_LATENCY_UPDATE:
            msg->group_sink->signalMinimumLatencyUpdate(msg->signalMinimumLatencyUpdate.latency);
            return 0;
    }

    return -1;
}

/* Called from I/O thread context */
static int sink_process_io_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
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

        case GROUP_SINK_IO_MSG_ENABLE: {
            pa_usec_t old_latency = u->group_sink->getTargetLatency(u->group_sink);

            bool enable = (data != nullptr);
            u->group_sink->enable(u->group_sink, enable);

            pa_usec_t new_latency = u->group_sink->getTargetLatency(u->group_sink);
            if (old_latency != new_latency) {
                pa_sink_set_latency_range_within_thread(u->sink, new_latency, new_latency);

                u->main_msg_->signalMinimumLatencyUpdate.latency = new_latency;
                pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->main_msg_), GROUP_SINK_MAIN_MSG_SIGNAL_MINIMUM_LATENCY_UPDATE, NULL, 0, NULL, NULL);
            }
            return 0;
        }
    }
    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* I/O thread */
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
        pa_usec_t target_latency = u->target_latency_;
        if (target_latency == PA_USEC_INVALID) {
            target_latency = u->group_sink->getTargetLatency(u->group_sink);
        }

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

/* Called from Main thread context */
static void handleDbusGetMinimumLatency(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    auto d = reinterpret_cast<GroupSinkCtrl *>(userdata);

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(d);

    // TODO(jbing): should we consider the helper library thread safe? Or should
    // all access be limited to the I/O thread (in which case we need to fix
    // the following)?
    auto latency = static_cast<dbus_uint64_t>(d->group_sink->getTargetLatency(d->group_sink));

    DBusMessage *reply;
    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    pa_assert_se(dbus_message_append_args(reply, DBUS_TYPE_UINT64, &latency, DBUS_TYPE_STRING, &d->sink->name, DBUS_TYPE_INVALID));

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

/* Called from Main thread context */
static void handleDbusSetAllocatedLatency(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    auto d = reinterpret_cast<GroupSinkCtrl *>(userdata);

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(d);

    DBusError error;
    dbus_error_init(&error);

    dbus_uint64_t latency;
    if ((dbus_message_get_args(msg, &error, DBUS_TYPE_UINT64, &latency, DBUS_TYPE_INVALID)) == 0) {
        pa_log("SetAllocatedLatency invalid args: %s", error.message);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    pa_log_info("Allocated latency for %susec: %" PRIu64, d->sink->name, latency);
    d->target_latency_ = latency;

    pa_dbus_send_empty_reply(conn, msg);
}

/* Called from Main thread context */
void GroupSinkCtrl::signalMinimumLatencyUpdate(pa_usec_t _latency) {
    pa_assert_ctl_context();

    auto latency = static_cast<dbus_uint64_t>(_latency);
    pa_log_info("Signaling minimum latency %" PRIu64 "usec for %s", latency, sink->name);

    DBusMessage *signal_msg;
    pa_assert_se(signal_msg = dbus_message_new_signal(dbus_path_.c_str(),
                     kGroupSinkDbusIntf,
                     kDbusMinimumLatencyUpdateSignal));
    pa_assert_se(dbus_message_append_args(signal_msg,
        DBUS_TYPE_UINT64, &latency,
        DBUS_TYPE_STRING, &sink->name,
        DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(dbus_protocol_, signal_msg);
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

    // Handlers for messages in io thread
    u->sink->parent.process_msg = sink_process_io_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;

    // Handlers for messages in main thread
    u->main_msg_ = pa_msgobject_new(GroupSinkMsg);
    u->main_msg_->parent.process_msg = sink_process_main_msg;
    u->main_msg_->group_sink = u.get();
    u->sink->set_state_in_main_thread = sink_set_state_in_main_thread_cb;

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

    // We expect the module to be loaded in turn, so no need for locks
    // TODO(jbing): currently there can only be one single instance adk-message
    // because the d-bus connection is shared and the object path is fixed. But
    // ideally, each sink should have its own AdkMessageService
    u->message_service_ = message_service_weak_ptr.lock();
    if (u->message_service_) {
        pa_log_debug("Group Sink Create : Message Service already initialized");
    } else {
        u->message_service_ = std::make_shared<adk::msg::AdkMessageService>(kIpcNamespace);
        if (!u->message_service_->Initialise()) {
            pa_log_debug("Group Sink Create Failed to initialise message service");
            goto fail;
        }
        message_service_weak_ptr = u->message_service_;
        pa_log_debug("Group Sink Create success to initialise message service");
    }

    u->dbus_protocol_ = pa_dbus_protocol_get(u->module->core);
    u->dbus_path_ = kGroupSinkDbusPathPrefix + std::to_string(u->sink->index);
    pa_assert_se(pa_dbus_protocol_add_interface(u->dbus_protocol_, u->dbus_path_.c_str(), &interface_info, u.get()) >= 0);

    pa_sink_put(u->sink);

    pa_log_warn("group sink created");

    return u;

fail:
    return {};
}
