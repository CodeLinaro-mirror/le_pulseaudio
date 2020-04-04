#ifndef foopulsetraceloghfoo
#define foopulsetraceloghfoo

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

#include <pulse/sample.h>

struct trace_log {
    int fd;
    pa_nsec_t last_checked;
};
typedef struct trace_log trace_log;

#define TRACE_LOG_STATIC_INIT \
    ((trace_log){.fd = -1, .last_checked = 0})

void trace_close(trace_log *log);
ssize_t trace_write(trace_log *log, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

ssize_t trace_newstream(trace_log *log, const char *sink_name);
ssize_t trace_ts(trace_log *log, const char *sink_name, pa_nsec_t ts, pa_nsec_t duration, size_t length);

#endif
