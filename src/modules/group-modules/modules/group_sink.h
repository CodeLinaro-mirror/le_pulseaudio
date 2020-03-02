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
#ifndef SRC_MODULES_GROUP_MODULES_MODULES_GROUP_SINK_H_
#define SRC_MODULES_GROUP_MODULES_MODULES_GROUP_SINK_H_

#include <pulse/channelmap.h>
#include <pulse/sample.h>
PA_C_DECL_BEGIN
#include <pulsecore/memchunk.h>

enum GroupSinkPlayState {
    GROUP_SINK_STOPPED,
    GROUP_SINK_PLAYING
};

struct GroupSinkInterfaces {
    const char *iface_name;
    const char *physical_iface_name;
};
typedef struct GroupSinkInterfaces GroupSinkInterfaces;

struct GroupSink;
typedef struct GroupSink GroupSink;
struct GroupSink {
    // When disable, a device is acting as a slave and must not send the audio
    // to other group members. When enabled it should change the target latency
    // should be to "lead_latency", and otherwise to "slave_latency" (see
    // group_sink_init_proto function)
    void (*enable)(GroupSink *gs, bool enable);

    // List of members in the group (IP addresses)
    void (*setMembers)(GroupSink *gs, const char *members[], size_t size);

    // List of network interfaces
    void (*updateInterfaces)(GroupSink *gs, const GroupSinkInterfaces interfaces[], size_t size);

    // Set the state of the playback. Can be used to connect to/disconnect from
    // the other group members
    void (*setState)(GroupSink *gs, enum GroupSinkPlayState state);

    // Queue a chunk for sending. The function is the new chunk's owner, i.e.
    // it doesn't need to increment the chunk's ref counter but will need to
    // decrement it once done with the chunk.
    void (*send)(GroupSink *gs, pa_memchunk *chunk);

    // Current target latency. When enabled the target latency should be
    // "lead_latency", and when not, "slave_latency" (see init function)
    pa_usec_t (*getTargetLatency)(GroupSink *gs);

    // Current latency. When that latency drops below the target latency, send()
    // will be called with a new chunk.
    pa_usec_t (*getCurrentLatency)(GroupSink *gs);
};

// Prototype for the initialization function.
// Returns a GroupSink struct filed with the appropriate function pointers.
//
// name is the name of the sink (for logging)
// spec is the stream format
// channel_map is the stream channel map
// lead_latency is the latency to use when the current device is the group lead
// and need to send the audio to the other group members (slaves)
// slave_latency is the latency to use when the current device is a slave.
// Currently this is mostly used to read chunks slightly ahead of the playback
// allowing them the be buffered nearer to the local output sink instead of
// pulseaudio core
typedef GroupSink *(group_sink_init_proto)(const char *name,
    const pa_sample_spec *spec, const pa_channel_map *channel_map,
    pa_usec_t lead_latency, pa_usec_t slave_latency);

// Prototype for the release function.
typedef void(group_sink_done_proto)(GroupSink *gs);

// Initialization function.
group_sink_init_proto group_sink_init;

// Release function
group_sink_done_proto group_sink_done;

PA_C_DECL_END

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_GROUP_SINK_H_
