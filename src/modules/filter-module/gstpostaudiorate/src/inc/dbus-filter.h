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

#ifndef SRC_MODULES_DBUS_FILTER_H_
#define SRC_MODULES_DBUS_FILTER_H_

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "pulsecore/config.h"

#include <stdio.h>
#include <gst/gst.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/core-util.h>
#include <dbus/dbus.h>

#include <pulsecore/protocol-dbus.h>

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

int init_filter_plugin(const char *plugin, void **handle);
const char *get_filter_plugin_dbus_path(const char *plugin);
struct pa_dbus_interface_info * get_filter_plugin_dbus_inteface_info(const char *plugin);
int set_filter_plugin_gst_element(const char *plugin, void *handle, GstElement *element);
int deinit_filter_plugin(const char *plugin, void *handle);
void set_filter_plugin_callbacks(filter_plugin_cb *cb_handle);

#endif /* SRC_MODULES_DBUS_FILTER_H_ */
