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
#ifndef SRC_MODULES_GROUP_MODULES_MODULES_TS_RENDERER_CTRL_H_
#define SRC_MODULES_GROUP_MODULES_MODULES_TS_RENDERER_CTRL_H_

#include <ltdl.h>
#include <pulse/cdecl.h>
PA_C_DECL_BEGIN
#include <pulsecore/memblockq.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
PA_C_DECL_END

#include <memory>

#include "ts_renderer.h"

class TsRendererCtrl {
 public:
    ~TsRendererCtrl();

    static std::shared_ptr<TsRendererCtrl> create(pa_module *m, pa_sink *master, const char *library,
        const pa_sample_spec &sample_spec, const pa_channel_map &channel_map);

    pa_nsec_t getLatency();

 public:  // TODO(jbing): make private
    pa_module *module{nullptr};
    pa_sink *sink{nullptr};
    pa_sink_input *sink_input{nullptr};

    lt_dlhandle dl;
    TsRenderer *ts_renderer;

    // TODO(jbing): this is a work around. When we don't "pop" as many samples
    // as requested by the sink, sinks that use pa_sink_render_*full (e.g. alsa
    // and qahw) end up immediately calling "pop" again (until the memchunk is
    // full) but the reported latency doesn't account for those partial chunks.
    // So we need to do it ourselves.
    size_t returned_since_last_full_{0};
};

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_TS_RENDERER_CTRL_H_
