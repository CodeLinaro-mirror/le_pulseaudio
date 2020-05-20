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
#ifndef SRC_MODULES_GROUP_MODULES_MODULES_GROUP_MANAGER_H_
#define SRC_MODULES_GROUP_MODULES_MODULES_GROUP_MANAGER_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#include <pulse/cdecl.h>
#include <pulse/timeval.h>
PA_C_DECL_BEGIN
#include <pulsecore/memblockq.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/trace_log.h>
PA_C_DECL_END

class GroupSinkCtrl;

class GroupManager {
 private:
    GroupManager() = default;

 public:
    ~GroupManager();

    static std::unique_ptr<GroupManager> create(pa_module *m, pa_sink *master,
        std::set<GroupSinkCtrl *> groups,
        const pa_sample_spec &sample_spec, const pa_channel_map &channel_map);

    void play();
    //    void pause();
    void stop();

    void setMasterId(const std::string &master_id);

    void resetTimestamp(pa_sink_input *new_input = nullptr);

 private:
    bool init(pa_module *m, pa_sink *master,
        std::set<GroupSinkCtrl *> group,
        const pa_sample_spec &sample_spec, const pa_channel_map &channel_map);

 public:  // TODO(jbing): make it private
    pa_module *module_{nullptr};
    pa_sink *sink_{nullptr};
    pa_sink_input *sink_input_{nullptr};

    std::mutex lock_;
    std::condition_variable cond_;

    std::set<GroupSinkCtrl *> group_sinks_;

    pa_sink_input *active_input_{nullptr};
    bool has_timestamps_{false};
    bool in_underrun_{false};
    pa_nsec_t timestamp_{PA_NSEC_INVALID};

    size_t min_chunk_length_{0};
    size_t max_chunk_length_{0};
    pa_memchunk remaining_chunk_;

    trace_log ts_logging_ = TRACE_LOG_STATIC_INIT;
    // name for tracing the first query for a packet
    std::string ts_query_name_;
    // time when we first ask for a packet (need to save it because we might
    // not know yet what the packet timestamp is)
    pa_nsec_t ts_query_ltime_{PA_NSEC_INVALID};
    pa_nsec_t ts_query_expected_ts{PA_NSEC_INVALID};
};

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_GROUP_MANAGER_H_
