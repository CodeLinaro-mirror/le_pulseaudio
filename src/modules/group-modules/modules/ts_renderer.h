/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
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
#ifndef SRC_MODULES_GROUP_MODULES_MODULES_TS_RENDERER_H_
#define SRC_MODULES_GROUP_MODULES_MODULES_TS_RENDERER_H_

#include <pulse/sample.h>
PA_C_DECL_BEGIN
#include <pulsecore/memchunk.h>

enum TsRendererPlayState {
    TS_RENDERER_STOPPED,
    TS_RENDERER_PLAYING
};

struct TsRenderer;
typedef struct TsRenderer TsRenderer;
struct TsRenderer {
    // Set the state of the playback. Can be used to reset the sink to neutral
    // (e.g. flush internal buffers)
    void (*setState)(TsRenderer *renderer, enum TsRendererPlayState state);

    // Process a chunk for playback. This function should take appropriate steps
    // to ensure the audio will play at the right time.
    // At each iteration, it is first all will a NULL "in" chunk, allowing the
    // renderer to return any leftover samples. If the "out" chunk is left empty,
    // the function is called again with a new chunk to process.
    //
    // in is first NULL to allow returning any remaining data from the previous
    // chunk, otherwise the next chunk to process
    // out is the data to push to the local output sink for playback
    // nbytes is the preferred amount of data to return in the "out" chunk
    // playback_time estimated playback time of the first sample that will be
    // returned in the "out" chunk. In ideal circumstances, it should be equal
    // to the "in" chunk's timestamp. In practice, this should be used to slow
    // down and speed up the playback.
    int (*render)(TsRenderer *renderer, pa_memchunk *in, pa_memchunk *out, size_t nbytes, pa_nsec_t playback_time);
};

// Prototype for the initialization function.
// Returns a TsRenderer struct filed with the appropriate function pointers.
//
// name is the name of the sink (for logging)
// spec if the stream format
// silence is a chunk that can be used by render() to play before a chunk that
// would otherwise play too early.
typedef TsRenderer *(ts_renderer_init_proto)(const char *name, const pa_sample_spec *spec, pa_memchunk *silence);

// Prototype for the release function.
typedef void(ts_renderer_done_proto)(TsRenderer *gs);

// Initialization function.
ts_renderer_init_proto ts_renderer_init;

// Release function
ts_renderer_done_proto ts_renderer_done;

PA_C_DECL_END

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_TS_RENDERER_H_
