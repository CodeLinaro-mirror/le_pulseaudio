/***
    This file is part of PulseAudio.

    Copyright 2010 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>
    Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.

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
#include "pulsecore_config.h"

#include <dbus/dbus.h>

#include <pulse/gccmacro.h>
#include <pulse/xmalloc.h>
PA_C_DECL_BEGIN
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/log.h>
#include <pulsecore/modargs.h>
#include <pulsecore/module.h>
#include <pulsecore/namereg.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/sink.h>
PA_C_DECL_END

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "group_manager.h"
#include "group_sink_ctrl.h"

#define MOD_EXPORT __attribute__((visibility("default")))

MOD_EXPORT int pa__init(pa_module *m);
MOD_EXPORT void pa__done(pa_module *m);
MOD_EXPORT int pa__get_n_used(pa_module *m);

MOD_EXPORT const char *pa__get_author(void);
MOD_EXPORT const char *pa__get_description(void);
MOD_EXPORT const char *pa__get_usage(void);
MOD_EXPORT const char *pa__get_version(void);
MOD_EXPORT const char *pa__get_deprecated(void);
MOD_EXPORT bool pa__load_once(void);

// Can't use constexpr since we need compile time concatenation
#define FORMAT_PARAM "format"
#define RATE_MAP_PARAM "rate"
#define CHANNELS_PARAM "channels"
#define CHANNEL_MAP_PARAM "channel_map"
#define MASTER_PARAM "master"

PA_MODULE_AUTHOR("Qualcomm Technologies, Inc.");
PA_MODULE_DESCRIPTION(_("Group Manager sink"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
// clang-format off
PA_MODULE_USAGE(
    FORMAT_PARAM "=<sample format> "
    RATE_MAP_PARAM "=<sample rate> "
    CHANNELS_PARAM "=<number of channels> "
    CHANNEL_MAP_PARAM "=<channel map> "
    MASTER_PARAM "=<name of sink to filter> "
);
// clang-format on

#define GROUP_MANAGER_DBUS_OBJECT_PATH_PREFIX "/com/qualcomm/qti/group_manager"
#define GROUP_DBUS_IFACE "com.qualcomm.qti.GroupManager1"

static const char *const valid_modargs[] = {
    FORMAT_PARAM,
    RATE_MAP_PARAM,
    CHANNELS_PARAM,
    CHANNEL_MAP_PARAM,
    MASTER_PARAM,
    nullptr};

static constexpr const char kGroupMultiroomSinkName[] = "multiroom";
static constexpr const char kGroupMultichannelSinkName[] = "multichannel";

namespace std {
template <>
struct default_delete<pa_modargs> {
    void operator()(pa_modargs *p) const {
        if (p) {
            pa_modargs_free(p);
        }
    }
};
}  // namespace std

struct GroupManagerModule {
    std::map<std::string, GroupSinkCtrl *> group_sinks;

    pa_dbus_protocol *dbus_protocol;

    std::unique_ptr<GroupManager> group_manager;
};

static void handle_set_group_peers(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    auto d = reinterpret_cast<GroupManagerModule *>(userdata);
    const char *master_id;
    const char *group;
    char **peers;
    int len;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(d);

    dbus_error_init(&error);
    if ((dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &master_id, DBUS_TYPE_STRING, &group, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &peers, &len, DBUS_TYPE_INVALID)) == 0) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        pa_log("handle_set_group_peers get args error");
        return;
    }
    d->group_manager->setMasterId(master_id);

    pa_log("Received %d group peers:", len);
    std::vector<std::string> peers_vec;
    for (int i = 0; i < len; i++) {
        peers_vec.emplace_back(peers[i]);
        pa_log("Group peer %d: %s", i, peers[i]);
    }

    dbus_free_string_array(peers);

    auto iter = d->group_sinks.find(group);
    if (iter == d->group_sinks.end()) {
        pa_log("Invalid group %s", group);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Invalid group %s", group);
        return;
    }

    iter->second->setPeers(std::move(peers_vec));
    pa_dbus_send_empty_reply(conn, msg);
}

static void handle_update_interfaces(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    auto d = reinterpret_cast<GroupManagerModule *>(userdata);
    std::vector<GroupSinkInterfaces> interfaces;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(d);

    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args)) {
        pa_log("handle_update_interfaces : Message args iter init failed");
        return;
    }
    DBusMessageIter arr;
    dbus_message_iter_recurse(&args, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict;
        dbus_message_iter_recurse(&arr, &dict);
        const char *key;
        const char *value;
        dbus_message_iter_get_basic(&dict, &key);
        dbus_message_iter_next(&dict);
        dbus_message_iter_get_basic(&dict, &value);
        pa_log("handle_update_interfaces : Received interface %s -> %s", key, value);
        interfaces.emplace_back(GroupSinkInterfaces{key, value});
        dbus_message_iter_next(&arr);
    }

    // send interfaces to group sinks
    for (auto &iter : d->group_sinks) {
        iter.second->updateInterfaces(interfaces);
    }
    pa_dbus_send_empty_reply(conn, msg);
}

// TODO(jbing): properties are more adapted for the value here (as of this
// writing)
static pa_dbus_arg_info set_group_peers_args[] = {
    {"master_id", DBUS_TYPE_STRING_AS_STRING, "in"},
    {"group", DBUS_TYPE_STRING_AS_STRING, "in"},
    {"peers", DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING, "in"}};

static pa_dbus_arg_info update_interfaces_args[] = {
    {"interfaces",
        DBUS_TYPE_ARRAY_AS_STRING DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
        DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_STRING_AS_STRING DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
        "in"}};

static pa_dbus_method_handler method_handlers[] = {
    {"SetGroupPeers",
     set_group_peers_args, sizeof(set_group_peers_args) / sizeof(set_group_peers_args[0]),
     handle_set_group_peers},
    {"UpdateInterfaces",
     update_interfaces_args, sizeof(update_interfaces_args) / sizeof(update_interfaces_args[0]),
     handle_update_interfaces}};

static pa_dbus_interface_info interface_info = {
    GROUP_DBUS_IFACE,
    method_handlers,
    sizeof(method_handlers) / sizeof(method_handlers[0]),
    nullptr,
    0,
    nullptr,
    nullptr,
    0};

int pa__init(pa_module *module) {
    std::unique_ptr<pa_modargs> ma;

    pa_assert(module);

    ma = std::unique_ptr<pa_modargs>(pa_modargs_new(module->argument, valid_modargs));
    if (!ma) {
        pa_log("failed to parse module arguments");
        return -1;
    }

    pa_sample_spec sample_spec = module->core->default_sample_spec;
    pa_channel_map channel_map = module->core->default_channel_map;
    if ((pa_modargs_get_sample_spec_and_channel_map(ma.get(), &sample_spec, &channel_map, PA_CHANNEL_MAP_DEFAULT) < 0)) {
        pa_log("Invalid sample specification.");
        return -1;
    }

    const char *master_name = pa_modargs_get_value(ma.get(), MASTER_PARAM, nullptr);
    pa_sink *master = reinterpret_cast<pa_sink *>(pa_namereg_get(module->core, master_name, PA_NAMEREG_SINK));
    if (master == nullptr) {
        pa_log("Master sink ('%s') not found", master_name);
        return -1;
    }

    auto d = new GroupManagerModule;
    module->userdata = d;

    d->dbus_protocol = pa_dbus_protocol_get(module->core);
    pa_assert_se(pa_dbus_protocol_add_interface(d->dbus_protocol, GROUP_MANAGER_DBUS_OBJECT_PATH_PREFIX, &interface_info, d) >= 0);
    pa_assert_se(pa_dbus_protocol_register_extension(d->dbus_protocol, GROUP_DBUS_IFACE) >= 0);

    // Find group sinks
    std::set<GroupSinkCtrl *> groups;
    pa_sink *sink = reinterpret_cast<pa_sink *>(pa_namereg_get(module->core, kGroupMultiroomSinkName, PA_NAMEREG_SINK));
    if (sink == nullptr) {
        pa_log_error("Failed to find multiroom sink: %s", kGroupMultiroomSinkName);
    } else {
        auto group_ctrl = reinterpret_cast<GroupSinkCtrl *>(sink->userdata);
        d->group_sinks.emplace(kGroupMultiroomSinkName, group_ctrl);
        groups.emplace(group_ctrl);
    }

    sink = reinterpret_cast<pa_sink *>(pa_namereg_get(module->core, kGroupMultichannelSinkName, PA_NAMEREG_SINK));
    if (sink == nullptr) {
        pa_log_error("Failed to find multichannel sink: %s", kGroupMultichannelSinkName);
    } else {
        auto group_ctrl = reinterpret_cast<GroupSinkCtrl *>(sink->userdata);
        d->group_sinks.emplace(kGroupMultichannelSinkName, group_ctrl);
        groups.emplace(group_ctrl);
    }

    d->group_manager = GroupManager::create(module, master, std::move(groups),
        sample_spec, channel_map);

    return 0;
}

void pa__done(pa_module *module) {
    auto d = reinterpret_cast<GroupManagerModule *>(module->userdata);
    if (d == nullptr) {
        return;
    }

    pa_assert_se(pa_dbus_protocol_unregister_extension(d->dbus_protocol, GROUP_DBUS_IFACE) >= 0);
    pa_assert_se(pa_dbus_protocol_remove_interface(d->dbus_protocol, GROUP_MANAGER_DBUS_OBJECT_PATH_PREFIX, interface_info.name) >= 0);
    pa_dbus_protocol_unref(d->dbus_protocol);

    delete d;
}
