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

#include "inc/dbus-filter.h"
#include <pulsecore/modargs.h>

static const char* const valid_properties[] = {
        "skip_to_first",
        /*Add properties as required */
        NULL
};

int init_filter_plugin(const char *plugin, void **handle) {
    pa_assert(plugin);
    int ret = 0;

    if (pa_streq("audiorate", plugin)) {
        ret = audiorate_prop_initialize(handle);
    }

    return ret;
}

int set_filter_plugin_gst_element(const char *plugin, void *handle, GstElement *element) {
    pa_assert(plugin);
    pa_assert(element);
    int ret = 0;

    if (pa_streq("audiorate", plugin)) {
        ret = audiorate_set_gst_element(handle, element);
    }

    return ret;
}

const char *get_filter_plugin_dbus_path(const char *plugin) {
    pa_assert(plugin);
    char *path = NULL;

    if (pa_streq("audiorate", plugin)) {
        path = audiorate_get_dbus_path();
    }

    return path;
}

struct pa_dbus_interface_info *get_filter_plugin_dbus_inteface_info(const char *plugin) {
    pa_assert(plugin);
    pa_dbus_interface_info *info = NULL;

    if (pa_streq("audiorate", plugin)) {
        info = audiorate_get_inteface_info();
    }

    return info;
}

int deinit_filter_plugin(const char *plugin, void *handle) {
    pa_assert(plugin);
    int ret = 0;

    if (pa_streq("audiorate", plugin)) {
        ret = audiorate_prop_deinitialize(handle);
    }

    return ret;
}

int create_filter_plugin_gst_pipeline(filter_plugin_handle *handle) {
    pa_assert(handle);

    filter_plugin_handle *u = handle;
    GstElement *src_element = NULL;
    filter_element_info *filter_plugin = NULL;
    GList *list = NULL;
    unsigned sink_out_mode = 0;
    bool skip_to_first = 1;
    pa_modargs *p_ma;

    /* Get the properties and parse  */
    /* convert all "," to spaces and add them to modargs*/
    if (u->filter_properties) {
        char *propname = u->filter_properties;
        while (*propname != '\0') {
            if (*propname == ',')
                *propname = ' ';
            propname++;
        }
        pa_log("propname : %s", u->filter_properties);

        if (!(p_ma = pa_modargs_new(u->filter_properties, valid_properties))) {
            pa_log("Failed to parse module arguments.");
            goto skip;
        }

        if (pa_modargs_get_value_boolean(p_ma, "skip_to_first", &skip_to_first) < 0) {
            pa_log("skip_to_first= expects a boolean value as parameter");
            goto skip;
        }

        pa_log("skip_to_first %d ", skip_to_first);

        if (p_ma)
            pa_modargs_free(p_ma);

    }

    /* Create basic pipeline gstreamer elements */
    u->pipeline = gst_pipeline_new("gstpipeline");
    u->appsrc = gst_element_factory_make("appsrc", "gstsource");

    u->appsink = gst_element_factory_make("appsink", "gstsink");
    if (!u->pipeline || !u->appsrc || !u->appsink) {
        pa_log("%s %d", __func__, __LINE__);
        goto fail;
    }

    /* Create filter elements and get dbus handle */
    for (list = u->filter_plugins; list; list = list->next) {
        filter_plugin = list->data;

        filter_plugin->element = gst_element_factory_make(filter_plugin->name, NULL);
        if (!filter_plugin->element) {
            pa_log("%s %d", __func__, __LINE__);
            goto fail;
        }
    }

    /* Add elements into pipe: appsrc -> filter elements -> appsink */
    gst_bin_add(GST_BIN(u->pipeline), u->appsrc);

    for (list = u->filter_plugins; list; list = list->next) {
        filter_plugin = list->data;

        gst_bin_add(GST_BIN(u->pipeline), filter_plugin->element);
    }
    gst_bin_add(GST_BIN(u->pipeline), u->appsink);

    /* Link elements into pipe: appsrc -> filter elements -> appsink */
    src_element = u->appsrc;

    for (list = u->filter_plugins; list; list = list->next) {
        filter_plugin = list->data;

        if (!gst_element_link(src_element, filter_plugin->element)) {
            pa_log("%s %d", __func__, __LINE__);
            goto fail;
        }

        src_element = filter_plugin->element;
    }

    if (!gst_element_link(src_element, u->appsink)) {
        pa_log("%s %d", __func__, __LINE__);
        goto fail;
    }

    src_element = NULL;

    /* Set filter elements properties */
    for (list = u->filter_plugins; list; list = list->next) {
        filter_plugin = list->data;
        if (pa_streq("audiorate", filter_plugin->name)) {
            g_object_set(filter_plugin->element, "skip-to-first", skip_to_first, NULL);
        }
    }

    return 1;

skip:
    pa_log(" No valid properties set");
    if (p_ma)
        pa_modargs_free(p_ma);

    return 0;

fail:
    /* This will delete all pipeline elements */
    if (u->pipeline) {
        gst_object_unref(GST_OBJECT(u->pipeline));
        u->pipeline = NULL;
    }

    return 0;
}

void set_filter_plugin_callbacks(filter_plugin_cb *cb_handle) {
    pa_assert(cb_handle);

    cb_handle->init = init_filter_plugin;
    cb_handle->set_gst_element = set_filter_plugin_gst_element;
    cb_handle->get_dbus_path = get_filter_plugin_dbus_path;
    cb_handle->get_dbus_inteface_info = get_filter_plugin_dbus_inteface_info;
    cb_handle->deinit = deinit_filter_plugin;
    cb_handle->create_gst_pipeline = create_filter_plugin_gst_pipeline;

    return;
}
