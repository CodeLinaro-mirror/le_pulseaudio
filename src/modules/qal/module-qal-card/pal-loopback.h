/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __PAL_LOOPBACK_H__
#define __PAL_LOOPBACK_H__

#include <PalApi.h>
#include <PalDefs.h>

#define MAX_USECASE_NAME_LENGTH        60
#define MAX_LOOPBACK_PROFILES           5
#define LOOPBACK_NUM_DEVICES            2

#define E_SUCCESS                       0
#define E_FAILURE                      -1

typedef struct {
    char *name;
    char *description;
    char **in_port_conf_string;
    char **out_port_conf_string;
    pa_hashmap *in_ports;
    pa_hashmap *out_ports;
} pa_pal_loopback_config;

typedef struct pa_pal_loopback_module_data {
    char *dbus_path;
    void *prv_data;
    int session_count;
    pa_card *card;
    pa_module *m;
    pa_dbus_protocol *dbus_protocol;
    pa_hashmap *loopback_confs;
    pa_hashmap *session_data;
} pa_pal_loopback_module_data_t;

typedef struct pa_pal_loopback_session_data {
    char *obj_path;
    char usecase[MAX_USECASE_NAME_LENGTH];
    pa_pal_loopback_module_data_t *common;
    pa_pal_loopback_config *loopback_config[MAX_LOOPBACK_PROFILES];
} pa_pal_loopback_ses_data_t;

/* Use case type and name entry */
typedef enum {
    PA_PAL_UC_BT_A2DP_SINK = 1,
    PA_PAL_UC_BT_SCO,
    PA_PAL_UC_BT_MAX,
} pa_bt_usecase_type_t;

static const char *usecase_name_list[] = {
    [PA_PAL_UC_BT_A2DP_SINK] = "bta2dp",
    [PA_PAL_UC_BT_SCO]       = "btsco"
};

int pa_pal_loopback_init(pa_core *core, pa_card *card, pa_hashmap *loopbacks,
        void *prv_data, pa_module *m);

void pa_pal_loopback_deinit(void);

#endif //__PAL_LOOPBACK_H__

