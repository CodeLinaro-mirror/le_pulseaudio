/*
 * Copyright 2019 The Linux Foundation. All rights reserved.
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


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "test-util.h"

#include <check.h>

#include <pulse/pulseaudio.h>

pa_test_context *ctx;
uint32_t sink_idx = PA_INVALID_INDEX;
static const char *bname = NULL;

static void timestamp_setup() {
    ctx = pa_test_context_new(bname);

    sink_idx = pa_test_context_load_null_sink(ctx, "timestamp_mode=yes");
}

static void timestamp_teardown() {
    pa_test_context_free(ctx);
}

#define SAMPLE_FORMAT PA_SAMPLE_S16LE
#define RATE 48000
#define CHANNELS 2

static void nop_free_cb(void *p) {}

static void write_cb(pa_stream *s, size_t nbytes, void *userdata) {
    pa_test_context *c = (pa_test_context *) userdata;

    pa_assert(c);

    pa_threaded_mainloop_signal(c->mainloop, false);
}

START_TEST (timestamp_test) {
    pa_format_info *format;
    pa_stream *s;
    int rate = RATE;
    int channels = CHANNELS;
    pa_sample_format_t sample_format = SAMPLE_FORMAT;
    /* Play out a a second of data */
    uint32_t data[RATE * CHANNELS] = { 0, };
    size_t index = 0;
    size_t sample_size = sizeof(data[0]);
    size_t nsamples = sizeof(data) / sample_size;
    pa_sample_spec spec;
    pa_nsec_t timestamp = 0;

    format = pa_format_info_new();
    format->encoding = PA_ENCODING_PCM;
    pa_format_info_set_sample_format(format, sample_format);
    pa_format_info_set_rate(format, rate);
    pa_format_info_set_channels(format, channels);

    pa_format_info_to_sample_spec(format, &spec, NULL);

    s = pa_test_context_create_stream(ctx, "timestamp test", sink_idx, format, PA_STREAM_NOFLAGS, NULL, 0);
    fail_unless(s != NULL);

    pa_threaded_mainloop_lock(ctx->mainloop);

    pa_stream_set_write_callback(s, write_cb, ctx);

    while (index < nsamples) {
        /* We write a "small" number of samples at a time with the expectation
         * that the sink won't try to read a smaller chunk than this (which
         * would cause the timestamp/duration to be invalidated. */
        pa_nsec_t duration = 10 * PA_NSEC_PER_MSEC;
        size_t to_write = pa_nsec_to_bytes(duration, &spec);
        int r;

        while (pa_stream_writable_size(s) > 0 && index < nsamples) {
            r = pa_stream_write_ts(s, &data[index], to_write, nop_free_cb, index, PA_SEEK_ABSOLUTE, timestamp, duration,
                                   PA_BUFFER_NOFLAGS);
            fail_unless(r == 0);

            timestamp += duration;
            index += to_write;
        }

        /* Wait for write callback */
        pa_threaded_mainloop_wait(ctx->mainloop);
    }

    fail_unless(pa_stream_get_state(s) == PA_STREAM_READY);

    pa_threaded_mainloop_unlock(ctx->mainloop);

    pa_test_context_destroy_stream(ctx, s);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    bname = argv[0];

    s = suite_create("Timestamp");
    tc = tcase_create("timestamp");
    tcase_add_checked_fixture(tc, timestamp_setup, timestamp_teardown);
    tcase_add_test(tc, timestamp_test);
    tcase_set_timeout(tc, 2);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
