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

#ifndef fooqsthwutilfoo
#define fooqsthwutilfoo

typedef struct pa_qal_voiceui_hooks pa_qal_voiceui_hooks;

typedef enum pa_qal_voiceui_hook {
    PA_HOOK_QAL_VOICEUI_START_DETECTION,
    PA_HOOK_QAL_VOICEUI_STOP_DETECTION,
    PA_HOOK_QAL_VOICEUI_MAX,
} pa_qal_voiceui_hook_t;

typedef struct {
    struct qal_st_phrase_recognition_event phrase_event;
    uint64_t timestamp;
} pa_qal_st_phrase_recognition_event;

struct pa_qal_voiceui_hooks {
    pa_hook hooks[PA_HOOK_QAL_VOICEUI_MAX];
};

#endif //fooqsthwutilfoo
