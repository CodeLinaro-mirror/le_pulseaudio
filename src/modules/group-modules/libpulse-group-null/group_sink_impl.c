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

#define SLAVE_LATENCY (25 * PA_USEC_PER_MSEC)

typedef struct GroupSinkImpl {
    GroupSink group_sink;

    bool enabled;
    pa_usec_t network_latency;

    GroupSinkCallbacks callbacks;
    void *callback_data;
} GroupSinkImpl;

pa_usec_t group_sink_getCurrentLatency(GroupSink *gs);

static void group_sink_enable(GroupSink *gs, bool enable) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    gs_impl->enabled = enable;
    // real sinks might want to (dis)connect to/from slaves

    if (gs_impl->callbacks.minimumLatencyUpdated) {
        gs_impl->callbacks.minimumLatencyUpdated(gs_impl->callback_data, group_sink_getCurrentLatency(gs));
    }
}

static void group_sink_setMembers(GroupSink *gs PA_GCC_UNUSED, const char *members[] PA_GCC_UNUSED, size_t size PA_GCC_UNUSED) {
    // null sink, we don't care about the group members
    // real sinks might want to connect to the added slaves/disconnect from
    // removed slaves and update the minimum latency
}

static void group_sink_updateInterfaces(GroupSink *gs PA_GCC_UNUSED, const GroupSinkInterfaces interfaces[] PA_GCC_UNUSED, size_t size PA_GCC_UNUSED) {
    // null sink, we don't care about the group sink interfaces
    // real sinks might want to reconnect to the slaves
}

static void group_sink_setState(GroupSink *gs PA_GCC_UNUSED, enum GroupSinkPlayState state PA_GCC_UNUSED) {
    // null sink, we don't care about the state
    // real sink should notify the slaves
}

static void group_sink_setSampleSpec(GroupSink *gs PA_GCC_UNUSED, pa_sample_spec *spec PA_GCC_UNUSED) {
    // null sink, we don't care about the new format
    // real sink should notify the slaves
}

static void group_sink_send(GroupSink *gs PA_GCC_UNUSED, pa_memchunk *chunk) {
    // real sinks should send data to slaves (must be non-blocking)
    pa_memblock_unref(chunk->memblock);
}

static void group_sink_setNetworkLatency(GroupSink *gs, pa_usec_t latency) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    gs_impl->network_latency = latency;
    if (gs_impl->callbacks.minimumLatencyUpdated) {
        gs_impl->callbacks.minimumLatencyUpdated(gs_impl->callback_data, group_sink_getCurrentLatency(gs));
    }
}

static pa_usec_t group_sink_getGroupMinimumLatency(GroupSink *gs) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    return (gs_impl->enabled
            ? (gs_impl->network_latency + SLAVE_LATENCY)
            : 0);
}

static void group_sink_setCallbacks(GroupSink *gs, const GroupSinkCallbacks *callbacks, void *user_data) {
    GroupSinkImpl *gs_impl = (GroupSinkImpl *)gs;
    gs_impl->callbacks = *callbacks;
    gs_impl->callback_data = user_data;
}

GroupSink *group_sink_init(const char *name PA_GCC_UNUSED,
    const pa_sample_spec *spec PA_GCC_UNUSED, const pa_channel_map *channel_map PA_GCC_UNUSED) {
    GroupSinkImpl *gs_impl = pa_xmalloc0(sizeof(GroupSinkImpl));

    gs_impl->group_sink.enable = &group_sink_enable;
    gs_impl->group_sink.setMembers = &group_sink_setMembers;
    gs_impl->group_sink.updateInterfaces = &group_sink_updateInterfaces;

    gs_impl->group_sink.setState = &group_sink_setState;
    gs_impl->group_sink.setSampleSpec = &group_sink_setSampleSpec;
    gs_impl->group_sink.send = &group_sink_send;

    gs_impl->group_sink.setNetworkLatency = &group_sink_setNetworkLatency;
    gs_impl->group_sink.getGroupMinimumLatency = &group_sink_getGroupMinimumLatency;

    gs_impl->group_sink.setCallbacks = &group_sink_setCallbacks;

    return (GroupSink *)gs_impl;
}
void group_sink_done(GroupSink *gs) {
    pa_xfree(gs);
}
