/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
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

#include "ts_renderer_impl.h"

static void ts_renderer_setState(TsRenderer *renderer PA_GCC_UNUSED, enum TsRendererPlayState state PA_GCC_UNUSED) {
    // Null sink, nothing to do
}

static int ts_renderer_render(TsRenderer *renderer PA_GCC_UNUSED, pa_memchunk *in, pa_memchunk *out, size_t nbytes PA_GCC_UNUSED, pa_nsec_t playback_time PA_GCC_UNUSED) {
    if (in == NULL) {
        // No new, chunk, wait for one
        pa_memchunk_reset(out);
        return 0;
    }

    // Null sink, no need to delay/speed up playback, just pass the chunk as-is
    *out = *in;
    return 0;
}

TsRenderer *ts_renderer_init(const char *name PA_GCC_UNUSED, const pa_sample_spec *spec PA_GCC_UNUSED, pa_memchunk *silence PA_GCC_UNUSED) {
    TsRenderer *renderer = pa_xmalloc0(sizeof(TsRenderer));
    renderer->setState = &ts_renderer_setState;
    renderer->render = &ts_renderer_render;
    return renderer;
}

void ts_renderer_done(TsRenderer *renderer) {
    pa_xfree(renderer);
}
