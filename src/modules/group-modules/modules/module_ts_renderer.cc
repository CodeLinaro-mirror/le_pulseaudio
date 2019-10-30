/***
   This file is part of PulseAudio.
 
   Copyright 2010 Intel Corporation
   Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>
   Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 
   PulseAudio is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published
   by the Free Software Foundation; either version 2.1 of the License,
   or (at your option) any later version.
 
   PulseAudio is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.
 
   You should have received a copy of the GNU Lesser General Public License
   along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/
#include "pulsecore_config.h"

#include <pulse/gccmacro.h>
#include <pulse/xmalloc.h>
PA_C_DECL_BEGIN
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/modargs.h>
#include <pulsecore/module.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
PA_C_DECL_END

#include "ts_renderer_ctrl.h"

#define MOD_EXPORT __attribute__((visibility("default")))

MOD_EXPORT int pa__init(pa_module *m);
MOD_EXPORT void pa__done(pa_module *m);
MOD_EXPORT int pa__get_n_used(pa_module *m);

MOD_EXPORT const char *pa__get_author(void);
MOD_EXPORT const char *pa__get_description(void);
MOD_EXPORT const char *pa__get_usage(void);
MOD_EXPORT const char *pa__get_version(void);
MOD_EXPORT const char *pa__get_deprecated(void);
MOD_EXPORT bool pa__load_once(void);

// Can't use constexpr since we need compile time concatenation
#define MASTER_SINK_PARAM "master"
#define LIB_PARAM "lib"
#define FORMAT_PARAM "format"
#define RATE_MAP_PARAM "rate"
#define CHANNELS_PARAM "channels"
#define CHANNEL_MAP_PARAM "channel_map"

PA_MODULE_AUTHOR("Qualcomm Technologies, Inc.");
PA_MODULE_DESCRIPTION(_("TS Renderer"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
// clang-format off
PA_MODULE_USAGE(
    MASTER_SINK_PARAM "=<sink name> "
    LIB_PARAM "=<implementation library> "
    FORMAT_PARAM "=<sample format> "
    RATE_MAP_PARAM "=<sample rate> "
    CHANNELS_PARAM "=<number of channels> "
    CHANNEL_MAP_PARAM "=<channel map> "
);
// clang-format on

static const char *const valid_modargs[] = {
    MASTER_SINK_PARAM,
    LIB_PARAM,
    FORMAT_PARAM,
    RATE_MAP_PARAM,
    CHANNELS_PARAM,
    CHANNEL_MAP_PARAM,
    nullptr};

namespace std {
template <>
struct default_delete<pa_modargs> {
    void operator()(pa_modargs *p) const {
        if (p) {
            pa_modargs_free(p);
        }
    }
};
}  // namespace std

struct TsRendererModule {
    std::shared_ptr<TsRendererCtrl> ts_renderer_ctrl;
};

int pa__init(pa_module *module) {
    pa_assert(module);

    std::unique_ptr<pa_modargs> ma;
    ma = std::unique_ptr<pa_modargs>(pa_modargs_new(module->argument, valid_modargs));
    if (!ma) {
        pa_log_error("failed to parse module arguments");
        return -1;
    }

    const char *master_sink_name = pa_modargs_get_value(ma.get(), MASTER_SINK_PARAM, nullptr);
    if (master_sink_name == nullptr) {
        pa_log_error("Missing master sink name");
        return -1;
    }

    const char *library = pa_modargs_get_value(ma.get(), LIB_PARAM, nullptr);
    if (library == nullptr) {
        pa_log_error("Missing implementation library");
        return -1;
    }

    void *reg = pa_namereg_get(module->core, master_sink_name, PA_NAMEREG_SINK);
    if (reg == nullptr) {
        pa_log_error("Could not find master sink '%s'", master_sink_name);
        return -1;
    }

    pa_sample_spec sample_spec = module->core->default_sample_spec;
    pa_channel_map channel_map = module->core->default_channel_map;
    if ((pa_modargs_get_sample_spec_and_channel_map(ma.get(), &sample_spec, &channel_map, PA_CHANNEL_MAP_DEFAULT) < 0)) {
        pa_log("Invalid sample specification.");
        return -1;
    }

    pa_sink *master_sink = reinterpret_cast<pa_sink *>(reg);

    auto d = new TsRendererModule;
    module->userdata = d;

    pa_log("Creating TsRenderer for %s", master_sink_name);
    d->ts_renderer_ctrl = TsRendererCtrl::create(module, master_sink, library, sample_spec, channel_map);
    if (!d->ts_renderer_ctrl) {
        pa__done(module);
        return -1;
    }

    return 0;
}

void pa__done(pa_module *module) {
    auto d = reinterpret_cast<TsRendererModule *>(module->userdata);
    if (d == nullptr) {
        return;
    }

    delete d;
}
