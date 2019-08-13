/***
    Copyright (c)  {2019} The Linux Foundation. All rights reserved.

    This file is part of PulseAudio.

    Copyright 2010 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>

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

#ifndef SRC_MODULES_FILTER_SINK_H_
#define SRC_MODULES_FILTER_SINK_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Store plugin name, gstreamer factory make element and dbus handle */
typedef struct {
    gchar *name;
    GstElement *element;
    void *dbus_handle;
    pa_dbus_interface_info *dbus_interface_info;
} filter_element_info;

typedef struct {
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *appsink;
    /* GList will store filter plugins related data which can be use in multiple functions
     * for reference as well to take iteration among all filter plugins */
    GList *filter_plugins;

    bool filesink_enable;
    GstElement *wavenc;
    char *filesink_location;
    /* Added to store properties */
    char *filter_properties;
    unsigned out_channels;
} filter_plugin_handle;

/* Callback function structure for filter plugin dbus */
typedef struct {
    int (*init)(const char *plugin, void **handle);
    const char * (*get_dbus_path)(const char *plugin);
    struct pa_dbus_interface_info * (*get_dbus_inteface_info)(const char *plugin);
    int (*set_gst_element)(const char *plugin, void *handle, GstElement *element);
    int (*deinit)(const char *plugin, void *handle);
    int (*create_gst_pipeline)(filter_plugin_handle *handle);
} filter_plugin_cb;

#ifdef __cplusplus
}
#endif

#endif /* SRC_MODULES_FILTER_SINK_H_ */
