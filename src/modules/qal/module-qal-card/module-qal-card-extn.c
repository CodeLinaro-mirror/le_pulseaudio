/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * 02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core-util.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/shared.h>
#include <stdio.h>

#ifdef PAL_USES_CUTILS
#include <cutils/str_parms.h>
#ifdef HAVE_QAL_SOURCETRACK
#include <cutils/properties.h>
#endif
#endif

#include <PalApi.h>
#include <PalDefs.h>

#include "qal-card.h"
#include "qal-source.h"
#include "qal-sink.h"
#include "qal-config-parser.h"

//to be updated in PalDefs.h
#define PAL_PARAM_SET_CUSTOM_VOLUME_INDEX 52
#define PAL_PARAM_SET_CUSTOM_VOIP_ENABLE 53
#define PAL_PARAM_SET_CUSTOM_VOICE_RECOGNITION_ENABLE 54
#define PAL_PARAM_SET_CUSTOM_BARGEIN_ENABLE 55

/*Param key strings to be validated against key received from client*/
#define PAL_PARAM_KEY_VOLUME_INDEX "l_volume_idx"
#define PAL_PARAM_KEY_VOIP "l_voip_enable"
#define PAL_PARAM_KEY_VOICE_RECOGNITION "l_voice_recognition_enable"
#define PAL_PARAM_KEY_BARGEIN "l_bargein_enable"

#define QAL_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qal"
#define QAL_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Qal.Module"

#define OK 0


#ifndef PAL_USES_CUTILS
struct str_parms *str_parms_create_str(const char *_string){return NULL;}
int str_parms_get_str(struct str_parms *str_parms, const char *key,
                      char *out_val, int len){return 0;}
char *str_parms_to_str(struct str_parms *str_parms){return NULL;}
int str_parms_add_str(struct str_parms *str_parms, const char *key,
                      const char *value){return 0;}
struct str_parms *str_parms_create(void){return NULL;}
void str_parms_del(struct str_parms *str_parms, const char *key){return;}
void str_parms_destroy(struct str_parms *str_parms){return;}
#endif

struct pal_module_extn_data {
	char *obj_path;
	pa_dbus_protocol *dbus_protocol;
	pa_card *card;
#ifdef HAVE_QAL_SOURCETRACK
	pa_core *core;
#endif
};

static struct pal_module_extn_data *pal_extn_mdata = NULL;

/* key,value based set params*/
static void pal_module_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pal_module_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
#ifdef HAVE_QAL_SOURCETRACK
static void pal_module_get_doa_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pal_module_set_doa_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
#endif

enum module_method_handler_index {
	METHOD_HANDLER_SET_PARAMETERS,
	METHOD_HANDLER_GET_PARAMETERS,
#ifdef HAVE_QAL_SOURCETRACK
	METHOD_HANDLER_SET_DOA_PARAMETERS,
	METHOD_HANDLER_GET_DOA_PARAMETERS,
	METHOD_HANDLER_MODULE_LAST = METHOD_HANDLER_GET_DOA_PARAMETERS,
#else
	METHOD_HANDLER_MODULE_LAST = METHOD_HANDLER_GET_PARAMETERS,
#endif
	METHOD_HANDLER_MODULE_MAX = METHOD_HANDLER_MODULE_LAST + 1,
};

static pa_dbus_arg_info set_parameters_args[] = {
	{"kv_pairs", "s", "in"},
};

static pa_dbus_arg_info get_parameters_args[] = {
	{"kv_pairs", "s", "in"},
	{"value", "s", "out"}
};

#ifdef HAVE_QAL_SOURCETRACK
static pa_dbus_arg_info set_doa_parameters_args[] = {
	{"payload", "ay", "in"},
};

static pa_dbus_arg_info get_doa_parameters_args[] = {
	{"param", "s", "in"},
	{"payload", "ay", "out"},
};
#endif

static pa_dbus_method_handler module_method_handlers[METHOD_HANDLER_MODULE_MAX] = {
	[METHOD_HANDLER_SET_PARAMETERS] = {
		.method_name = "SetParameters",
		.arguments = set_parameters_args,
		.n_arguments = sizeof(set_parameters_args)/sizeof(pa_dbus_arg_info),
		.receive_cb = pal_module_set_parameters},
	[METHOD_HANDLER_GET_PARAMETERS] = {
		.method_name = "GetParameters",
		.arguments = get_parameters_args,
		.n_arguments = sizeof(get_parameters_args)/sizeof(pa_dbus_arg_info),
		.receive_cb = pal_module_get_parameters},
#ifdef HAVE_QAL_SOURCETRACK
	[METHOD_HANDLER_SET_DOA_PARAMETERS] = {
		.method_name = "SetDoaParameters",
		.arguments = set_doa_parameters_args,
		.n_arguments = sizeof(set_doa_parameters_args)/sizeof(pa_dbus_arg_info),
		.receive_cb = pal_module_set_doa_parameters},
	[METHOD_HANDLER_GET_DOA_PARAMETERS] = {
		.method_name = "GetDoaParameters",
		.arguments = get_doa_parameters_args,
		.n_arguments = sizeof(get_doa_parameters_args)/sizeof(pa_dbus_arg_info),
		.receive_cb = pal_module_get_doa_parameters},
#endif
};

static pa_dbus_interface_info module_interface_info = {
	.name = QAL_DBUS_MODULE_IFACE,
	.method_handlers = module_method_handlers,
	.n_method_handlers = METHOD_HANDLER_MODULE_MAX,
	.property_handlers = NULL,
	.n_property_handlers = 0,
	.get_all_properties_cb = NULL,
};

static void pal_module_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
	int status = 0, err;
	DBusError error;
	const char *kvpairs = NULL;
	pal_param_payload *param_payload = NULL;
	struct str_parms *parms = NULL;
	char c_value[32];

	pa_assert(conn);
	pa_assert(msg);
	pa_assert(userdata);
	dbus_error_init(&error);

	if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &kvpairs, DBUS_TYPE_INVALID)) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
		dbus_error_free(&error);
		return;
	}
	parms = str_parms_create_str(kvpairs);
	if (!parms) {
		pa_log_error("failed to create params\n");
		status = -1;
		goto done;
	}
	err = str_parms_get_str(parms, PAL_PARAM_KEY_VOLUME_INDEX, c_value, sizeof(c_value));
	if (err >= 0) {
		int volume_idx;
		volume_idx = atoi(c_value);
		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
					sizeof(volume_idx));
		if (!param_payload) {
			pa_log_error("calloc failed for size %zu", sizeof(pal_param_payload) +
						sizeof(volume_idx));
			status = -1;
		} else {
			param_payload->payload_size = sizeof(volume_idx);
			memcpy(&param_payload->payload[0], &volume_idx, sizeof(volume_idx));
			status =  pal_set_param(PAL_PARAM_SET_CUSTOM_VOLUME_INDEX, &param_payload,
						sizeof(param_payload->payload_size));
			if (status)
				pa_log_error("Volume set failed with status %x",status);
			free(param_payload);
			param_payload = NULL;
		}
		goto done;
	}
	err = str_parms_get_str(parms, PAL_PARAM_KEY_VOIP, c_value, sizeof(c_value));
	if (err >= 0) {
		bool voip_enable;
		if (strcmp(c_value, "true") == 0)
			voip_enable = true;
		else if (strcmp(c_value, "false") == 0) {
			voip_enable = false;
		}else{
			str_parms_destroy(parms);
			pa_log_error("Invalid param value.");
			status = -1;
			goto done;
		}

		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) + sizeof(bool));
		if (!param_payload) {
			pa_log_error("calloc failed for size %zu", sizeof(pal_param_payload) + sizeof(bool));
			status = -1;
		} else {
			param_payload->payload_size = sizeof(bool);
			memcpy(&param_payload->payload[0], &voip_enable, sizeof(voip_enable));
			status = pal_set_param(PAL_PARAM_SET_CUSTOM_VOIP_ENABLE, &param_payload,
						sizeof(param_payload->payload_size));

			if (status)
				pa_log_error("Voip enable set failed with status %x", status);
			free(param_payload);
			param_payload = NULL;
		}
		goto done;
	}
	err = str_parms_get_str(parms, PAL_PARAM_KEY_VOICE_RECOGNITION, c_value, sizeof(c_value));
	if (err >= 0) {
		bool voice_recog_enable;
		if (strcmp(c_value, "true") == 0)
			voice_recog_enable = true;
		else if (strcmp(c_value, "false") == 0) {
			voice_recog_enable = false;
		}else{
			str_parms_destroy(parms);
			pa_log_error("Invalid param value.");
			status = -1;
			goto done;
		}
		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) + sizeof(bool));
		if (!param_payload) {
			pa_log_error("calloc failed for size %zu", sizeof(pal_param_payload) + sizeof(bool));
			status = -1;
		} else {
			param_payload->payload_size = sizeof(bool);
			memcpy(&param_payload->payload[0], &voice_recog_enable, sizeof(voice_recog_enable));
			status = pal_set_param(PAL_PARAM_SET_CUSTOM_VOICE_RECOGNITION_ENABLE, &param_payload,
						sizeof(param_payload->payload_size));

			if (status)
				pa_log_error("Voice recognition set failed with status %x",status);
			free(param_payload);
			param_payload = NULL;
		}
		goto done;
	}
	err = str_parms_get_str(parms, PAL_PARAM_KEY_BARGEIN, c_value, sizeof(c_value));
	if (err >= 0) {
		bool bargein_enable;
		if (strcmp(c_value, "true") == 0)
			bargein_enable = true;
		else if (strcmp(c_value, "false") == 0) {
			bargein_enable = false;
		}else{
			str_parms_destroy(parms);
			pa_log_error("Invalid param value");
			status = -1;
			goto done;
		}
		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) + sizeof(bool));
		if (!param_payload) {
			pa_log_error("calloc failed for size %zu", sizeof(pal_param_payload) + sizeof(bool));
			status = -1;
		} else {
			param_payload->payload_size = sizeof(bool);
			memcpy(&param_payload->payload[0], &bargein_enable, sizeof(bargein_enable));
			status = pal_set_param(PAL_PARAM_SET_CUSTOM_BARGEIN_ENABLE, &param_payload,
						sizeof(param_payload->payload_size));
			if (status)
				pa_log_error("Bargein enable set failed with status %x", status);
			free(param_payload);
			param_payload = NULL;
		}
		goto done;
	}

done:
	if (OK != status) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param failed");
		dbus_error_free(&error);
		return;
	}
	pa_dbus_send_empty_reply(conn, msg);
}

static void pal_module_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
	int status = 0;
	DBusError error;
	const char *kvpairs = NULL;
	DBusMessage *reply = NULL;
	pal_param_payload *param_payload = NULL;

	pa_assert(conn);
	pa_assert(msg);
	pa_assert(userdata);
	dbus_error_init(&error);

	if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &kvpairs,DBUS_TYPE_INVALID)) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
		dbus_error_free(&error);
		return;
	}

	if (strcmp("device_mute", kvpairs) == 0) {
		pal_device_mute_t *pdev_mute = NULL;
		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
					sizeof(pal_device_mute_t));
		status = pal_get_param(PAL_PARAM_ID_DEVICE_MUTE, (void **)&param_payload,
					(size_t *)sizeof(param_payload->payload_size), NULL);
		if (status) {
			pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param failed");
			dbus_error_free(&error);
			free(param_payload);
			return;
		}
		pdev_mute = (pal_device_mute_t*)param_payload->payload[0];
		pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_STRING, &pdev_mute->mute);
	}
}

#ifdef HAVE_QAL_SOURCETRACK
static void pal_module_set_doa_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
	int status = 0;
	DBusError error;
	DBusMessageIter arg_i, array_i;
	void *array_ptr = NULL;
	int n_elements = 0;

	pal_param_payload *payload = NULL;
	struct pal_module_extn_data *mdata = (struct pal_module_extn_data *)userdata;
	pa_source *s;
	pal_source_data *pal_sdata;
	char prop_value[PROPERTY_VALUE_MAX] = {0};
	pal_param_id_type_t param_id;

	pa_assert(conn);
	pa_assert(msg);
	pa_assert(mdata && mdata->core);

	s = mdata->core->default_source;
	if (!s || !s->userdata) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "No default source active");
		return;
	}

	pal_sdata = ((pa_pal_source_data *)s->userdata)->pal_sdata;
	if (!pal_sdata || !pal_sdata->stream_handle) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Stream not ready");
		return;
	}

	dbus_error_init(&error);

	property_get("ro.vendor.audio.sdk.fluencetype", prop_value, "");

	if (strncmp(prop_value, "fluencepro", sizeof("fluencepro")) == 0) {
		param_id = PAL_PARAM_ID_FLUENCE_SOUNDFOCUS;
		pa_log_debug("Selected PAL Param ID: %d for fluence type: %s",
			(int)param_id, prop_value);
	} else {
		/*
		 * For 'fluencenn' or 'none', no valid param_id exists in this context.
		 * Return specific error to client.
		 */
		pa_dbus_send_error(conn, msg, DBUS_ERROR_NOT_SUPPORTED,
			"Unsupported PAL parameter id");
		dbus_error_free(&error);
		return;
	}

	if (!pa_streq(dbus_message_get_signature(msg), "ay")) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
			"Invalid signature: expected 'ay', got '%s'",
			dbus_message_get_signature(msg));
		dbus_error_free(&error);
		return;
	}

	if (!dbus_message_iter_init(msg, &arg_i)) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "No arguments");
		dbus_error_free(&error);
		return;
	}

	/* Extract the raw fixed array directly */
	dbus_message_iter_recurse(&arg_i, &array_i);
	dbus_message_iter_get_fixed_array(&array_i, &array_ptr, &n_elements);

	if (n_elements != sizeof(struct qcmn_sector_interf_param_t)) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
			"Invalid data size: expected %zu, got %d",
			sizeof(struct qcmn_sector_interf_param_t), n_elements);
		dbus_error_free(&error);
		return;
	}

	payload = (pal_param_payload *)calloc(1,
			sizeof(pal_param_payload) + n_elements);

	if (!payload) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "OOM");
		dbus_error_free(&error);
		return;
	}

	payload->payload_size = n_elements;
	memcpy(payload->payload, array_ptr, n_elements);
	status = pal_stream_set_param(pal_sdata->stream_handle, (uint32_t)param_id, payload);

	if (status != 0) {
		pa_log_error("Failed to set SoundFocus param, status %d", status);
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "PAL set_param failed");
	} else {
		pa_dbus_send_empty_reply(conn, msg);
	}

	free(payload);
	dbus_error_free(&error);
}

static void pal_module_get_doa_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
	int status = 0;
	DBusError error;
	const char *param = NULL;
	pal_param_id_type_t param_id;
	pal_param_payload *payload = NULL;
	DBusMessage *reply = NULL;
	DBusMessageIter arg_i, array_i;
	struct qcmn_source_tracking_interf_param_t *doa = NULL;
	pal_source_data *pal_sdata;
	struct pal_module_extn_data *mdata = (struct pal_module_extn_data *)userdata;
	pa_source *s;
	const void *array_data_ptr;
	char prop_value[PROPERTY_VALUE_MAX] = {0};

	pa_assert(conn);
	pa_assert(msg);
	pa_assert(mdata && mdata->core);

	s = mdata->core->default_source;
	if (!s || !s->userdata) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "No default source active");
		return;
	}

	pal_sdata = ((pa_pal_source_data *)s->userdata)->pal_sdata;

	if (!pal_sdata || !pal_sdata->stream_handle) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Stream not ready");
		return;
	}

	dbus_error_init(&error);
	if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &param, DBUS_TYPE_INVALID)) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
		dbus_error_free(&error);
		return;
	}

	if (!param || !pa_streq("st_direction_of_arrival", param)) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Unsupported parameter");
		dbus_error_free(&error);
		return;
	}

	property_get("ro.vendor.audio.sdk.fluencetype", prop_value, "");

	if (strncmp(prop_value, "fluencepro", sizeof("fluencepro")) == 0) {
		param_id = PAL_PARAM_ID_FLUENCE_SOURCETRACKING;
		pa_log_debug("Selected PAL Param ID: %d for fluence type: %s",
			(int)param_id, prop_value);
	} else {
		/*
		 * For 'fluencenn' or 'none', no valid param_id exists in this context.
		 * Return specific error to client.
		 */
		pa_dbus_send_error(conn, msg, DBUS_ERROR_NOT_SUPPORTED,
			"Unsupported PAL parameter id");
		dbus_error_free(&error);
		return;
	}

	status = pal_stream_get_param(pal_sdata->stream_handle, (uint32_t)param_id, &payload);

	if (status != 0 || payload == NULL) {
		if (payload) free(payload);
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Failed to get param from PAL");
		dbus_error_free(&error);
		return;
	}

	doa = (struct qcmn_source_tracking_interf_param_t *)payload;

	pa_assert_se((reply = dbus_message_new_method_return(msg)));
	dbus_message_iter_init_append(reply, &arg_i);
	dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);

	array_data_ptr = (const void *)doa;
	dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &array_data_ptr,
			sizeof(struct qcmn_source_tracking_interf_param_t));

	dbus_message_iter_close_container(&arg_i, &array_i);
	pa_assert_se(dbus_connection_send(conn, reply, NULL));

	dbus_message_unref(reply);
	free(payload);
}
#endif

int pa_pal_module_extn_init(pa_core *core, pa_card *card)
{
	pa_assert(core);
	pa_assert(card);

	if (pal_extn_mdata) {
		pa_log_info("%s: Module already intialized",__func__);
		return -1;
	}

	pa_log_info("%s", __func__);
	pal_extn_mdata = pa_xnew0(struct pal_module_extn_data, 1);
	pal_extn_mdata->obj_path = pa_sprintf_malloc("%s", QAL_DBUS_OBJECT_PATH_PREFIX);
	pal_extn_mdata->dbus_protocol = pa_dbus_protocol_get(core);
	pal_extn_mdata->card = card;
#ifdef HAVE_QAL_SOURCETRACK
	pal_extn_mdata->core = core;
#endif

	pa_assert_se(pa_dbus_protocol_add_interface(pal_extn_mdata->dbus_protocol,
					pal_extn_mdata->obj_path, &module_interface_info, pal_extn_mdata) >= 0);
	return 0;
}

void pa_pal_module_extn_deinit(void)
{
	pa_assert(pal_extn_mdata);
	pa_assert(pal_extn_mdata->dbus_protocol);
	pa_assert(pal_extn_mdata->obj_path);
	pa_assert_se(pa_dbus_protocol_remove_interface(pal_extn_mdata->dbus_protocol,
				pal_extn_mdata->obj_path, module_interface_info.name) >= 0);
	pa_dbus_protocol_unref(pal_extn_mdata->dbus_protocol);
	pa_xfree(pal_extn_mdata->obj_path);
	pa_xfree(pal_extn_mdata);
	pal_extn_mdata = NULL;
}
