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

#include "ts_renderer_ctrl.h"

#include <fcntl.h>

#include <pulse/gccmacro.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>
PA_C_DECL_BEGIN
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/ltdl-helper.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
PA_C_DECL_END

#include <iomanip>
#include <sstream>

#include "clock.h"
#include "enums.h"

static constexpr pa_usec_t kMaxSilence = 1 * 1000;  // 1ms

template <>
struct is_flags<pa_sink_input_flags> : std::true_type {};
template <>
struct is_flags<pa_sink_flags_t> : std::true_type {};

/* Called from I/O thread context */
static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    TsRendererCtrl *u = reinterpret_cast<TsRendererCtrl *>(PA_SINK(o)->userdata);

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:

            /* The sink is _put() before the sink input is, so let's
             * make sure we don't access it in that time. Also, the
             * sink input is first shut down, the sink second. */
            if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)
                || !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state)) {
                *reinterpret_cast<int64_t *>(data) = 0;
                return 0;
            }

            *reinterpret_cast<int64_t *>(data) = static_cast<int64_t>(u->getLatency() / PA_NSEC_PER_USEC);
            pa_log_error("latency: %" PRId64, *reinterpret_cast<int64_t *>(data));

            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state_cb(pa_sink *s, pa_sink_state_t state,
    PA_UNUSED pa_suspend_cause_t cause) {
    TsRendererCtrl *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(s->userdata));

    if (!PA_SINK_IS_LINKED(state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input->state)) {
        return 0;
    }

    pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);
    return 0;
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state,
    PA_UNUSED pa_suspend_cause_t new_suspend_cause) {
    TsRendererCtrl *u;

    pa_assert(s);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(s->userdata));

    /* When set to running or idle for the first time, request a rewind
     * of the master sink to make sure we are heard immediately */
    if (PA_SINK_IS_OPENED(new_state) && u->sink->thread_info.state == PA_SINK_INIT) {
        pa_log_debug("Requesting rewind due to state change.");
        pa_sink_input_request_rewind(u->sink_input, 0, false, true, true);
    }

    if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(s->thread_info.state)) {
        u->ts_renderer->setState(u->ts_renderer, TS_RENDERER_PLAYING);
    } else if (!PA_SINK_IS_OPENED(new_state)) {
        u->ts_renderer->setState(u->ts_renderer, TS_RENDERER_STOPPED);
    }

    return 0;
}

/* Called from I/O thread context */
static void sink_request_rewind(pa_sink *s) {
    TsRendererCtrl *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(s->userdata));

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state)) {
        return;
    }

    /* Just hand this one over to the master sink */
    pa_sink_input_request_rewind(u->sink_input,
        s->thread_info.rewind_nbytes + 0,
        true, false, false);
}

/* Called from I/O thread context */
static void sink_update_requested_latency(pa_sink *s) {
    TsRendererCtrl *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(s->userdata));

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state)) {
        return;
    }

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
        u->sink_input,
        pa_sink_get_requested_latency_within_thread(s));
}

/* Called from main context */
static void sink_set_mute_cb(pa_sink *s) {
    TsRendererCtrl *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(s->userdata));

    if (!PA_SINK_IS_LINKED(s->state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input->state)) {
        return;
    }

    pa_sink_input_set_mute(u->sink_input, s->muted, s->save_muted);
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)) {
        return -1;
    }

    /* Hmm, process any rewind request that might be queued up */
    pa_sink_process_rewind(u->sink, 0);

    pa_nsec_t playback_time = ts_clock_now() + u->getLatency();
    int res = u->ts_renderer->render(u->ts_renderer, nullptr, chunk, nbytes, playback_time);
    if (res < 0) {
        return res;
    }
    while (chunk->length <= 0) {
        size_t block_size_max_sink = pa_frame_align(pa_mempool_block_size_max(i->core->mempool), &u->sink->sample_spec);
        pa_memchunk nchunk;
        pa_sink_render(u->sink, block_size_max_sink, &nchunk);

        // Pass chunks without a timestamp (likely a silent chunk) directly
        if (nchunk.timestamp == PA_NSEC_INVALID) {
            // Pass silence directly
            *chunk = nchunk;
            // Since we asked for as big a chunk as possible, we got a huge
            // silent block, so reduce the size to something more reasonable
            chunk->length = std::min(nbytes, pa_usec_to_bytes(kMaxSilence, &u->sink->sample_spec));
            return 0;
        }

        playback_time = ts_clock_now() + u->getLatency();
        res = u->ts_renderer->render(u->ts_renderer, &nchunk, chunk, nbytes, playback_time);
        if (res < 0) {
            return res;
        }
    }

    u->returned_since_last_full_ += chunk->length;
    if (chunk->length == nbytes) {
        // We are returning what was requested, so lets assume the sink
        // will flush out and thus we'll get the correct latency when we ask
        u->returned_since_last_full_ = 0;
    }

    return 0;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t /*nbytes*/) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    /* If the sink is not yet linked, there is nothing to rewind */
    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    // Since we don't support rewind
    u->sink->thread_info.rewind_nbytes = 0;

    pa_sink_process_rewind(u->sink, 0);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    /* FIXME: Too small max_rewind:
     * https://bugs.freedesktop.org/show_bug.cgi?id=53709 */
    pa_sink_set_max_rewind_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    /* (6) IF YOU NEED A FIXED BLOCK SIZE ROUND nbytes UP TO MULTIPLES
     * OF IT HERE. THE PA_ROUND_UP MACRO IS USEFUL FOR THAT. */

    pa_sink_set_max_request_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void sink_input_update_sink_fixed_latency_cb(pa_sink_input *i) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    /* (7) IF YOU NEED A FIXED BLOCK SIZE ADD THE LATENCY FOR ONE
     * BLOCK MINUS ONE SAMPLE HERE. pa_usec_to_bytes_round_up() IS
     * USEFUL FOR THAT. */

    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    if (PA_SINK_IS_LINKED(u->sink->thread_info.state))
        pa_sink_detach_within_thread(u->sink);

    pa_sink_set_rtpoll(u->sink, nullptr);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    pa_sink_set_rtpoll(u->sink, i->sink->thread_info.rtpoll);
    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    /* (8.1) IF YOU NEED A FIXED BLOCK SIZE ADD THE LATENCY FOR ONE
     * BLOCK MINUS ONE SAMPLE HERE. SEE (7) */
    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);

    /* (8.2) IF YOU NEED A FIXED BLOCK SIZE ROUND
     * pa_sink_input_get_max_request(i) UP TO MULTIPLES OF IT
     * HERE. SEE (6) */
    pa_sink_set_max_request_within_thread(u->sink, pa_sink_input_get_max_request(i));

    /* FIXME: Too small max_rewind:
     * https://bugs.freedesktop.org/show_bug.cgi?id=53709 */
    pa_sink_set_max_rewind_within_thread(u->sink, pa_sink_input_get_max_rewind(i));

    if (PA_SINK_IS_LINKED(u->sink->thread_info.state))
        pa_sink_attach_within_thread(u->sink);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    /* The order here matters! We first kill the sink so that streams
     * can properly be moved away while the sink input is still connected
     * to the master. */
    pa_sink_input_cork(u->sink_input, true);
    pa_sink_unlink(u->sink);
    pa_sink_input_unlink(u->sink_input);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = nullptr;

    pa_sink_unref(u->sink);
    u->sink = nullptr;

    pa_module_unload_request(u->module, true);
}

/* Called from main context */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    if (dest) {
        pa_sink_set_asyncmsgq(u->sink, dest->asyncmsgq);
        pa_sink_update_flags(u->sink,
            (pa_sink_flags_t)(PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY),
            dest->flags);
    } else {
        pa_sink_set_asyncmsgq(u->sink, nullptr);
    }
}

/* Called from main context */
static void sink_input_mute_changed_cb(pa_sink_input *i) {
    TsRendererCtrl *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<TsRendererCtrl *>(i->userdata));

    pa_sink_mute_changed(u->sink, i->muted);
}

pa_nsec_t TsRendererCtrl::getLatency() {
    // Get current playtime: latency in the sink + latency in the sink-input
    // buffer + latency of data not yet pushed to the sink-input queue
    // The "not yet pushed" is based on the assumption that the sinks use
    // the "render full" function family, like pa_sink_render_full (true for
    // Alsa and QAHW sinks)
    // TODO(jbing): with real timestamps, that assumption will likely not
    // be true, revisit then.
    return static_cast<pa_usec_t>(pa_sink_get_latency_within_thread(sink_input->sink, false)) * PA_NSEC_PER_USEC
        + pa_bytes_to_nsec(
              pa_memblockq_get_length(sink_input->thread_info.render_memblockq)
                  + returned_since_last_full_,
              &sink_input->sample_spec);
}

std::shared_ptr<TsRendererCtrl> TsRendererCtrl::create(pa_module *m,
    pa_sink *master, const char *library,
    const pa_sample_spec &sample_spec, const pa_channel_map &channel_map) {
    pa_sink_input_new_data sink_input_data;
    pa_sink_new_data sink_data;

    pa_assert(m);
    pa_assert(master);

    auto u = std::shared_ptr<TsRendererCtrl>{new TsRendererCtrl};
    u->module = m;

    std::string name = std::string(master->name) + ".ts_renderer";

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;

    sink_data.name = pa_xstrdup(name.c_str());
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");
    pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "TS Renderer %s", master->name);
    pa_sink_new_data_set_sample_spec(&sink_data, &sample_spec);
    pa_sink_new_data_set_channel_map(&sink_data, &channel_map);
    u->sink = pa_sink_new(m->core, &sink_data, (master->flags & (PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY)) | PA_SINK_SHARE_VOLUME_WITH_MASTER);
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg_cb;
    u->sink->set_state_in_main_thread = sink_set_state_cb;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    u->sink->update_requested_latency = sink_update_requested_latency;
    u->sink->request_rewind = sink_request_rewind;
    pa_sink_set_set_mute_callback(u->sink, sink_set_mute_cb);
    u->sink->userdata = u.get();

    pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    pa_sink_input_new_data_set_sink(&sink_input_data, master, false, true);
    sink_input_data.origin_sink = u->sink;
    pa_proplist_setf(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "TS Renderer Stream from %s", pa_proplist_gets(u->sink->proplist, PA_PROP_DEVICE_DESCRIPTION));
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &u->sink->sample_spec);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &u->sink->channel_map);
    sink_input_data.flags |= PA_SINK_INPUT_START_CORKED;

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!u->sink_input)
        goto fail;

    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->update_sink_latency_range = sink_input_update_sink_latency_range_cb;
    u->sink_input->update_sink_fixed_latency = sink_input_update_sink_fixed_latency_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->moving = sink_input_moving_cb;
    u->sink_input->volume_changed = nullptr;
    u->sink_input->mute_changed = sink_input_mute_changed_cb;
    u->sink_input->userdata = u.get();

    u->sink->input_to_master = u->sink_input;

    /* (9) INITIALIZE ANYTHING ELSE YOU NEED HERE */
    u->dl = lt_dlopenext(library);
    if (u->dl == nullptr) {
        pa_log("Failed to open support library for '%s': %s", name.c_str(), lt_dlerror());
        goto fail;
    }
    {
        ts_renderer_init_proto *init = reinterpret_cast<ts_renderer_init_proto *>(pa_load_sym(u->dl, nullptr, "ts_renderer_init"));
        if (init == nullptr) {
            pa_log("Failed to find 'ts_renderer_init' symbol");
            goto fail;
        }
        u->ts_renderer = (*init)(name.c_str(), &sample_spec, &u->sink->silence);
    }

    /* The order here is important. The input must be put first,
     * otherwise streams might attach to the sink before the sink
     * input is attached to the master. */
    pa_sink_input_put(u->sink_input);
    pa_sink_put(u->sink);
    pa_sink_input_cork(u->sink_input, false);
    return u;

fail:

    return {};
}

TsRendererCtrl::~TsRendererCtrl() {
    /* See comments in sink_input_kill_cb() above regarding
     * destruction order! */

    if (sink_input)
        pa_sink_input_cork(sink_input, true);

    if (sink)
        pa_sink_unlink(sink);

    if (sink_input) {
        pa_sink_input_unlink(sink_input);
        pa_sink_input_unref(sink_input);
    }

    if (dl != nullptr) {
        if (ts_renderer) {
            ts_renderer_done_proto *done = reinterpret_cast<ts_renderer_done_proto *>(pa_load_sym(dl, nullptr, "ts_renderer_done"));
            if (done == nullptr) {
                pa_log("Failed to find 'ts_renderer_done' symbol");
            } else {
                done(ts_renderer);
            }
        }
        lt_dlclose(dl);
    }

    if (sink)
        pa_sink_unref(sink);
}
