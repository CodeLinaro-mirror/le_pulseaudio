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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <pulse/timeval.h>
#include <pulsecore/ts_clock.h>

#include "trace_log.h"

#define TRACE_CHECK_PERIOD (1 * PA_NSEC_PER_SEC)

static bool verify_open(trace_log *log) {
    if (log == NULL) {
        return false;
    }
    if (log->fd >= 0) {
        return true;
    }

    pa_nsec_t now = ts_clock_now();
    if ((now > log->last_checked) && ((now - log->last_checked) < TRACE_CHECK_PERIOD)) {
        return false;
    }
    log->last_checked = now;

    log->fd = open("/sys/kernel/debug/tracing/instances/ttp/trace_marker", O_WRONLY);
    return (log->fd >= 0);
}

void trace_close(trace_log *log) {
    if ((log != NULL) && (log->fd >= 0)) {
        close(log->fd);
        log->fd = -1;
        log->last_checked = 0;
    }
}

ssize_t trace_write(trace_log *log, const char *fmt, ...) {
    va_list ap;
    char buf[256];
    int len;
    ssize_t count;

    if (!verify_open(log)) {
        errno = EBADF;
        return -1;
    }

    va_start(ap, fmt);

    errno = 0;
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) {
        // Assumes errno was set by printf
        return -1;
    }
    if (((size_t)len) > sizeof(buf)) {
        len = sizeof(buf);
    }
    count = write(log->fd, buf, (size_t)len);
    if (count < 0) {
        int old_errno = errno;
        trace_close(log);
        errno = old_errno;
        return -1;
    }
    if (count != len) {
        trace_close(log);
        return 0;
    }
    return 1;
}

ssize_t trace_newstream(trace_log *log, const char *sink_name) {
    return trace_write(log, "s=%s new_stream", sink_name);
}

ssize_t trace_ts(trace_log *log, const char *sink_name, pa_nsec_t ts, pa_nsec_t duration, size_t length) {
    return trace_write(log, "s=%s ts=%" PRIu64 " d=%" PRIu64 " ltime=%" PRId64 " sz=%zu",
        sink_name,
        (ts / PA_NSEC_PER_USEC),
        (duration / PA_NSEC_PER_USEC),
        (ts_clock_now() / PA_NSEC_PER_USEC),
        length);
}

ssize_t trace_ts_ltime(trace_log *log, const char *sink_name, pa_nsec_t ltime, pa_nsec_t ts, pa_nsec_t duration, size_t length) {
    return trace_write(log, "s=%s ts=%" PRIu64 " d=%" PRIu64 " ltime=%" PRId64 " sz=%zu",
        sink_name,
        (ts / PA_NSEC_PER_USEC),
        (duration / PA_NSEC_PER_USEC),
        (ltime / PA_NSEC_PER_USEC),
        length);
}

ssize_t trace_ts_no_ltime(trace_log *log, const char *sink_name, pa_nsec_t ts, pa_nsec_t duration, size_t length) {
    return trace_write(log, "s=%s ts=%" PRIu64 " d=%" PRIu64 " ltime=- sz=%zu",
        sink_name,
        (ts / PA_NSEC_PER_USEC),
        (duration / PA_NSEC_PER_USEC),
        length);
}
