/***
    This file is part of PulseAudio.

    Copyright 2010 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>
    Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.

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

#include "group_manager.h"

#include <fcntl.h>
extern "C" {
#include <pulsecore/dbus-shared.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/ts_clock.h>
}

#include <iomanip>
#include <sstream>

#include "enums.h"
#include "group_sink_ctrl.h"

constexpr pa_usec_t kMinChunkDuration = 5 * PA_USEC_PER_MSEC;

template <>
struct is_flags<pa_sink_input_flags> : std::true_type {};
template <>
struct is_flags<pa_sink_flags_t> : std::true_type {};

template <typename T, size_t S>
constexpr size_t arraySize(T (&)[S]) {
    return S;
}

enum {
    GROUP_MANAGER_SINK_SET_MASTER_ID = PA_SINK_MESSAGE_MAX
};
static void handleDbusGetMinimumLatency(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handleDbusSetAllocatedLatency(DBusConnection *conn, DBusMessage *msg, void *userdata);

static constexpr const char kGroupSinkDbusPathPrefix[] = "/org/pulseaudio/ext/latency/sink";
static constexpr const char kGroupSinkDbusIntf[] = "org.PulseAudio.Ext.Latency.Sink";

static pa_dbus_arg_info dbus_get_minimum_latency_args[] = {
    {"latency", DBUS_TYPE_UINT64_AS_STRING, "out"},
    {"name", DBUS_TYPE_STRING_AS_STRING, "out"}};

static pa_dbus_arg_info dbus_set_allocated_latency_args[] = {
    {"latency", DBUS_TYPE_UINT64_AS_STRING, "in"}};

static pa_dbus_arg_info dbus_minimum_latency_update_args[] = {
    {"latency", DBUS_TYPE_UINT64_AS_STRING, nullptr},
    {"name", DBUS_TYPE_STRING_AS_STRING, nullptr}};

static pa_dbus_method_handler method_handlers[] = {
    {"GetMinimumLatency",
        dbus_get_minimum_latency_args, arraySize(dbus_get_minimum_latency_args),
        handleDbusGetMinimumLatency},
    {"SetAllocatedLatency",
        dbus_set_allocated_latency_args, arraySize(dbus_set_allocated_latency_args),
        handleDbusSetAllocatedLatency}};

static constexpr const char kDbusMinimumLatencyUpdateSignal[] = "MinimumLatencyUpdate";
static pa_dbus_signal_info signals[] = {
    {kDbusMinimumLatencyUpdateSignal,
        dbus_minimum_latency_update_args, arraySize(dbus_minimum_latency_update_args)}};

static pa_dbus_interface_info interface_info = {
    kGroupSinkDbusIntf,
    method_handlers, arraySize(method_handlers),
    nullptr, 0,
    nullptr,
    signals, arraySize(signals)};

static std::string get_sink_input_index_str(const pa_sink_input *input) {
    return input ? std::to_string(input->index) : "<none>";
}

/* Called from I/O thread context */
static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    GroupManager *u = reinterpret_cast<GroupManager *>(PA_SINK(o)->userdata);

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:

            /* The sink is _put() before the sink input is, so let's
             * make sure we don't access it in that time. Also, the
             * sink input is first shut down, the sink second. */
            if (!PA_SINK_IS_LINKED(u->sink_->thread_info.state)
                || !PA_SINK_INPUT_IS_LINKED(u->sink_input_->thread_info.state)) {
                *reinterpret_cast<int64_t *>(data) = 0;
                return 0;
            }
            if (u->timestamp_ == PA_NSEC_INVALID) {
                *reinterpret_cast<int64_t *>(data) = static_cast<int64_t>(u->getTargetLatency())
                    / static_cast<int64_t>(PA_NSEC_PER_USEC);
            } else {
                // cast to signed before the division to correctly extend the sign bit
                int64_t latency =
                    static_cast<int64_t>(u->timestamp_ - ts_clock_now()) / static_cast<int64_t>(PA_NSEC_PER_USEC);
                if (u->remaining_chunk_.length != 0) {
                    latency += static_cast<int64_t>(pa_bytes_to_usec(u->remaining_chunk_.length, &u->sink_->sample_spec));
                }
                *reinterpret_cast<int64_t *>(data) = latency;
            }
            pa_log_debug("latency: %lld", *reinterpret_cast<int64_t *>(data));
            return 0;

        case PA_SINK_MESSAGE_REMOVE_INPUT: {
            pa_sink_input *i = PA_SINK_INPUT(data);
            if (i == u->active_input_) {
                // Input has changed
                u->resetTimestamp();
            }

            break;
        }

        case GROUP_MANAGER_SINK_SET_MASTER_ID:
            // TODO(hudas) We need to implement this later to support "multiple masters" in the group
            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state_cb(pa_sink *s, pa_sink_state_t state,
    PA_UNUSED pa_suspend_cause_t cause) {
    GroupManager *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));

    if (!PA_SINK_IS_LINKED(state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input_->state)) {
        return 0;
    }

    pa_sink_input_cork(u->sink_input_, state == PA_SINK_SUSPENDED);
    return 0;
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state,
    PA_UNUSED pa_suspend_cause_t new_suspend_cause) {
    GroupManager *u;

    pa_assert(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));

    /* When set to running or idle for the first time, request a rewind
     * of the master sink to make sure we are heard immediately */
    if (PA_SINK_IS_OPENED(new_state) && u->sink_->thread_info.state == PA_SINK_INIT) {
        pa_log_debug("Requesting rewind due to state change.");
        pa_sink_input_request_rewind(u->sink_input_, 0, false, true, true);
    }

    if (PA_SINK_IS_OPENED(new_state) && !PA_SINK_IS_OPENED(u->sink_->thread_info.state)) {
        u->play();
    } else if (PA_SINK_IS_OPENED(u->sink_->thread_info.state) && !PA_SINK_IS_OPENED(new_state)) {
        u->stop();
    }

    // When resuming from non-RUNNING, since there has been a discontinuity,
    // restart the computation of the timestamps
    if ((new_state == PA_SINK_RUNNING) && (u->sink_->thread_info.state != PA_SINK_RUNNING)) {
        u->resetTimestamp();
    } else if ((new_state != PA_SINK_RUNNING) && (u->sink_->thread_info.state == PA_SINK_RUNNING)) {
        u->resetTimestamp();  // Mostly for logging when stopping

        pa_log_info("Disabling group sinks (idle/suspended)");
        for (auto sink : u->group_sinks_) {
            sink->enable(false);
        }
    }

    return 0;
}

/* Called from I/O thread context */
static void sink_update_requested_latency(pa_sink *s) {
    GroupManager *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));

    if (!PA_SINK_IS_LINKED(u->sink_->thread_info.state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input_->thread_info.state)) {
        return;
    }

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
        u->sink_input_,
        pa_sink_get_requested_latency_within_thread(s));
}

/* Called from main context */
static void sink_set_mute_cb(pa_sink *s) {
    GroupManager *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));

    if (!PA_SINK_IS_LINKED(s->state)
        || !PA_SINK_INPUT_IS_LINKED(u->sink_input_->state)) {
        return;
    }

    pa_sink_input_set_mute(u->sink_input_, s->muted, s->save_muted);
}

/* Called from I/O thread context */
static bool sink_input_pop_one_cb(pa_sink_input *i, pa_memchunk *chunk) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    if (!PA_SINK_IS_LINKED(u->sink_->thread_info.state)) {
        return false;
    }

    /* Hmm, process any rewind request that might be queued up */
    pa_sink_process_rewind(u->sink_, 0);

    pa_nsec_t duration;
    for (;;) {
        if (u->ts_query_ltime_ == PA_NSEC_INVALID) {
            u->ts_query_ltime_ = ts_clock_now();
            u->ts_query_expected_ts = u->timestamp_;
        }
        if (u->remaining_chunk_.length != 0) {
            *chunk = u->remaining_chunk_;
            pa_memchunk_reset(&u->remaining_chunk_);
        } else if (!pa_sink_render_one(u->sink_, chunk)) {
            if ((!u->in_underrun_) && (!u->has_timestamps_)) {
                // Underrun for a non-timestamped stream => either because
                // lost packet that reduced the end-to-end latency, or because
                // the end-to-end latency was not high enough, or because we
                // are paused => reset the computation.
                // FIXME: check if the active input as timestamps and call
                // pa_sink_render or pa_sink_render_one accordingly, so we can
                // benefit from PA's latency management for the non-timestamp
                // case (i.e. classic PA behavior)
                pa_log_info("Underrun in non-timestamped stream (sink-input %s)",
                    get_sink_input_index_str(u->active_input_).c_str());
                u->in_underrun_ = true;
                // We do not reset the timestamp because the underrun might
                // be caused by the end of the stream and the source might
                // rely on the latency (computed from timestamp) to know when
                // all the audio has been drained. If we reset the timestamp,
                // the latency will go back to the target latency, making the
                // the source believe that up to <target latency> of audio still
                // need to be played.
            }
            return false;
        }

        // If the chunk has no timestamp, see if we need to combine it with
        // more chunks to make a reasonably-sized chunk
        if (chunk->timestamp == PA_NSEC_INVALID) {
            while (chunk->length < u->min_chunk_length_) {
                if (!pa_sink_render_one(u->sink_, &u->remaining_chunk_)) {
                    // No more chunks, return what we have
                    break;
                }
                if (u->remaining_chunk_.timestamp != PA_NSEC_INVALID) {
                    // Next chunk has a timestamp, so it can't be combined
                    break;
                }

                size_t combined_length = chunk->length + u->remaining_chunk_.length;
                if (combined_length > u->max_chunk_length_) {
                    // If the combined chunk would be too big, generate a
                    // half-sized chunk instead, and we'll return the rest
                    // next time
                    pa_assert((combined_length / 2) < u->max_chunk_length_);
                    combined_length = std::max(u->min_chunk_length_, combined_length / 2);
                    combined_length = pa_frame_align(combined_length, &u->sink_->sample_spec);
                }
                size_t copy_length = combined_length - chunk->length;

                chunk = pa_memchunk_make_writable(chunk, combined_length);

                auto d = reinterpret_cast<uint8_t *>(pa_memblock_acquire_chunk(chunk));
                auto s = reinterpret_cast<uint8_t *>(pa_memblock_acquire_chunk(&u->remaining_chunk_));

                memmove(d + chunk->length, s, copy_length);

                pa_memblock_release(chunk->memblock);
                pa_memblock_release(u->remaining_chunk_.memblock);

                u->remaining_chunk_.index += copy_length;
                u->remaining_chunk_.length -= copy_length;
                if (u->remaining_chunk_.length == 0) {
                    pa_memblock_unref(u->remaining_chunk_.memblock);
                    pa_memchunk_reset(&u->remaining_chunk_);
                }
                chunk->length = combined_length;
            }
        }

        // Reset the timestamp computation if the active sink-input has changed
        {
            pa_sink_input *upstream;
            void *state = nullptr;
            for (upstream = reinterpret_cast<pa_sink_input *>(pa_hashmap_iterate(u->sink_->thread_info.inputs, &state, nullptr));
                 upstream != nullptr;
                 upstream = reinterpret_cast<pa_sink_input *>(pa_hashmap_iterate(u->sink_->thread_info.inputs, &state, nullptr))) {
                if (upstream->thread_info.state == PA_SINK_INPUT_RUNNING) {
                    break;
                }
            }
            if ((!upstream) && (u->active_input_ != nullptr)) {
                // No more active input
                u->resetTimestamp(nullptr);

                // No point in disabling the group sinks yet. Lets wait for the
                // suspended state or for a new active input
            } else if (upstream && (u->active_input_ != upstream)) {
                // Input has changed
                u->resetTimestamp(upstream);

                const char *client_str = pa_proplist_gets(upstream->proplist, "slave");
                const char *app_binary = pa_proplist_gets(upstream->proplist, "application.process.binary");
                for (auto sink : u->group_sinks_) {
                    // TODO(jbing): support disabling some groups (e.g. multiroom)
                    // while keeping others enable (e.g. multichannel). Requires
                    // propert handling of latency (i.e. slaves must have
                    // lower latency than lead) or fix for underrun.
                    bool enable = (client_str == nullptr);
                    pa_log_info("%s %s group sink (active input %s, app %s)",
                        (enable ? "Enabling" : "Disabling"),
                        sink->sink->name,
                        get_sink_input_index_str(u->active_input_).c_str(),
                        (app_binary ? app_binary : "<unknown>"));
                    sink->enable(enable);
                }
            }
        }

        if (u->has_timestamps_ || (chunk->timestamp != PA_NSEC_INVALID)) {
            if (!u->has_timestamps_) {
                pa_log_info("Stream with timestamps");
                u->has_timestamps_ = true;
            }
            // We expect the stream to provide timestamps => don't compute any.
            // But still keep track of the timestamp for get_latency()
            if (chunk->timestamp != PA_NSEC_INVALID) {
                u->timestamp_ = chunk->timestamp;
            }

            if (chunk->duration != PA_NSEC_INVALID) {
                duration = chunk->duration;
            } else {
                duration = pa_bytes_to_nsec(chunk->length, &u->sink_->sample_spec);
            }
            u->timestamp_ += duration;
        } else {
            pa_nsec_t now = ts_clock_now();
            pa_nsec_t target_latency = u->getTargetLatency();

            bool recompute_timestamp = false;
            if (target_latency != u->latency_in_use_) {
                pa_log_info("Latency changed to %" PRIu64 "us", target_latency / PA_NSEC_PER_USEC);
                u->latency_in_use_ = target_latency;
                recompute_timestamp = true;
            } else if (u->timestamp_ == PA_NSEC_INVALID) {
                // No timestamp yet => initialize it
                pa_log_info("Initializing packet latency to %" PRIu64 "us",
                    target_latency / PA_NSEC_PER_USEC);
                recompute_timestamp = true;
            } else if (u->in_underrun_) {
                if (u->timestamp_ > (now + target_latency)) {
                    // Typically, at the start of the stream, the first packet
                    // is in the future, and further reads to fill the
                    // downstream buffers only moves the timestamp even further
                    // in the future. If we have an underrun then, we still have
                    // plenty of time to recover before downstream itself
                    // underruns. So there is no need to reset the timestamp
                    // computation.
                    pa_log_info("Quick recovery from underrun, keep timestamp");
                    u->in_underrun_ = false;
                } else {
                    pa_log_info("Recovering from underrun, resetting timestamp for %" PRIu64 "us latency",
                        target_latency / PA_NSEC_PER_USEC);
                    recompute_timestamp = true;
                }
            }

            if (recompute_timestamp) {
                u->timestamp_ = now + target_latency;
                u->in_underrun_ = false;
            }

            duration = pa_bytes_to_nsec(chunk->length, &u->sink_->sample_spec);
            chunk->timestamp = u->timestamp_;
            chunk->duration = duration;

            // The rounding error could accumulate in timestamp_ but as long as
            // everybody play the same data at the same timestamp that won't
            // affect the synchronization, and the time-to-play will smooth over
            // any gap/overlap (i.e. the user won't notice)
            u->timestamp_ += duration;
        }

        if (!pa_memblock_is_silence(chunk->memblock)) {
            // Found a valid chunk
            break;
        }

        // Since we call render_one, a silent chunk means a "hole" in the
        // stream, so we should account for it in the timestamp computation (to
        // maintain the overall latency) but we don't need to play it. And we
        // can immediately ask for the next chunk since we know there is one
        // waiting (it wouldn't be a "hole" otherwise).
        pa_memblock_unref(chunk->memblock);
    }

    // Log query time and reset the timestamp for the next query
    if ((u->ts_query_expected_ts == PA_NSEC_INVALID)
        || (std::abs(static_cast<int64_t>(u->timestamp_ - duration - u->ts_query_expected_ts))
            > static_cast<int64_t>(duration + duration / 2))) {
        // Don't log query time if it took a long time to get the packet and
        // yet the packet is not late, i.e. upstream was paused or we lost
        // packets in between.
        trace_ts_no_ltime(&(u->ts_logging_), u->ts_query_name_.c_str(), chunk->timestamp, chunk->duration, chunk->length);
    } else {
        trace_ts_ltime(&(u->ts_logging_), u->ts_query_name_.c_str(), u->ts_query_ltime_, chunk->timestamp, chunk->duration, chunk->length);
    }
    u->ts_query_ltime_ = PA_NSEC_INVALID;
    // Log packet timestamp itself
    trace_ts(&(u->ts_logging_), u->sink_->name, chunk->timestamp, chunk->duration, chunk->length);

    // TODO(jbing): recompute start time if "discontinuity flag" is set.

    return true;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t /*nbytes*/) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    /* If the sink is not yet linked, there is nothing to rewind */
    if (!PA_SINK_IS_LINKED(u->sink_->thread_info.state))
        return;

    pa_sink_process_rewind(u->sink_, 0);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    /* (6) IF YOU NEED A FIXED BLOCK SIZE ROUND nbytes UP TO MULTIPLES
     * OF IT HERE. THE PA_ROUND_UP MACRO IS USEFUL FOR THAT. */

    pa_sink_set_max_request_within_thread(u->sink_, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    pa_log_debug("Latency range update: %" PRIu64 ", %" PRIu64, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
    pa_sink_set_latency_range_within_thread(u->sink_, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void sink_input_update_sink_fixed_latency_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    /* (7) IF YOU NEED A FIXED BLOCK SIZE ADD THE LATENCY FOR ONE
     * BLOCK MINUS ONE SAMPLE HERE. pa_usec_to_bytes_round_up() IS
     * USEFUL FOR THAT. */

    pa_log_debug("Fixed latency update: %" PRIu64, i->sink->thread_info.fixed_latency);
    pa_sink_set_fixed_latency_within_thread(u->sink_, i->sink->thread_info.fixed_latency);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    if (PA_SINK_IS_LINKED(u->sink_->thread_info.state))
        pa_sink_detach_within_thread(u->sink_);

    pa_sink_set_rtpoll(u->sink_, nullptr);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    pa_sink_set_rtpoll(u->sink_, i->sink->thread_info.rtpoll);
    pa_sink_set_latency_range_within_thread(u->sink_, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    /* (8.1) IF YOU NEED A FIXED BLOCK SIZE ADD THE LATENCY FOR ONE
     * BLOCK MINUS ONE SAMPLE HERE. SEE (7) */
    pa_sink_set_fixed_latency_within_thread(u->sink_, i->sink->thread_info.fixed_latency);
    pa_log_debug("Attached latency, fixed: %" PRIu64 ",  range: %" PRIu64 "-%" PRIu64,
        u->sink_->thread_info.fixed_latency,
        u->sink_->thread_info.min_latency, u->sink_->thread_info.max_latency);

    /* (8.2) IF YOU NEED A FIXED BLOCK SIZE ROUND
     * pa_sink_input_get_max_request(i) UP TO MULTIPLES OF IT
     * HERE. SEE (6) */
    pa_sink_set_max_request_within_thread(u->sink_, pa_sink_input_get_max_request(i));

    if (PA_SINK_IS_LINKED(u->sink_->thread_info.state))
        pa_sink_attach_within_thread(u->sink_);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    /* The order here matters! We first kill the sink so that streams
     * can properly be moved away while the sink input is still connected
     * to the master. */
    pa_sink_input_cork(u->sink_input_, true);
    pa_sink_unlink(u->sink_);
    pa_sink_input_unlink(u->sink_input_);

    pa_sink_input_unref(u->sink_input_);
    u->sink_input_ = nullptr;

    pa_sink_unref(u->sink_);
    u->sink_ = nullptr;

    pa_module_unload_request(u->module_, true);
}

/* Called from main context */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    GroupManager *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(i->userdata));

    if (dest) {
        pa_sink_set_asyncmsgq(u->sink_, dest->asyncmsgq);
        pa_sink_update_flags(u->sink_,
            (pa_sink_flags_t)(PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY),
            dest->flags);
    } else {
        pa_sink_set_asyncmsgq(u->sink_, nullptr);
    }
}

/* Called from Main thread context */
static void handleDbusGetMinimumLatency(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    auto d = reinterpret_cast<GroupManager *>(userdata);

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(d);

    dbus_uint64_t latency = 0;  // no latency to speak of

    DBusMessage *reply;
    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    pa_assert_se(dbus_message_append_args(reply, DBUS_TYPE_UINT64, &latency, DBUS_TYPE_STRING, &d->sink_->name, DBUS_TYPE_INVALID));

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

/* Called from Main thread context */
static void handleDbusSetAllocatedLatency(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    auto d = reinterpret_cast<GroupManager *>(userdata);

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(d);

    DBusError error;
    dbus_error_init(&error);

    dbus_uint64_t latency;
    if ((dbus_message_get_args(msg, &error, DBUS_TYPE_UINT64, &latency, DBUS_TYPE_INVALID)) == 0) {
        pa_log("SetAllocatedLatency invalid args: %s", error.message);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    pa_log_info("Allocated latency for %susec: %" PRIu64, d->sink_->name, latency);
    d->allocated_latency_ = latency * PA_NSEC_PER_USEC;

    pa_dbus_send_empty_reply(conn, msg);
}

void GroupManager::sinkInputRemove() {
    pa_sink_input_cork(sink_->input_to_master, true);
    pa_sink_input_unlink(sink_->input_to_master);
    pa_sink_input_unref(sink_->input_to_master);
    sink_->input_to_master = nullptr;
}

bool GroupManager::sinkInputCreate(pa_sink *master) {
    pa_sink_assert_ref(sink_);
    pa_sink_input_new_data sink_input_data;

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = module_;
    pa_sink_input_new_data_set_sink(&sink_input_data, master, false, true);
    sink_input_data.origin_sink = sink_;
    pa_proplist_setf(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "Sink Input from %s", pa_proplist_gets(sink_->proplist, PA_PROP_DEVICE_DESCRIPTION));
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &sink_->sample_spec);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &sink_->channel_map);
    sink_input_data.flags |= PA_SINK_INPUT_START_CORKED;

    pa_sink_input_new(&sink_input_, module_->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);
    if (!sink_input_) {
        pa_log("Failed to create sink_input");
        return false;
    }

    // no pop callback, this is not supposed to be used with TS rendering
    sink_input_->pop_one = sink_input_pop_one_cb;
    sink_input_->process_rewind = sink_input_process_rewind_cb;
    sink_input_->update_max_request = sink_input_update_max_request_cb;
    sink_input_->update_sink_latency_range = sink_input_update_sink_latency_range_cb;
    sink_input_->update_sink_fixed_latency = sink_input_update_sink_fixed_latency_cb;
    sink_input_->kill = sink_input_kill_cb;
    sink_input_->attach = sink_input_attach_cb;
    sink_input_->detach = sink_input_detach_cb;
    sink_input_->moving = sink_input_moving_cb;
    sink_input_->volume_changed = nullptr;
    sink_input_->mute_changed = nullptr;
    sink_input_->userdata = this;

    sink_->input_to_master = sink_input_;
    return true;
}

static int sink_reconfigure_cb(pa_sink *s, pa_sample_spec *spec, pa_channel_map *map, bool passthrough)
{
    GroupManager *u;
    pa_sink_assert_ref(s);
    pa_assert_se(u = reinterpret_cast<GroupManager *>(s->userdata));
    /*Since the 'master' is asserted during init, assuming that master exists*/
    pa_sink *master = s->input_to_master->sink;
    u->sinkInputRemove();

    s->sample_spec.rate = spec->rate;
    s->sample_spec.format = spec->format;
    /*Note:
    We do not want to reconfigure channels/channel map because it will affect the sink graph settings
    */
    if(!u->sinkInputCreate(master)) {
        return -1;
    }
    pa_sink_input_put(s->input_to_master);
    return 0;
}

GroupManager::~GroupManager() {
    /* See comments in sink_input_kill_cb() above regarding destruction order! */

    if (sink_input_) {
        pa_sink_input_cork(sink_input_, true);
    }

    if (sink_) {
        pa_sink_unlink(sink_);
    }

    if (sink_input_) {
        pa_sink_input_unlink(sink_input_);
        pa_sink_input_unref(sink_input_);
    }

    if (dbus_protocol_) {
        pa_assert_se(pa_dbus_protocol_remove_interface(dbus_protocol_, dbus_path_.c_str(), interface_info.name) >= 0);
        pa_dbus_protocol_unref(dbus_protocol_);
        dbus_protocol_ = nullptr;
    }

    if (sink_) {
        pa_sink_unref(sink_);
    }

    trace_close(&ts_logging_);
}

std::unique_ptr<GroupManager> GroupManager::create(
    pa_module *m, pa_sink *master,
    std::set<GroupSinkCtrl *> groups,
    const pa_sample_spec &sample_spec, const pa_channel_map &channel_map, bool avoid_processing) {
    pa_assert(m);
    pa_assert(master);

    auto u = std::unique_ptr<GroupManager>(new GroupManager);
    if (!u->init(m, master, std::move(groups),
            sample_spec, channel_map, avoid_processing)) {
        return {};
    }
    return u;
}

bool GroupManager::init(pa_module *m, pa_sink *master,
    std::set<GroupSinkCtrl *> groups,
    const pa_sample_spec &sample_spec,
    const pa_channel_map &channel_map,
    bool avoid_processing) {
    pa_sink_new_data sink_data;

    pa_assert(m);
    pa_assert(master);

    module_ = m;

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;
    sink_data.avoid_processing = avoid_processing;
    sink_data.name = pa_xstrdup("group_manager");
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");

    {
        std::string group_names;
        for (auto group : groups) {
            group_names += group->sink->name;
            group_names += ',';
        }
        // Remove extra ','
        if (group_names.size() > 0) {
            group_names.resize(group_names.size() - 1);
        }
        pa_proplist_sets(sink_data.proplist, "group_manager.groups", group_names.c_str());
    }
    pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Group Manager");
    pa_sink_new_data_set_sample_spec(&sink_data, &sample_spec);
    pa_sink_new_data_set_channel_map(&sink_data, &channel_map);
    sink_ = pa_sink_new(m->core, &sink_data, (master->flags & (PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY)) | PA_SINK_SHARE_VOLUME_WITH_MASTER);
    pa_sink_new_data_done(&sink_data);

    if (!sink_) {
        pa_log("Failed to create sink.");
        return false;
    }

    sink_->parent.process_msg = sink_process_msg_cb;
    sink_->set_state_in_main_thread = sink_set_state_cb;
    sink_->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    sink_->update_requested_latency = sink_update_requested_latency;
    sink_->reconfigure = sink_reconfigure_cb;
    pa_sink_set_set_mute_callback(sink_, sink_set_mute_cb);
    sink_->userdata = this;

    pa_sink_set_asyncmsgq(sink_, master->asyncmsgq);
    pa_sink_set_max_rewind(sink_, 0);
    /* Create sink input */
    if (!sinkInputCreate(master)) {
        return false;
    }
    min_chunk_length_ = pa_usec_to_bytes(kMinChunkDuration, &sample_spec);
    max_chunk_length_ = pa_frame_align(pa_mempool_block_size_max(m->core->mempool), &sample_spec);
    pa_assert(min_chunk_length_ <= max_chunk_length_);
    pa_log_debug("Chunk length min: %zu, max: %zu", min_chunk_length_, max_chunk_length_);
    pa_memchunk_reset(&remaining_chunk_);

    ts_query_name_ = std::string(sink_->name) + "_query";

    group_sinks_ = std::move(groups);

    dbus_protocol_ = pa_dbus_protocol_get(module_->core);
    dbus_path_ = kGroupSinkDbusPathPrefix + std::to_string(sink_->index);
    pa_assert_se(pa_dbus_protocol_add_interface(dbus_protocol_, dbus_path_.c_str(), &interface_info, this) >= 0);

    /* The order here is important. The input must be put first,
     * otherwise streams might attach to the sink before the sink
     * input is attached to the master. */
    pa_sink_input_put(sink_input_);
    pa_sink_put(sink_);
    pa_sink_input_cork(sink_input_, false);

    return true;
}

void GroupManager::play() {
    resetTimestamp();
    trace_newstream(&ts_logging_, ts_query_name_.c_str());
    trace_newstream(&ts_logging_, sink_->name);
}

void GroupManager::stop() {
    trace_close(&ts_logging_);
}

void GroupManager::setMasterId(const std::string &master_id) {
    auto master_id_str = pa_xstrdup(master_id.c_str());
    pa_asyncmsgq_post(sink_->asyncmsgq, PA_MSGOBJECT(sink_), GROUP_MANAGER_SINK_SET_MASTER_ID,
        master_id_str, 0, nullptr, pa_xfree);
}

void GroupManager::resetTimestamp(pa_sink_input *new_input) {
    if (active_input_ == new_input) {
        return;
    }

    pa_log_info("Resetting timestamps computation (new sink_input: %s)", get_sink_input_index_str(new_input).c_str());
    active_input_ = new_input;
    has_timestamps_ = false;
    timestamp_ = PA_NSEC_INVALID;
}

/* Called from I/O thread context */
pa_nsec_t GroupManager::getTargetLatency() {
    pa_nsec_t target_latency = allocated_latency_;
    if (target_latency != PA_NSEC_INVALID) {
        return target_latency;
    }

    target_latency =
        (((sink_->flags & PA_SINK_DYNAMIC_LATENCY) == 0)
                ? sink_->thread_info.fixed_latency
                : ((sink_->thread_info.min_latency + sink_->thread_info.max_latency) / 2))
        * PA_NSEC_PER_USEC;
    return target_latency;
}
