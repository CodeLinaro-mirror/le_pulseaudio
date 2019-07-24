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
#include "pulsecore_config.h"

#include "ts_logging.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <pulse/timeval.h>

#include <chrono>

#include "clock.h"

#if !defined(__cplusplus)
#    define nullptr NULL
#endif  // !__cplusplus

static bool trace_is_valid(trace_log *log) {
    return ((log != nullptr) && (log->fd >= 0));
}

trace_log trace_open(trace_log *old_log) {
    if (trace_is_valid(old_log)) {
        // Already opened
        return *old_log;
    }
    trace_log new_log = TRACE_LOG_STATIC_INIT;
    new_log.fd = open("/sys/kernel/debug/tracing/trace_marker", O_WRONLY);
    if (old_log != nullptr) {
        *old_log = new_log;
    }
    return new_log;
}
void trace_close(trace_log *log) {
    if (trace_is_valid(log)) {
        ::close(log->fd);
        log->fd = -1;
    }
}
ssize_t trace_write(trace_log *log, const char *fmt, ...) {
    if (!trace_is_valid(log)) {
        errno = EBADF;
        return -1;
    }

    va_list ap;
    va_start(ap, fmt);

    char buf[256];
    errno = 0;
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) {
        // Assumes errno was set by printf
        return -1;
    }
    if (static_cast<size_t>(len) > sizeof(buf)) {
        len = sizeof(buf);
    }
    ssize_t count = ::write(log->fd, buf, static_cast<size_t>(len));
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
ssize_t trace_ts(trace_log *log, const char *sink_name, pa_nsec_t ts, pa_nsec_t duration) {
    return trace_write(log, "s=%s ts=%" PRIu64 " d=%" PRIu64 " ltime=%" PRId64,
        sink_name,
        (ts / PA_NSEC_PER_USEC),
        (duration / PA_NSEC_PER_USEC),
        (ts_clock_now() / PA_NSEC_PER_USEC));
}
