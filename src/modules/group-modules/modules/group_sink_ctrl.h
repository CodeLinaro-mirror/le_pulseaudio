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
PA_C_DECL_BEGIN
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
PA_C_DECL_END

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <adk/message-service/adk-message-service.h>

#include "group_sink.h"

class GroupSinkCtrl {
 public:
    GroupSinkCtrl() = default;
    ~GroupSinkCtrl();

    static std::shared_ptr<GroupSinkCtrl> create(pa_module *_module, const char *name, const char *library,
        pa_usec_t lead_latency, pa_usec_t slave_latency,
        const pa_sample_spec &sample_spec, const pa_channel_map &channel_map);

    void setPeers(std::vector<std::string> peers);
    void updateInterfaces(const std::vector<GroupSinkInterfaces> &interfaces);
    void enable(bool enable);

 public:  // TODO(jbing): should all be private
    void updateLowLatencyMode(pa_sink_state_t state);

    void setPeersCount(size_t peers_count) { peers_count_ = peers_count; }
    size_t getPeersCount() const { return peers_count_; }

 public:  // TODO(jbing): should all be private
    std::shared_ptr<adk::msg::AdkMessageService> message_service_;

    pa_module *module{nullptr};
    pa_sink *sink{nullptr};
    std::thread thread;
    pa_thread_mq thread_mq{};
    pa_rtpoll *rtpoll{nullptr};

    lt_dlhandle dl{nullptr};
    GroupSink *group_sink{nullptr};

    size_t peers_count_{0};
};

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_GROUP_SINK_CTRL_H_
