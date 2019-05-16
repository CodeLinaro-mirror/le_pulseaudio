/***
  Copyright (c) 2019, The Linux Foundation. All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
   ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
   BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
   OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
   IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "pulsecore/config.h"
#include <gst/gst.h>
#include <stdio.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>
#include <dbus/dbus.h>
#include "inc/dbus-audiorate.h"

#define AUDIORATE_INTERFACE "org.filtereffects.audiorate"
#define AUDIORATE_DBUS_PATH "/org/filtereffects/audiorate"

struct audiorate_dbus_handle {
    GstElement *audiorate;
};

enum audiorate_property_index {
    AUDIORATE_ADD,
    AUDIORATE_DROP,
    AUDIORATE_IN,
    AUDIORATE_OUT,
    AUDIORATE_SILENT,
    AUDIORATE_SKIP_TO_FIRST,
    AUDIORATE_TOLERANCE,
    AUDIORATE_PROPERTY_MAX
};

static void get_audiorate_property(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void set_audiorate_property(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *userdata);
static void get_all_audiorate_properties(DBusConnection *conn, DBusMessage *msg, void *userdata);

static pa_dbus_property_handler audiorate_property_handlers[AUDIORATE_PROPERTY_MAX] = {
        [AUDIORATE_ADD] = {
                .property_name = "add",
                .type = "t",
                .get_cb = get_audiorate_property,
                .set_cb = set_audiorate_property },
        [AUDIORATE_DROP] = {
                .property_name = "drop",
                .type = "t",
                .get_cb = get_audiorate_property,
                .set_cb = set_audiorate_property },
        [AUDIORATE_IN] = {
                .property_name = "in",
                .type = "t",
                .get_cb = get_audiorate_property,
                .set_cb = set_audiorate_property },
        [AUDIORATE_OUT] = {
                .property_name = "out",
                .type = "t",
                .get_cb = get_audiorate_property,
                .set_cb = set_audiorate_property },
        [AUDIORATE_SILENT] = {
                .property_name = "silent",
                .type = "b",
                .get_cb = get_audiorate_property,
                .set_cb = set_audiorate_property },
        [AUDIORATE_SKIP_TO_FIRST] = {
                .property_name = "skip-to-first",
                .type = "b",
                .get_cb = get_audiorate_property,
                .set_cb = set_audiorate_property },
        [AUDIORATE_TOLERANCE] = {
                .property_name = "tolerance",
                .type = "t",
                .get_cb = get_audiorate_property,
                .set_cb = set_audiorate_property }
};

static pa_dbus_interface_info audiorate_info = {
        .name = AUDIORATE_INTERFACE,
        .method_handlers = NULL,
        .n_method_handlers = 0,
        .property_handlers = audiorate_property_handlers,
        .n_property_handlers = AUDIORATE_PROPERTY_MAX,
        .get_all_properties_cb = get_all_audiorate_properties,
        .signals = NULL,
        .n_signals = 0
};

static void get_audiorate_property(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    const char *audiorate_property_string = NULL;
    int prop_range;
    dbus_bool_t audiorate_property_value_bool;
    dbus_uint64_t audiorate_property_value_uint64;
    char * audiorate_property_value_string;
    DBusMessageIter msg_iter;
    struct audiorate_dbus_handle *u = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u = userdata);
    pa_assert(u->audiorate);
    pa_assert_se(dbus_message_iter_init(msg, &msg_iter));
    pa_assert_se(dbus_message_iter_next(&msg_iter));

    dbus_message_iter_get_basic(&msg_iter, &audiorate_property_string);
    pa_assert(audiorate_property_string);

    for (prop_range = 0; prop_range < AUDIORATE_PROPERTY_MAX; prop_range++) {
        if (strcmp(audiorate_property_handlers[prop_range].property_name, audiorate_property_string) == 0)
            break;
    }

    if (prop_range == AUDIORATE_PROPERTY_MAX)
        return;

    if (strcmp(audiorate_property_handlers[prop_range].type, "b") == 0) {
        g_object_get(u->audiorate, audiorate_property_string, &audiorate_property_value_bool, NULL);
        pa_log("GStreamer audiorate g_object_get bool value %d", audiorate_property_value_bool);
        pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_BOOLEAN, &audiorate_property_value_bool);
    } else if (strcmp(audiorate_property_handlers[prop_range].type, "t") == 0) {
        g_object_get(u->audiorate, audiorate_property_string, &audiorate_property_value_uint64, NULL);
        pa_log("GStreamer audiorate g_object_get uint value %lu", audiorate_property_value_uint64);
        pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT64, &audiorate_property_value_uint64);
    }
}

static void set_audiorate_property(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *userdata) {
    const char *audiorate_property_string = NULL;
    int prop_range;
    dbus_bool_t audiorate_property_value_bool;
    dbus_uint64_t audiorate_property_value_uint64;
    char *audiorate_property_value_string;
    DBusMessageIter msg_iter;
    struct audiorate_dbus_handle *u = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u = userdata);
    pa_assert(u->audiorate);
    pa_assert_se(dbus_message_iter_init(msg, &msg_iter));
    pa_assert_se(dbus_message_iter_next(&msg_iter));

    dbus_message_iter_get_basic(&msg_iter, &audiorate_property_string);
    pa_assert(audiorate_property_string);

    for (prop_range = 0; prop_range < AUDIORATE_PROPERTY_MAX; prop_range++) {
        if (strcmp(audiorate_property_handlers[prop_range].property_name, audiorate_property_string) == 0)
            break;
    }

    if (prop_range == AUDIORATE_PROPERTY_MAX) {
        return;
    }

    if (strcmp(audiorate_property_handlers[prop_range].type, "b") == 0) {
        dbus_message_iter_get_basic(iter, &audiorate_property_value_bool);
        pa_log("GStreamer audiorate g_object_set bool value %d", audiorate_property_value_bool);
        g_object_set(u->audiorate, audiorate_property_string, audiorate_property_value_bool, NULL);
    } else if (strcmp(audiorate_property_handlers[prop_range].type, "t") == 0
            && strcmp(audiorate_property_handlers[prop_range].property_name, "tolerance") == 0) {
        dbus_message_iter_get_basic(iter, &audiorate_property_value_uint64);
        pa_log("GStreamer audiorate g_object_set uint value %lu", audiorate_property_value_uint64);
        g_object_set(u->audiorate, audiorate_property_string, audiorate_property_value_uint64, NULL);
    }

    /* Reply without appending any arguments to the message */
    pa_dbus_send_empty_reply(conn, msg);
}

static void get_all_audiorate_properties(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct audiorate_dbus_handle *u;
    int prop_range;
    dbus_bool_t audiorate_property_value_bool;
    dbus_uint64_t audiorate_property_value_uint64;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter;
    DBusMessageIter dict_iter;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u = userdata);
    pa_assert(u->audiorate);
    pa_assert_se((reply = dbus_message_new_method_return(msg)));

    dbus_message_iter_init_append(reply, &msg_iter);
    pa_assert_se(dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter));

    for (prop_range = 0; prop_range < AUDIORATE_PROPERTY_MAX; prop_range++) {
        if (strcmp(audiorate_property_handlers[prop_range].type, "b") == 0) {
            g_object_get(u->audiorate, audiorate_property_handlers[prop_range].property_name,
                    &audiorate_property_value_bool, NULL);
            pa_dbus_append_basic_variant_dict_entry(&dict_iter, audiorate_property_handlers[prop_range].property_name,
                    DBUS_TYPE_BOOLEAN, &audiorate_property_value_bool);
        } else if (strcmp(audiorate_property_handlers[prop_range].type, "t") == 0) {
            g_object_get(u->audiorate, audiorate_property_handlers[prop_range].property_name,
                    &audiorate_property_value_uint64, NULL);
            pa_dbus_append_basic_variant_dict_entry(&dict_iter, audiorate_property_handlers[prop_range].property_name,
                    DBUS_TYPE_UINT64, &audiorate_property_value_uint64);
        }
    }

    pa_assert_se(dbus_message_iter_close_container(&msg_iter, &dict_iter));
    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

struct pa_dbus_interface_info * audiorate_get_inteface_info(void) {
    return &audiorate_info;
}

const char *audiorate_get_interface_name(void) {
    return AUDIORATE_INTERFACE;
}

int audiorate_prop_initialize(void **audiorate_data) {
    *audiorate_data = pa_xnew0(struct audiorate_dbus_handle, 1);

    if (*audiorate_data) {
        return 0;
    } else {
        return 1;
    }
}

int audiorate_prop_deinitialize(void *audiorate_data) {
    free(audiorate_data);

    return 0;
}

int audiorate_set_gst_element(void *u, GstElement *audiorate) {
    struct audiorate_dbus_handle * dbus_handle = (struct audiorate_dbus_handle *) u;
    dbus_handle->audiorate = audiorate;

    return 0;
}

char *audiorate_get_dbus_path(void) {
    return AUDIORATE_DBUS_PATH;
}
