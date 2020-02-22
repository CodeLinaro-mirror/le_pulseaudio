/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
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
#include "pulsecore/config.h"

#include "group_sink_impl.h"
#include "pulse/timeval.h"
#include "pulsecore/ts_clock.h"

typedef struct GroupSinkImpl {
    GroupSink group_sink;

    pa_usec_t lead_latency;
    pa_usec_t slave_latency;

    pa_usec_t target_latency;

    pa_nsec_t last_sample_ts;
} GroupSinkImpl;

static void group_sink_enable(GroupSink *gs, bool enable) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    gs_impl->target_latency = enable ? gs_impl->lead_latency : gs_impl->slave_latency;
}

static void group_sink_setMembers(GroupSink *gs PA_GCC_UNUSED, const char *members[] PA_GCC_UNUSED, size_t size PA_GCC_UNUSED) {
    // null sink, we don't care about the group members
}

static void group_sink_updateInterfaces(GroupSink *gs PA_GCC_UNUSED, const GroupSinkInterfaces interfaces[] PA_GCC_UNUSED, size_t size PA_GCC_UNUSED) {
    // null sink, we don't care about the group sink interfaces
}

static void group_sink_setState(GroupSink *gs, enum GroupSinkPlayState state PA_GCC_UNUSED) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    gs_impl->last_sample_ts = ts_clock_now();
}

static void group_sink_send(GroupSink *gs, pa_memchunk *chunk) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    gs_impl->last_sample_ts = chunk->timestamp + chunk->duration;

    pa_memblock_unref(chunk->memblock);
}

static pa_usec_t group_sink_getTargetLatency(GroupSink *gs) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    return gs_impl->target_latency;
}

static pa_usec_t group_sink_getCurrentLatency(GroupSink *gs) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    pa_nsec_t latency = gs_impl->last_sample_ts - ts_clock_now();
    if (((int64_t)latency) < 0) {
        return 0;
    }
    return latency / PA_NSEC_PER_USEC;
}

GroupSink *group_sink_init(const char *name PA_GCC_UNUSED,
    const pa_sample_spec *spec PA_GCC_UNUSED, const pa_channel_map *channel_map PA_GCC_UNUSED,
    pa_usec_t lead_latency, pa_usec_t slave_latency) {
    GroupSinkImpl *gs_impl = pa_xmalloc0(sizeof(GroupSinkImpl));

    gs_impl->group_sink.enable = &group_sink_enable;
    gs_impl->group_sink.setMembers = &group_sink_setMembers;

    gs_impl->group_sink.setState = &group_sink_setState;
    gs_impl->group_sink.send = &group_sink_send;

    gs_impl->group_sink.getTargetLatency = &group_sink_getTargetLatency;
    gs_impl->group_sink.getCurrentLatency = &group_sink_getCurrentLatency;

    gs_impl->lead_latency = lead_latency;
    gs_impl->slave_latency = slave_latency;
    gs_impl->target_latency = slave_latency;

    return (GroupSink *)gs_impl;
}
void group_sink_done(GroupSink *gs) {
    pa_xfree(gs);
}
