/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
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
#ifndef SRC_MODULES_GROUP_MODULES_MODULES_GROUP_SINK_CTRL_H_
#define SRC_MODULES_GROUP_MODULES_MODULES_GROUP_SINK_CTRL_H_

#include <ltdl.h>
#include <pulse/cdecl.h>
#include <pulse/timeval.h>
PA_C_DECL_BEGIN
#include <pulsecore/module.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/sink.h>
PA_C_DECL_END

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <adk/message-service/adk-message-service.h>

#include "group_sink.h"

struct GroupSinkMsg;

class GroupSinkCtrl {
 public:
    GroupSinkCtrl() = default;
    ~GroupSinkCtrl();

    static std::shared_ptr<GroupSinkCtrl> create(pa_module *_module, const char *name, const char *library,
        const pa_sample_spec &sample_spec, const pa_channel_map &channel_map, bool avoid_processing);

    void setPeers(std::vector<std::string> peers);
    void updateInterfaces(const std::vector<GroupSinkInterfaces> &interfaces);
    void enable(bool enable);

 public:  // TODO(jbing): should all be private
    void updateLowLatencyMode(pa_sink_state_t state);

    void setPeersCount(size_t peers_count) { peers_count_ = peers_count; }
    size_t getPeersCount() const { return peers_count_; }

    void updateMinimumLatency(pa_usec_t latency);
    void signalMinimumLatencyUpdate(pa_usec_t _latency);

 public:  // TODO(jbing): should all be private
    std::shared_ptr<adk::msg::AdkMessageService> message_service_;

    pa_module *module{nullptr};
    pa_sink *sink{nullptr};
    std::thread thread;
    pa_thread_mq thread_mq{};
    pa_rtpoll *rtpoll{nullptr};

    GroupSinkMsg *main_msg_;

    lt_dlhandle dl{nullptr};
    GroupSink *group_sink{nullptr};

    pa_nsec_t next_timestamp_{PA_NSEC_INVALID};

    size_t peers_count_{0};

    std::string dbus_path_;
    pa_dbus_protocol *dbus_protocol_{nullptr};

    std::atomic<pa_usec_t> target_latency_{PA_USEC_INVALID};
};

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_GROUP_SINK_CTRL_H_
