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

struct GroupSinkCallbacks {
    // Callbacks should avoid calling into GroupSink, there is no guarantee
    // that GroupSink is reentrant.
    void (*minimumLatencyUpdated)(void *user_data, pa_usec_t latency);
};
typedef struct GroupSinkCallbacks GroupSinkCallbacks;

struct GroupSink;
typedef struct GroupSink GroupSink;
struct GroupSink {
    // When disable, a device is acting as a slave and must not send the audio
    // to other group members.
    void (*enable)(GroupSink *gs, bool enable);

    // List of members in the group (IP addresses)
    void (*setMembers)(GroupSink *gs, const char *members[], size_t size);

    // List of network interfaces
    void (*updateInterfaces)(GroupSink *gs, const GroupSinkInterfaces interfaces[], size_t size);

    // Set the state of the playback. Can be used to connect to/disconnect from
    // the other group members
    void (*setState)(GroupSink *gs, enum GroupSinkPlayState state);

    // Change the stream format. Only called when the stream is stopped.
    // The function should override the content of the spec parameter if some
    // values are not acceptable and let PulseAudio reformat the next stream
    // as needed.
    void (*setSampleSpec)(GroupSink *gs, pa_sample_spec *spec);

    // Queue a chunk for sending. The function is the new chunk's owner, i.e.
    // it doesn't need to increment the chunk's ref counter but will need to
    // decrement it once done with the chunk.
    void (*send)(GroupSink *gs, pa_memchunk *chunk);

    // Set the network latency the group should use when computing the minimum
    // latency but it can be ignored/overruled if the group deem the latency
    // inappropriate (e.g. if it's too low)
    void (*setNetworkLatency)(GroupSink *gs, pa_usec_t latency);

    // Return the minimum latency needed by the group to ensure glitch-free
    // playback
    pa_usec_t (*getGroupMinimumLatency)(GroupSink *gs);

    // Set callback that the group will use to notify the PA module, or if it
    // needs to query something from PA
    void (*setCallbacks)(GroupSink *gs, const GroupSinkCallbacks *callbacks, void *user_data);
};

// Prototype for the initialization function.
// Returns a GroupSink struct filed with the appropriate function pointers.
//
// name is the name of the sink (for logging)
// spec is the stream format
// channel_map is the stream channel map
typedef GroupSink *(group_sink_init_proto)(const char *name,
    const pa_sample_spec *spec, const pa_channel_map *channel_map);

// Prototype for the release function.
typedef void(group_sink_done_proto)(GroupSink *gs);

// Initialization function.
group_sink_init_proto group_sink_init;

// Release function
group_sink_done_proto group_sink_done;

PA_C_DECL_END

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_GROUP_SINK_H_
