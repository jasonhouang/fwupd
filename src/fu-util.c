/*
 * Copyright 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuMain"

#include "config.h"

#include <fwupdplugin.h>

#include <fcntl.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#ifdef HAVE_GIO_UNIX
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#endif
#include <locale.h>
#include <stdlib.h>
#include <unistd.h>

#include "fu-console.h"
#include "fu-polkit-agent.h"
#include "fu-util-bios-setting.h"
#include "fu-util-common.h"

#ifdef HAVE_SYSTEMD
#include "fu-systemd.h"
#endif

typedef enum {
	FU_UTIL_OPERATION_UNKNOWN,
	FU_UTIL_OPERATION_UPDATE,
	FU_UTIL_OPERATION_DOWNGRADE,
	FU_UTIL_OPERATION_INSTALL,
	FU_UTIL_OPERATION_LAST
} FuUtilOperation;

struct FuUtil {
	GCancellable *cancellable;
	GMainContext *main_ctx;
	GMainLoop *loop;
	GOptionContext *context;
	FwupdInstallFlags flags;
	FwupdClientDownloadFlags download_flags;
	FwupdClient *client;
	FuConsole *console;
	gboolean no_remote_check;
	gboolean no_metadata_check;
	gboolean no_reboot_check;
	gboolean no_unreported_check;
	gboolean no_safety_check;
	gboolean no_device_prompt;
	gboolean no_emulation_check;
	gboolean no_security_fix;
	gboolean assume_yes;
	gboolean sign;
	gboolean show_all;
	gboolean disable_ssl_strict;
	gboolean as_json;
	/* only valid in update and downgrade */
	FuUtilOperation current_operation;
	FwupdDevice *current_device;
	GPtrArray *post_requests;
	FwupdDeviceFlags completion_flags;
	FwupdDeviceFlags filter_device_include;
	FwupdDeviceFlags filter_device_exclude;
	FwupdReleaseFlags filter_release_include;
	FwupdReleaseFlags filter_release_exclude;
};

static gboolean
fu_util_report_history(FuUtil *self, gchar **values, GError **error);
static FwupdDevice *
fu_util_get_device_by_id(FuUtil *self, const gchar *id, GError **error);

static void
fu_util_client_notify_cb(GObject *object, GParamSpec *pspec, FuUtil *self)
{
	if (self->as_json)
		return;
	fu_console_set_progress(self->console,
				fwupd_client_get_status(self->client),
				fwupd_client_get_percentage(self->client));
}

static void
fu_util_update_device_request_cb(FwupdClient *client, FwupdRequest *request, FuUtil *self)
{
	/* nothing sensible to show */
	if (fwupd_request_get_message(request) == NULL)
		return;

	/* show this now */
	if (fwupd_request_get_kind(request) == FWUPD_REQUEST_KIND_IMMEDIATE) {
		g_autofree gchar *fmt = NULL;
		g_autofree gchar *tmp = NULL;

		/* TRANSLATORS: the user needs to do something, e.g. remove the device */
		fmt = fu_console_color_format(_("Action Required:"), FU_CONSOLE_COLOR_RED);
		tmp = g_strdup_printf("%s %s", fmt, fwupd_request_get_message(request));
		fu_console_set_progress_title(self->console, tmp);
		fu_console_beep(self->console, 5);
	}

	/* save for later */
	if (fwupd_request_get_kind(request) == FWUPD_REQUEST_KIND_POST)
		g_ptr_array_add(self->post_requests, g_object_ref(request));
}

static void
fu_util_update_device_changed_cb(FwupdClient *client, FwupdDevice *device, FuUtil *self)
{
	g_autofree gchar *str = NULL;

	/* action has not been assigned yet */
	if (self->current_operation == FU_UTIL_OPERATION_UNKNOWN)
		return;

	/* allowed to set whenever the device has changed */
	if (fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN))
		self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN;
	if (fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT))
		self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_REBOOT;

	/* same as last time, so ignore */
	if (self->current_device == NULL ||
	    g_strcmp0(fwupd_device_get_composite_id(self->current_device),
		      fwupd_device_get_composite_id(device)) == 0) {
		g_set_object(&self->current_device, device);
		return;
	}

	/* ignore indirect devices that might have changed */
	if (fwupd_device_get_status(device) == FWUPD_STATUS_IDLE ||
	    fwupd_device_get_status(device) == FWUPD_STATUS_UNKNOWN) {
		g_debug("ignoring %s with status %s",
			fwupd_device_get_name(device),
			fwupd_status_to_string(fwupd_device_get_status(device)));
		return;
	}

	/* show message in console */
	if (self->current_operation == FU_UTIL_OPERATION_UPDATE) {
		/* TRANSLATORS: %1 is a device name */
		str = g_strdup_printf(_("Updating %s…"), fwupd_device_get_name(device));
		fu_console_set_progress_title(self->console, str);
	} else if (self->current_operation == FU_UTIL_OPERATION_DOWNGRADE) {
		/* TRANSLATORS: %1 is a device name */
		str = g_strdup_printf(_("Downgrading %s…"), fwupd_device_get_name(device));
		fu_console_set_progress_title(self->console, str);
	} else if (self->current_operation == FU_UTIL_OPERATION_INSTALL) {
		/* TRANSLATORS: %1 is a device name  */
		str = g_strdup_printf(_("Installing on %s…"), fwupd_device_get_name(device));
		fu_console_set_progress_title(self->console, str);
	} else {
		g_warning("no FuUtilOperation set");
	}
	g_set_object(&self->current_device, device);
}

static FwupdDevice *
fu_util_prompt_for_device(FuUtil *self, GPtrArray *devices, GError **error)
{
	FwupdDevice *dev;
	guint idx;
	g_autoptr(GPtrArray) devices_filtered = NULL;

	/* filter results */
	devices_filtered = fwupd_device_array_filter_flags(devices,
							   self->filter_device_include,
							   self->filter_device_exclude,
							   error);
	if (devices_filtered == NULL)
		return NULL;

	/* exactly one */
	if (devices_filtered->len == 1) {
		dev = g_ptr_array_index(devices_filtered, 0);
		if (!self->as_json) {
			fu_console_print(
			    self->console,
			    "%s: %s",
			    /* TRANSLATORS: device has been chosen by the daemon for the user */
			    _("Selected device"),
			    fwupd_device_get_name(dev));
		}
		return g_object_ref(dev);
	}

	/* no questions */
	if (self->no_device_prompt) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "can't prompt for devices");
		return NULL;
	}

	/* TRANSLATORS: this is to abort the interactive prompt */
	fu_console_print(self->console, "0.\t%s", _("Cancel"));
	for (guint i = 0; i < devices_filtered->len; i++) {
		dev = g_ptr_array_index(devices_filtered, i);
		fu_console_print(self->console,
				 "%u.\t%s (%s)",
				 i + 1,
				 fwupd_device_get_id(dev),
				 fwupd_device_get_name(dev));
	}
	/* TRANSLATORS: get interactive prompt */
	idx = fu_console_input_uint(self->console, devices_filtered->len, "%s", _("Choose device"));
	if (idx == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "Request canceled");
		return NULL;
	}
	dev = g_ptr_array_index(devices_filtered, idx - 1);
	return g_object_ref(dev);
}

static gboolean
fu_util_perhaps_show_unreported(FuUtil *self, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_failed = g_ptr_array_new();
	g_autoptr(GPtrArray) devices_success = g_ptr_array_new();
	g_autoptr(GPtrArray) remotes = NULL;
	g_autoptr(GHashTable) remote_id_uri_map = NULL;
	gboolean all_automatic = FALSE;

	/* we don't want to ask anything */
	if (self->no_unreported_check || self->as_json) {
		g_debug("skipping unreported check");
		return TRUE;
	}

	/* get all devices from the history database */
	devices = fwupd_client_get_history(self->client, self->cancellable, &error_local);
	if (devices == NULL) {
		if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO))
			return TRUE;
		g_propagate_error(error, g_steal_pointer(&error_local));
		return FALSE;
	}

	/* create a map of RemoteID to RemoteURI */
	remotes = fwupd_client_get_remotes(self->client, self->cancellable, error);
	if (remotes == NULL)
		return FALSE;
	remote_id_uri_map = g_hash_table_new(g_str_hash, g_str_equal);
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index(remotes, i);
		gboolean remote_automatic;
		if (fwupd_remote_get_id(remote) == NULL)
			continue;
		if (fwupd_remote_get_report_uri(remote) == NULL)
			continue;
		g_debug("adding %s for %s",
			fwupd_remote_get_report_uri(remote),
			fwupd_remote_get_id(remote));
		g_hash_table_insert(remote_id_uri_map,
				    (gpointer)fwupd_remote_get_id(remote),
				    (gpointer)fwupd_remote_get_report_uri(remote));
		remote_automatic =
		    fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_AUTOMATIC_REPORTS);
		g_debug("%s is %d", fwupd_remote_get_title(remote), remote_automatic);
		if (remote_automatic && !all_automatic)
			all_automatic = TRUE;
		if (!remote_automatic && all_automatic) {
			all_automatic = FALSE;
			break;
		}
	}

	/* check that they can be reported */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		FwupdRelease *rel = fwupd_device_get_release_default(dev);
		const gchar *remote_id;
		const gchar *remote_uri;

		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		if (fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_REPORTED))
			continue;
		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_SUPPORTED))
			continue;

		/* find the RemoteURI to use for the device */
		remote_id = fwupd_release_get_remote_id(rel);
		if (remote_id == NULL) {
			g_debug("%s has no RemoteID", fwupd_device_get_id(dev));
			continue;
		}
		remote_uri = g_hash_table_lookup(remote_id_uri_map, remote_id);
		if (remote_uri == NULL) {
			g_debug("%s has no RemoteURI", remote_id);
			continue;
		}

		/* only send success and failure */
		if (fwupd_device_get_update_state(dev) == FWUPD_UPDATE_STATE_FAILED) {
			g_ptr_array_add(devices_failed, dev);
		} else if (fwupd_device_get_update_state(dev) == FWUPD_UPDATE_STATE_SUCCESS) {
			g_ptr_array_add(devices_success, dev);
		} else {
			g_debug("ignoring %s with UpdateState %s",
				fwupd_device_get_id(dev),
				fwupd_update_state_to_string(fwupd_device_get_update_state(dev)));
		}
	}

	/* nothing to do */
	if (devices_failed->len == 0 && devices_success->len == 0) {
		g_debug("no unreported devices");
		return TRUE;
	}

	g_debug("All automatic: %d", all_automatic);
	/* show the success and failures */
	if (!self->assume_yes && !all_automatic) {
		/* delimit */
		fu_console_line(self->console, 48);

		/* failures */
		if (devices_failed->len > 0) {
			fu_console_print_literal(self->console,
						 /* TRANSLATORS: a list of failed updates */
						 _("Devices that were not updated correctly:"));
			for (guint i = 0; i < devices_failed->len; i++) {
				FwupdDevice *dev = g_ptr_array_index(devices_failed, i);
				FwupdRelease *rel = fwupd_device_get_release_default(dev);
				fu_console_print(self->console,
						 " • %s (%s → %s)",
						 fwupd_device_get_name(dev),
						 fwupd_device_get_version(dev),
						 fwupd_release_get_version(rel));
			}
		}

		/* success */
		if (devices_success->len > 0) {
			fu_console_print_literal(self->console,
						 /* TRANSLATORS: a list of successful updates */
						 _("Devices that have been updated successfully:"));
			for (guint i = 0; i < devices_success->len; i++) {
				FwupdDevice *dev = g_ptr_array_index(devices_success, i);
				FwupdRelease *rel = fwupd_device_get_release_default(dev);
				fu_console_print(self->console,
						 " • %s (%s → %s)",
						 fwupd_device_get_name(dev),
						 fwupd_device_get_version(dev),
						 fwupd_release_get_version(rel));
			}
		}

		/* ask for permission */
		fu_console_print_literal(self->console,
					 /* TRANSLATORS: explain why we want to upload */
					 _("Uploading firmware reports helps hardware vendors "
					   "to quickly identify failing and successful updates "
					   "on real devices."));
		if (!fu_console_input_bool(self->console,
					   TRUE,
					   "%s (%s)",
					   /* TRANSLATORS: ask the user to upload */
					   _("Review and upload report now?"),
					   /* TRANSLATORS: metadata is downloaded */
					   _("Requires internet connection"))) {
			if (fu_console_input_bool(self->console,
						  FALSE,
						  "%s",
						  /* TRANSLATORS: offer to disable this nag */
						  _("Do you want to disable this feature "
						    "for future updates?"))) {
				for (guint i = 0; i < remotes->len; i++) {
					FwupdRemote *remote = g_ptr_array_index(remotes, i);
					const gchar *remote_id = fwupd_remote_get_id(remote);
					if (fwupd_remote_get_report_uri(remote) == NULL)
						continue;
					if (!fwupd_client_modify_remote(self->client,
									remote_id,
									"ReportURI",
									"",
									self->cancellable,
									error))
						return FALSE;
				}
			}
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOTHING_TO_DO,
					    "Declined upload");
			return FALSE;
		}
	}

	/* upload */
	if (!fu_util_report_history(self, NULL, error))
		return FALSE;

	/* offer to make automatic */
	if (!self->assume_yes && !all_automatic) {
		if (fu_console_input_bool(self->console,
					  FALSE,
					  "%s",
					  /* TRANSLATORS: offer to stop asking the question */
					  _("Do you want to upload reports automatically for "
					    "future updates?"))) {
			for (guint i = 0; i < remotes->len; i++) {
				FwupdRemote *remote = g_ptr_array_index(remotes, i);
				const gchar *remote_id = fwupd_remote_get_id(remote);
				if (fwupd_remote_get_report_uri(remote) == NULL)
					continue;
				if (fwupd_remote_has_flag(remote,
							  FWUPD_REMOTE_FLAG_AUTOMATIC_REPORTS))
					continue;
				if (!fwupd_client_modify_remote(self->client,
								remote_id,
								"AutomaticReports",
								"true",
								self->cancellable,
								error))
					return FALSE;
			}
		}
	}

	/* success */
	return TRUE;
}

static void
fu_util_build_device_tree(FuUtil *self, FuUtilNode *root, GPtrArray *devs, FwupdDevice *dev)
{
	for (guint i = 0; i < devs->len; i++) {
		FwupdDevice *dev_tmp = g_ptr_array_index(devs, i);
		if (!fwupd_device_match_flags(dev_tmp,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		if (!self->show_all && !fu_util_is_interesting_device(devs, dev_tmp))
			continue;
		if (fwupd_device_get_parent(dev_tmp) == dev) {
			FuUtilNode *child = g_node_append_data(root, g_object_ref(dev_tmp));
			fu_util_build_device_tree(self, child, devs, dev_tmp);
		}
	}
}

static gboolean
fu_util_get_releases_as_json(FuUtil *self, GPtrArray *rels, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "Releases");
	json_builder_begin_array(builder);
	for (guint i = 0; i < rels->len; i++) {
		FwupdRelease *rel = g_ptr_array_index(rels, i);
		if (!fwupd_release_match_flags(rel,
					       self->filter_release_include,
					       self->filter_release_exclude))
			continue;
		json_builder_begin_object(builder);
		fwupd_codec_to_json(FWUPD_CODEC(rel), builder, FWUPD_CODEC_FLAG_NONE);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return fu_util_print_builder(self->console, builder, error);
}

static gboolean
fu_util_get_devices_as_json(FuUtil *self, GPtrArray *devs, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "Devices");
	json_builder_begin_array(builder);
	for (guint i = 0; i < devs->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devs, i);
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;

		/* filter */
		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;

		/* add all releases that could be applied */
		rels = fwupd_client_get_releases(self->client,
						 fwupd_device_get_id(dev),
						 self->cancellable,
						 &error_local);
		if (rels == NULL) {
			g_debug("not adding releases to device: %s", error_local->message);
		} else {
			for (guint j = 0; j < rels->len; j++) {
				FwupdRelease *rel = g_ptr_array_index(rels, j);
				if (!fwupd_release_match_flags(rel,
							       self->filter_release_include,
							       self->filter_release_exclude))
					continue;
				fwupd_device_add_release(dev, rel);
			}
		}

		/* add to builder */
		json_builder_begin_object(builder);
		fwupd_codec_to_json(FWUPD_CODEC(dev), builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return fu_util_print_builder(self->console, builder, error);
}

static gboolean
fu_util_check_reboot_needed(FuUtil *self, gchar **values, GError **error)
{
	/* handle both forms */
	if (g_strv_length(values) == 0) {
		g_autoptr(GPtrArray) devices =
		    fwupd_client_get_devices(self->client, self->cancellable, error);
		if (devices == NULL)
			return FALSE;
		for (guint i = 0; i < devices->len; i++) {
			FwupdDevice *device = g_ptr_array_index(devices, i);

			if (fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT))
				self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_REBOOT;
			if (fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN))
				self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN;
		}
	} else {
		for (guint idx = 0; idx < g_strv_length(values); idx++) {
			FwupdDevice *device = fu_util_get_device_by_id(self, values[idx], error);

			if (device == NULL) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_ARGS,
					    "'%s' is not a valid GUID nor DEVICE-ID",
					    values[idx]);
				return FALSE;
			}
			if (fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT))
				self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_REBOOT;
			if (fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN))
				self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN;
		}
	}

	if (!(self->completion_flags &
	      (FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN | FWUPD_DEVICE_FLAG_NEEDS_REBOOT))) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: no rebooting needed */
				    _("No reboot is necessary"));
		return FALSE;
	}

	if (self->as_json)
		return TRUE;

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_get_devices(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuUtilNode) root = g_node_new(NULL);
	g_autoptr(GPtrArray) devs = NULL;

	/* get results from daemon */
	if (g_strv_length(values) > 0) {
		devs = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
		for (guint i = 0; values[i] != NULL; i++) {
			FwupdDevice *device = fu_util_get_device_by_id(self, values[i], error);
			if (device == NULL)
				return FALSE;
			g_ptr_array_add(devs, device);
		}
	} else {
		devs = fwupd_client_get_devices(self->client, self->cancellable, error);
		if (devs == NULL)
			return FALSE;
	}

	/* not for human consumption */
	if (self->as_json)
		return fu_util_get_devices_as_json(self, devs, error);

	/* print */
	if (devs->len > 0)
		fu_util_build_device_tree(self, root, devs, NULL);
	if (g_node_n_children(root) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: nothing attached that can be upgraded */
				    _("No hardware detected with firmware update capability"));
		return FALSE;
	}
	fu_util_print_node(self->console, self->client, root);

	/* nag? */
	if (!fu_util_perhaps_show_unreported(self, error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_util_get_plugins(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) plugins = NULL;

	/* get results from daemon */
	plugins = fwupd_client_get_plugins(self->client, self->cancellable, error);
	g_ptr_array_sort(plugins, (GCompareFunc)fu_util_plugin_name_sort_cb);
	if (plugins == NULL)
		return FALSE;
	if (self->as_json) {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		json_builder_begin_object(builder);
		fwupd_codec_array_to_json(plugins, "Plugins", builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
		return fu_util_print_builder(self->console, builder, error);
	}

	/* print */
	for (guint i = 0; i < plugins->len; i++) {
		FuPlugin *plugin = g_ptr_array_index(plugins, i);
		g_autofree gchar *str = fu_util_plugin_to_string(FWUPD_PLUGIN(plugin), 0);
		fu_console_print_literal(self->console, str);
	}

	/* success */
	return TRUE;
}

static gchar *
fu_util_download_if_required(FuUtil *self, const gchar *perhapsfn, GError **error)
{
	g_autofree gchar *filename = NULL;
	g_autoptr(GBytes) blob = NULL;

	/* a local file */
	if (g_file_test(perhapsfn, G_FILE_TEST_EXISTS))
		return g_strdup(perhapsfn);
	if (!fu_util_is_url(perhapsfn))
		return g_strdup(perhapsfn);

	/* download the firmware to a cachedir */
	filename = fu_util_get_user_cache_path(perhapsfn);
	if (g_file_test(filename, G_FILE_TEST_EXISTS))
		return g_steal_pointer(&filename);
	if (!fu_path_mkdir_parent(filename, error))
		return NULL;
	blob = fwupd_client_download_bytes(self->client,
					   perhapsfn,
					   self->download_flags,
					   self->cancellable,
					   error);
	if (blob == NULL)
		return NULL;

	/* save file to cache */
	if (!fu_bytes_set_contents(filename, blob, error))
		return NULL;
	return g_steal_pointer(&filename);
}

static void
fu_util_display_current_message(FuUtil *self)
{
	if (self->as_json)
		return;

	/* TRANSLATORS: success message */
	fu_console_print_literal(self->console, _("Successfully installed firmware"));

	/* print all POST requests */
	for (guint i = 0; i < self->post_requests->len; i++) {
		FwupdRequest *request = g_ptr_array_index(self->post_requests, i);
		fu_console_print_literal(self->console, fu_util_request_get_message(request));
	}
}

typedef struct {
	guint nr_success;
	guint nr_missing;
	guint nr_skipped;
	JsonBuilder *builder;
	const gchar *name;
	gboolean use_emulation;
	GHashTable *report_metadata;
} FuUtilDeviceTestHelper;

static void
fu_util_device_test_helper_free(FuUtilDeviceTestHelper *helper)
{
	if (helper->report_metadata != NULL)
		g_hash_table_unref(helper->report_metadata);
	if (helper->builder != NULL)
		g_object_unref(helper->builder);
	g_free(helper);
}

static FuUtilDeviceTestHelper *
fu_util_device_test_helper_new(void)
{
	FuUtilDeviceTestHelper *helper = g_new0(FuUtilDeviceTestHelper, 1);
	helper->builder = json_builder_new();
	return helper;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuUtilDeviceTestHelper, fu_util_device_test_helper_free)

static GPtrArray *
fu_util_filter_devices(FuUtil *self, GPtrArray *devices, GError **error)
{
	g_autoptr(GPtrArray) devices_filtered =
	    g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		g_ptr_array_add(devices_filtered, g_object_ref(dev));
	}
	if (devices_filtered->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "failed to find any devices");
		return NULL;
	}
	return g_steal_pointer(&devices_filtered);
}

static gboolean
fu_util_device_test_component(FuUtil *self,
			      FuUtilDeviceTestHelper *helper,
			      JsonObject *json_obj,
			      GError **error)
{
	JsonArray *json_array;
	const gchar *name = "component";
	const gchar *protocol = NULL;
	g_autoptr(FwupdDevice) device = NULL;

	/* some elements are optional */
	if (json_object_has_member(json_obj, "name")) {
		name = json_object_get_string_member(json_obj, "name");
		json_builder_set_member_name(helper->builder, "name");
		json_builder_add_string_value(helper->builder, name);
	}
	if (json_object_has_member(json_obj, "protocol")) {
		protocol = json_object_get_string_member(json_obj, "protocol");
		json_builder_set_member_name(helper->builder, "protocol");
		json_builder_add_string_value(helper->builder, protocol);
	}

	/* find the device with any of the matching GUIDs */
	if (!json_object_has_member(json_obj, "guids")) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "JSON invalid as has no 'guids'");
		return FALSE;
	}
	json_array = json_object_get_array_member(json_obj, "guids");
	json_builder_set_member_name(helper->builder, "guids");
	json_builder_begin_array(helper->builder);
	for (guint i = 0; i < json_array_get_length(json_array); i++) {
		JsonNode *json_node = json_array_get_element(json_array, i);
		FwupdDevice *device_tmp;
		const gchar *guid = json_node_get_string(json_node);
		g_autoptr(GPtrArray) devices = NULL;
		g_autoptr(GPtrArray) devices_filtered = NULL;

		g_debug("looking for guid %s", guid);
		devices =
		    fwupd_client_get_devices_by_guid(self->client, guid, self->cancellable, NULL);
		if (devices == NULL)
			continue;
		devices_filtered = fu_util_filter_devices(self, devices, NULL);
		if (devices_filtered == NULL)
			continue;
		if (devices_filtered->len > 1) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "multiple devices with GUID %s",
				    guid);
			return FALSE;
		}
		device_tmp = g_ptr_array_index(devices_filtered, 0);
		if (protocol != NULL && !fwupd_device_has_protocol(device_tmp, protocol))
			continue;
		device = g_object_ref(device_tmp);
		json_builder_add_string_value(helper->builder, guid);
		break;
	}
	json_builder_end_array(helper->builder);
	if (device == NULL) {
		if (!self->as_json) {
			g_autofree gchar *msg = NULL;
			msg = fu_console_color_format(
			    /* TRANSLATORS: this is for the device tests */
			    _("Did not find any devices with matching GUIDs"),
			    FU_CONSOLE_COLOR_RED);
			fu_console_print(self->console, "%s: %s", name, msg);
		}
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no devices found");
		return FALSE;
	}

	/* verify the version matches what we expected */
	if (json_object_has_member(json_obj, "version")) {
		const gchar *version = json_object_get_string_member(json_obj, "version");
		if (g_strcmp0(version, fwupd_device_get_version(device)) != 0) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "version did not match: got %s, expected %s",
				    fwupd_device_get_version(device),
				    version);
			return FALSE;
		}
	}

	/* verify the bootloader version matches what we expected */
	if (json_object_has_member(json_obj, "version-bootloader")) {
		const gchar *version =
		    json_object_get_string_member(json_obj, "version-bootloader");
		if (g_strcmp0(version, fwupd_device_get_version_bootloader(device)) != 0) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "bootloader version did not match: got %s, expected %s",
				    fwupd_device_get_version_bootloader(device),
				    version);
			return FALSE;
		}
	}

	/* verify the branch matches what we expected */
	if (json_object_has_member(json_obj, "branch")) {
		const gchar *version = json_object_get_string_member(json_obj, "branch");
		if (g_strcmp0(version, fwupd_device_get_branch(device)) != 0) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "branch did not match: got %s, expected %s",
				    fwupd_device_get_branch(device),
				    version);
			return FALSE;
		}
	}

	/* success */
	if (!self->as_json) {
		g_autofree gchar *msg = NULL;
		/* TRANSLATORS: this is for the device tests */
		msg = fu_console_color_format(_("OK!"), FU_CONSOLE_COLOR_GREEN);
		if (g_strcmp0(name, "component") != 0) {
			fu_console_print(self->console, "%s [%s]: %s", helper->name, name, msg);
		} else {
			fu_console_print(self->console, "%s: %s", helper->name, msg);
		}
	}
	helper->nr_success++;
	return TRUE;
}

static gboolean
fu_util_device_test_remove_emulated_devices(FuUtil *self, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;

	devices = fwupd_client_get_devices(self->client, self->cancellable, error);
	if (devices == NULL)
		return FALSE;
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *device = g_ptr_array_index(devices, i);
		g_autoptr(GError) error_local = NULL;
		if (!fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_EMULATED))
			continue;
		if (!fwupd_client_modify_device(self->client,
						fwupd_device_get_id(device),
						"Flags",
						"~emulated",
						self->cancellable,
						&error_local)) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND)) {
				g_debug("ignoring: %s", error_local->message);
				continue;
			}
			g_propagate_prefixed_error(error,
						   g_steal_pointer(&error_local),
						   "failed to modify device: ");
			return FALSE;
		}
	}

	/* success */
	return TRUE;
}

static gchar *
fu_util_maybe_expand_basename(FuUtil *self, const gchar *maybe_basename, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;

	if (g_str_has_prefix(maybe_basename, "https://"))
		return g_strdup(maybe_basename);
	if (g_str_has_prefix(maybe_basename, "/"))
		return g_strdup(maybe_basename);

	/* find LVFS remote */
	remote = fwupd_client_get_remote_by_id(self->client, "lvfs", self->cancellable, error);
	if (remote == NULL)
		return NULL;
	if (fwupd_remote_get_firmware_base_uri(remote)) {
		g_debug("no FirmwareBaseURI set in lvfs.conf, using default");
		return g_strdup_printf("https://fwupd.org/downloads/%s", maybe_basename);
	}
	return g_strdup_printf("%s/%s", fwupd_remote_get_firmware_base_uri(remote), maybe_basename);
}

static gboolean
fu_util_device_test_step(FuUtil *self,
			 FuUtilDeviceTestHelper *helper,
			 JsonObject *json_obj,
			 GError **error)
{
	JsonArray *json_array;

	/* send this data to the daemon */
	if (helper->use_emulation) {
		g_autofree gchar *emulation_filename = NULL;
		g_autofree gchar *emulation_url = NULL;

		/* just ignore anything without emulation data */
		if (json_object_has_member(json_obj, "emulation-url")) {
			const gchar *url_tmp =
			    json_object_get_string_member(json_obj, "emulation-url");

			emulation_url = fu_util_maybe_expand_basename(self, url_tmp, error);
			if (emulation_url == NULL)
				return FALSE;
			emulation_filename =
			    fu_util_download_if_required(self, emulation_url, error);
			if (emulation_filename == NULL) {
				g_prefix_error(error, "failed to download %s: ", emulation_url);
				return FALSE;
			}
		} else if (json_object_has_member(json_obj, "emulation-file")) {
			emulation_filename =
			    g_strdup(json_object_get_string_member(json_obj, "emulation-file"));
		} else {
			return TRUE;
		}

		/* log */
		if (emulation_url != NULL) {
			json_builder_set_member_name(helper->builder, "emulation-url");
			json_builder_add_string_value(helper->builder, emulation_url);
		}
		json_builder_set_member_name(helper->builder, "emulation-file");
		json_builder_add_string_value(helper->builder, emulation_filename);
		if (!fwupd_client_emulation_load(self->client,
						 emulation_filename,
						 self->cancellable,
						 error)) {
			g_prefix_error(error, "failed to load %s: ", emulation_filename);
			return FALSE;
		}
	}

	/* download file if required */
	if (json_object_has_member(json_obj, "url")) {
		const gchar *url_tmp = json_object_get_string_member(json_obj, "url");
		g_autofree gchar *filename = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GError) error_local = NULL;

		url = fu_util_maybe_expand_basename(self, url_tmp, error);
		if (url == NULL)
			return FALSE;
		filename = fu_util_download_if_required(self, url, error);
		if (filename == NULL) {
			g_prefix_error(error, "failed to download %s: ", url);
			return FALSE;
		}

		/* log */
		json_builder_set_member_name(helper->builder, "url");
		json_builder_add_string_value(helper->builder, url);

		/* install file */
		self->flags |= FWUPD_INSTALL_FLAG_ALLOW_OLDER;
		self->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
		if (!fwupd_client_install(self->client,
					  FWUPD_DEVICE_ID_ANY,
					  filename,
					  self->flags,
					  self->cancellable,
					  &error_local)) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND)) {
				if (self->as_json) {
					json_builder_set_member_name(helper->builder, "info");
					json_builder_add_string_value(helper->builder,
								      error_local->message);
				} else {
					g_autofree gchar *msg = NULL;
					msg = fu_console_color_format(error_local->message,
								      FU_CONSOLE_COLOR_YELLOW);
					fu_console_print(self->console,
							 "%s: %s",
							 helper->name,
							 msg);
				}
				helper->nr_missing++;
				return TRUE;
			}
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}
	}

	/* process each step */
	if (!json_object_has_member(json_obj, "components")) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "JSON invalid as has no 'components'");
		return FALSE;
	}
	json_array = json_object_get_array_member(json_obj, "components");
	for (guint i = 0; i < json_array_get_length(json_array); i++) {
		JsonNode *json_node = json_array_get_element(json_array, i);
		JsonObject *json_obj_tmp = json_node_get_object(json_node);
		if (!fu_util_device_test_component(self, helper, json_obj_tmp, error))
			return FALSE;
	}

	/* remove emulated devices */
	if (helper->use_emulation) {
		if (!fu_util_device_test_remove_emulated_devices(self, error)) {
			g_prefix_error(error, "failed to remove emulated devices: ");
			return FALSE;
		}
	}

	/* success */
	json_builder_set_member_name(helper->builder, "success");
	json_builder_add_boolean_value(helper->builder, TRUE);
	return TRUE;
}

static gboolean
fu_util_device_test_filename(FuUtil *self,
			     FuUtilDeviceTestHelper *helper,
			     const gchar *filename,
			     GError **error)
{
	JsonNode *json_root;
	JsonNode *json_steps;
	JsonObject *json_obj;
	guint repeat = 1;
	g_autoptr(JsonParser) parser = json_parser_new();

	/* log */
	json_builder_set_member_name(helper->builder, "filename");
	json_builder_add_string_value(helper->builder, filename);

	/* parse JSON */
	if (!json_parser_load_from_file(parser, filename, error)) {
		g_prefix_error(error, "test not in JSON format: ");
		return FALSE;
	}
	json_root = json_parser_get_root(parser);
	if (json_root == NULL || !JSON_NODE_HOLDS_OBJECT(json_root)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "JSON invalid as has no root");
		return FALSE;
	}
	json_obj = json_node_get_object(json_root);
	if (!json_object_has_member(json_obj, "steps")) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "JSON invalid as has no 'steps'");
		return FALSE;
	}
	json_steps = json_object_get_member(json_obj, "steps");
	if (!JSON_NODE_HOLDS_ARRAY(json_steps)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "JSON invalid as has 'steps' is not an array");
		return FALSE;
	}

	/* some elements are optional */
	if (json_object_has_member(json_obj, "name")) {
		helper->name = json_object_get_string_member(json_obj, "name");
		json_builder_set_member_name(helper->builder, "name");
		json_builder_add_string_value(helper->builder, helper->name);
	}
	if (json_object_has_member(json_obj, "interactive")) {
		gboolean interactive = json_object_get_boolean_member(json_obj, "interactive");
		json_builder_set_member_name(helper->builder, "interactive");
		json_builder_add_boolean_value(helper->builder, interactive);
	}
	if (json_object_has_member(json_obj, "cpu-architectures")) {
		JsonArray *json_array = json_object_get_array_member(json_obj, "cpu-architectures");
		gboolean matched = FALSE;
		const gchar *arch = g_hash_table_lookup(helper->report_metadata, "CpuArchitecture");
		for (guint i = 0; i < json_array_get_length(json_array); i++) {
			const gchar *arch_tmp = json_array_get_string_element(json_array, i);
			if (g_strcmp0(arch, arch_tmp) == 0) {
				matched = TRUE;
				break;
			}
		}
		if (!matched) {
			helper->nr_skipped++;
			return TRUE;
		}
	}
	if (json_object_has_member(json_obj, "platform-architectures")) {
		JsonArray *json_array =
		    json_object_get_array_member(json_obj, "platform-architectures");
		gboolean matched = FALSE;
		const gchar *arch =
		    g_hash_table_lookup(helper->report_metadata, "PlatformArchitecture");
		for (guint i = 0; i < json_array_get_length(json_array); i++) {
			const gchar *arch_tmp = json_array_get_string_element(json_array, i);
			if (g_strcmp0(arch, arch_tmp) == 0) {
				matched = TRUE;
				break;
			}
		}
		if (!matched) {
			helper->nr_skipped++;
			return TRUE;
		}
	}

	/* process each step */
	if (json_object_has_member(json_obj, "repeat")) {
		repeat = json_object_get_int_member(json_obj, "repeat");
		json_builder_set_member_name(helper->builder, "repeat");
		json_builder_add_int_value(helper->builder, repeat);
	}
	json_builder_set_member_name(helper->builder, "steps");
	json_builder_begin_array(helper->builder);
	for (guint j = 0; j < repeat; j++) {
		JsonArray *json_array = json_node_get_array(json_steps);
		for (guint i = 0; i < json_array_get_length(json_array); i++) {
			JsonNode *json_node = json_array_get_element(json_array, i);
			json_obj = json_node_get_object(json_node);
			json_builder_begin_object(helper->builder);
			if (!fu_util_device_test_step(self, helper, json_obj, error))
				return FALSE;
			json_builder_end_object(helper->builder);
		}
	}
	json_builder_end_array(helper->builder);

	/* success */
	return TRUE;
}

typedef struct {
	FuUtil *self;
	gchar *inhibit_id;
} FuUtilInhibitHelper;

static void
fu_util_inhibit_helper_free(FuUtilInhibitHelper *helper)
{
	g_free(helper->inhibit_id);
	g_free(helper);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuUtilInhibitHelper, fu_util_inhibit_helper_free)

static gboolean
fu_util_inhibit_timeout_cb(FuUtilInhibitHelper *helper)
{
	FuUtil *self = helper->self;
	g_autoptr(GError) error_local = NULL;

	if (!fwupd_client_uninhibit(self->client,
				    helper->inhibit_id,
				    self->cancellable,
				    &error_local)) {
		g_warning("failed to auto-uninhibit: %s", error_local->message);
	}
	g_main_loop_quit(self->loop);
	return G_SOURCE_REMOVE;
}

static gboolean
fu_util_inhibit(FuUtil *self, gchar **values, GError **error)
{
	const gchar *reason = "not set";
	guint64 timeout_ms = 0;
	g_autoptr(FuUtilInhibitHelper) helper = g_new0(FuUtilInhibitHelper, 1);
	g_autoptr(GString) str = g_string_new(NULL);

	if (g_strv_length(values) > 0)
		reason = values[0];
	if (g_strv_length(values) > 1) {
		if (!fu_strtoull(values[1],
				 &timeout_ms,
				 0,
				 G_MAXUINT32,
				 FU_INTEGER_BASE_AUTO,
				 error))
			return FALSE;
	}

	/* inhibit then wait */
	helper->self = self;
	helper->inhibit_id = fwupd_client_inhibit(self->client, reason, self->cancellable, error);
	if (helper->inhibit_id == NULL)
		return FALSE;
	if (timeout_ms > 0) {
		g_autoptr(GSource) source = g_timeout_source_new(timeout_ms);
		g_source_set_callback(source,
				      (GSourceFunc)fu_util_inhibit_timeout_cb,
				      helper,
				      NULL);
		g_source_attach(source, self->main_ctx);
	}

	/* TRANSLATORS: the inhibit ID is a short string like dbus-123456 */
	g_string_append_printf(str, _("Inhibit ID is %s."), helper->inhibit_id);
	g_string_append(str, "\n");
	if (timeout_ms > 0) {
		g_string_append_printf(str,
				       /* TRANSLATORS: we can auto-uninhibit after a timeout */
				       _("Automatically uninhibiting in %ums…"),
				       (guint)timeout_ms);
		g_string_append(str, "\n");
	}
	/* TRANSLATORS: CTRL^C [holding control, and then pressing C] will exit the program */
	g_string_append(str, _("Use CTRL^C to cancel."));
	/* TRANSLATORS: this CLI tool is now preventing system updates */
	fu_console_box(self->console, _("System Update Inhibited"), str->str, 80);
	g_main_loop_run(self->loop);
	return TRUE;
}

static gboolean
fu_util_uninhibit(FuUtil *self, gchar **values, GError **error)
{
	/* one argument required */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments, expected INHIBIT-ID");
		return FALSE;
	}

	/* just uninhibit with the token */
	return fwupd_client_uninhibit(self->client, values[0], self->cancellable, error);
}

typedef struct {
	FuUtil *self;
	const gchar *value;
	FwupdDevice *device; /* no-ref */
} FuUtilWaitHelper;

static void
fu_util_device_wait_added_cb(FwupdClient *client, FwupdDevice *device, FuUtilWaitHelper *helper)
{
	FuUtil *self = helper->self;
	if (g_strcmp0(fwupd_device_get_id(device), helper->value) == 0 ||
	    fwupd_device_has_guid(device, helper->value)) {
		helper->device = device;
		g_main_loop_quit(self->loop);
		return;
	}
}

static gboolean
fu_util_device_wait_timeout_cb(gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;
	g_main_loop_quit(self->loop);
	return G_SOURCE_REMOVE;
}

static gboolean
fu_util_device_wait(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) device = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GSource) source = g_timeout_source_new_seconds(30);
	g_autoptr(GTimer) timer = g_timer_new();
	FuUtilWaitHelper helper = {.self = self, .value = values[0]};

	/* one argument required */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments, expected GUID|DEVICE-ID");
		return FALSE;
	}

	/* check if the device already exists */
	device = fwupd_client_get_device_by_id(self->client, helper.value, NULL, NULL);
	if (device != NULL) {
		/* TRANSLATORS: the device is already connected */
		fu_console_print_literal(self->console, _("Device already exists"));
		return TRUE;
	}
	devices = fwupd_client_get_devices_by_guid(self->client, helper.value, NULL, NULL);
	if (devices != NULL) {
		/* TRANSLATORS: the device is already connected */
		fu_console_print_literal(self->console, _("Device already exists"));
		return TRUE;
	}

	/* wait for device to show up */
	fu_console_set_progress(self->console, FWUPD_STATUS_IDLE, 0);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "device-added",
			 G_CALLBACK(fu_util_device_wait_added_cb),
			 &helper);
	g_source_set_callback(source, fu_util_device_wait_timeout_cb, self, NULL);
	g_source_attach(source, self->main_ctx);
	g_main_loop_run(self->loop);

	/* timed out */
	if (helper.device == NULL) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_ARGS,
			    "Stopped waiting for %s after %.0fms",
			    helper.value,
			    g_timer_elapsed(timer, NULL) * 1000.f);
		return FALSE;
	}

	/* success */
	fu_console_print(self->console,
			 /* TRANSLATORS: the device showed up in time */
			 _("Successfully waited %.0fms for device"),
			 g_timer_elapsed(timer, NULL) * 1000.f);
	return TRUE;
}

static gboolean
fu_util_quit(FuUtil *self, gchar **values, GError **error)
{
	/* success */
	return fwupd_client_quit(self->client, self->cancellable, error);
}

static gboolean
fu_util_device_test_full(FuUtil *self,
			 gchar **values,
			 FuUtilDeviceTestHelper *helper,
			 GError **error)
{
	/* required for interactive devices */
	self->current_operation = FU_UTIL_OPERATION_UPDATE;

	/* at least one argument required */
	if (g_strv_length(values) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* get the report metadata */
	helper->report_metadata =
	    fwupd_client_get_report_metadata(self->client, self->cancellable, error);
	if (helper->report_metadata == NULL)
		return FALSE;

	/* prepare to save the data as JSON */
	json_builder_begin_object(helper->builder);

	/* process all the files */
	json_builder_set_member_name(helper->builder, "results");
	json_builder_begin_array(helper->builder);
	for (guint i = 0; values[i] != NULL; i++) {
		json_builder_begin_object(helper->builder);
		if (!fu_util_device_test_filename(self, helper, values[i], error))
			return FALSE;
		json_builder_end_object(helper->builder);
	}
	json_builder_end_array(helper->builder);

	/* dump to screen as JSON format */
	json_builder_end_object(helper->builder);
	if (self->as_json) {
		if (!fu_util_print_builder(self->console, helper->builder, error))
			return FALSE;
	}

	/* just warning */
	if (helper->nr_skipped > 0) {
		g_autoptr(GString) str = g_string_new(NULL);
		g_string_append_printf(
		    str,
		    /* TRANSLATORS: device tests can be specific to a CPU type */
		    ngettext("%u test was skipped", "%u tests were skipped", helper->nr_skipped),
		    helper->nr_skipped);
		fu_console_print_full(self->console,
				      FU_CONSOLE_PRINT_FLAG_WARNING,
				      "%s\n",
				      str->str);
	}

	/* we need all to pass for a zero return code */
	if (helper->nr_missing > 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "%u devices required for %u tests were not found",
			    helper->nr_missing,
			    g_strv_length(values));
		return FALSE;
	}
	if (helper->nr_success == 0 && helper->nr_skipped == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "None of the tests were successful");
		return FALSE;
	}

	/* nag? */
	if (!fu_util_perhaps_show_unreported(self, error))
		return FALSE;

	/* success */
	return TRUE;
}

static gboolean
fu_util_device_emulate(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuUtilDeviceTestHelper) helper = fu_util_device_test_helper_new();
	helper->use_emulation = TRUE;
	self->flags |= FWUPD_INSTALL_FLAG_ONLY_EMULATED;
	self->filter_device_include |= FWUPD_DEVICE_FLAG_EMULATED;
	return fu_util_device_test_full(self, values, helper, error);
}

static gboolean
fu_util_device_test(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuUtilDeviceTestHelper) helper = fu_util_device_test_helper_new();
	self->filter_device_exclude |= FWUPD_DEVICE_FLAG_EMULATED;
	return fu_util_device_test_full(self, values, helper, error);
}

static gboolean
fu_util_download(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *basename = NULL;
	g_autoptr(GBytes) blob = NULL;

	/* one argument required */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* file already exists */
	basename = g_path_get_basename(values[0]);
	if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    g_file_test(basename, G_FILE_TEST_EXISTS)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_ARGS,
			    "%s already exists",
			    basename);
		return FALSE;
	}
	blob = fwupd_client_download_bytes(self->client,
					   values[0],
					   self->download_flags,
					   self->cancellable,
					   error);
	if (blob == NULL)
		return FALSE;
	return g_file_set_contents(basename,
				   g_bytes_get_data(blob, NULL),
				   g_bytes_get_size(blob),
				   error);
}

static gboolean
fu_util_local_install(FuUtil *self, gchar **values, GError **error)
{
	const gchar *id;
	g_autofree gchar *filename = NULL;
	g_autoptr(FwupdDevice) dev = NULL;

	/* handle both forms */
	if (g_strv_length(values) == 1) {
		id = FWUPD_DEVICE_ID_ANY;
	} else if (g_strv_length(values) == 2) {
		dev = fu_util_get_device_by_id(self, values[1], error);
		if (dev == NULL)
			return FALSE;
		id = fwupd_device_get_id(dev);
	} else {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	self->current_operation = FU_UTIL_OPERATION_INSTALL;

	/* install with flags chosen by the user */
	filename = fu_util_download_if_required(self, values[0], error);
	if (filename == NULL)
		return FALSE;

	/* detect bitlocker */
	if (dev != NULL && !self->no_safety_check && !self->assume_yes) {
		if (!fu_util_prompt_warning_fde(self->console, dev, error))
			return FALSE;
	}

	if (!fwupd_client_install(self->client,
				  id,
				  filename,
				  self->flags,
				  self->cancellable,
				  error))
		return FALSE;

	fu_util_display_current_message(self);

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	/* show reboot if needed */
	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_get_details(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) array = NULL;
	g_autoptr(FuUtilNode) root = g_node_new(NULL);

	/* check args */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* implied, important for get-details on a device not in your system */
	self->show_all = TRUE;

	array = fwupd_client_get_details(self->client, values[0], self->cancellable, error);
	if (array == NULL)
		return FALSE;
	if (self->as_json) {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		json_builder_begin_object(builder);
		fwupd_codec_array_to_json(array, "Devices", builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
		return fu_util_print_builder(self->console, builder, error);
	}

	fu_util_build_device_tree(self, root, array, NULL);
	fu_util_print_node(self->console, self->client, root);

	return TRUE;
}

static gboolean
fu_util_report_history_for_remote(FuUtil *self,
				  GPtrArray *devices,
				  FwupdRemote *remote_filter,
				  FwupdRemote *remote_upload,
				  GError **error)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *report_uri = NULL;
	g_autofree gchar *sig = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(GHashTable) metadata = NULL;

	/* convert to JSON */
	metadata = fwupd_client_get_report_metadata(self->client, self->cancellable, error);
	if (metadata == NULL)
		return FALSE;
	data = fwupd_client_build_report_history(self->client,
						 devices,
						 remote_filter,
						 metadata,
						 error);
	if (data == NULL)
		return FALSE;

	/* self sign data */
	if (self->sign) {
		sig = fwupd_client_self_sign(self->client,
					     data,
					     FWUPD_SELF_SIGN_FLAG_ADD_TIMESTAMP,
					     self->cancellable,
					     error);
		if (sig == NULL)
			return FALSE;
	}

	/* ask for permission */
	report_uri = fwupd_remote_build_report_uri(remote_upload, error);
	if (report_uri == NULL)
		return FALSE;
	if (!self->assume_yes &&
	    !fwupd_remote_has_flag(remote_upload, FWUPD_REMOTE_FLAG_AUTOMATIC_REPORTS)) {
		fu_console_print_kv(self->console, _("Target"), report_uri);
		fu_console_print_kv(self->console, _("Payload"), data);
		if (sig != NULL)
			fu_console_print_kv(self->console, _("Signature"), sig);
		if (!fu_console_input_bool(self->console, TRUE, "%s", _("Proceed with upload?"))) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_PERMISSION_DENIED,
					    "User declined action");
			return FALSE;
		}
	}

	/* POST request and parse reply */
	uri = fwupd_client_upload_report(self->client,
					 report_uri,
					 data,
					 sig,
					 FWUPD_CLIENT_UPLOAD_FLAG_NONE,
					 self->cancellable,
					 error);
	if (uri == NULL)
		return FALSE;

	/* server wanted us to see a message */
	if (g_strcmp0(uri, "") != 0) {
		fu_console_print(
		    self->console,
		    "%s %s",
		    /* TRANSLATORS: the server sent the user a small message */
		    _("Update failure is a known issue, visit this URL for more information:"),
		    uri);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_report_history_force(FuUtil *self, GPtrArray *devices, GError **error)
{
	g_autoptr(FwupdRemote) remote_upload = NULL;
	g_autoptr(GString) str = g_string_new(NULL);

	/* just assume every report goes to this remote */
	remote_upload =
	    fwupd_client_get_remote_by_id(self->client, "lvfs", self->cancellable, error);
	if (remote_upload == NULL)
		return FALSE;
	if (!fu_util_report_history_for_remote(self,
					       devices,
					       NULL, /* no filter */
					       remote_upload,
					       error))
		return FALSE;

	/* mark each device as reported */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *device = g_ptr_array_index(devices, i);
		g_debug("setting flag on %s", fwupd_device_get_id(device));
		if (!fwupd_client_modify_device(self->client,
						fwupd_device_get_id(device),
						"Flags",
						"reported",
						self->cancellable,
						error))
			return FALSE;
	}

	/* success */
	g_string_append_printf(str,
			       /* TRANSLATORS: success message -- where the user has uploaded
				* success and/or failure reports to the remote server */
			       ngettext("Successfully uploaded %u report",
					"Successfully uploaded %u reports",
					devices->len),
			       devices->len);
	fu_console_print_literal(self->console, str->str);
	return TRUE;
}

static gboolean
fu_util_report_export(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GHashTable) metadata = NULL;
	g_autoptr(GPtrArray) devices_filtered =
	    g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	g_autoptr(GPtrArray) devices = NULL;

	/* get all devices from the history database, then filter them and export to JSON */
	devices = fwupd_client_get_history(self->client, self->cancellable, error);
	if (devices == NULL)
		return FALSE;
	g_debug("%u devices with history", devices->len);

	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		gboolean dev_skip_byid = TRUE;

		/* only process particular DEVICE-ID or GUID if specified */
		for (guint idx = 0; idx < g_strv_length(values); idx++) {
			const gchar *tmpid = values[idx];
			const gchar *device_id = fwupd_device_get_id(dev);
			if (fwupd_device_has_guid(dev, tmpid) || g_strcmp0(device_id, tmpid) == 0) {
				dev_skip_byid = FALSE;
				break;
			}
		}
		if (g_strv_length(values) > 0 && dev_skip_byid)
			continue;

		/* filter, if not forcing */
		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
			if (fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_REPORTED)) {
				g_debug("%s has already been reported", fwupd_device_get_id(dev));
				continue;
			}
		}

		/* only send success and failure */
		if (fwupd_device_get_update_state(dev) != FWUPD_UPDATE_STATE_FAILED &&
		    fwupd_device_get_update_state(dev) != FWUPD_UPDATE_STATE_SUCCESS) {
			g_debug("ignoring %s with UpdateState %s",
				fwupd_device_get_id(dev),
				fwupd_update_state_to_string(fwupd_device_get_update_state(dev)));
			continue;
		}
		g_ptr_array_add(devices_filtered, g_object_ref(dev));
	}

	/* nothing to report, but try harder with --force */
	if (devices_filtered->len == 0 && (self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "No reports require uploading");
		return FALSE;
	}

	/* get metadata */
	metadata = fwupd_client_get_report_metadata(self->client, self->cancellable, error);
	if (metadata == NULL)
		return FALSE;

	/* write each device report as a new file */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		g_autofree gchar *data = NULL;
		g_autofree gchar *filename = NULL;
		g_autoptr(FuFirmware) archive = fu_archive_firmware_new();
		g_autoptr(FuFirmware) payload_img = NULL;
		g_autoptr(GBytes) payload_blob = NULL;
		g_autoptr(GFile) file = NULL;
		g_autoptr(GPtrArray) devices_tmp = g_ptr_array_new();

		/* convert single device to JSON */
		g_ptr_array_add(devices_tmp, dev);
		data = fwupd_client_build_report_history(self->client,
							 devices,
							 NULL, /* remote */
							 metadata,
							 error);
		if (data == NULL)
			return FALSE;
		payload_blob = g_bytes_new(data, strlen(data));
		payload_img = fu_firmware_new_from_bytes(payload_blob);
		fu_firmware_set_id(payload_img, "report.json");
		fu_firmware_add_image(archive, payload_img);

		/* self sign data */
		if (self->sign) {
			g_autofree gchar *sig = NULL;
			g_autoptr(FuFirmware) sig_img = NULL;
			g_autoptr(GBytes) sig_blob = NULL;

			sig = fwupd_client_self_sign(self->client,
						     data,
						     FWUPD_SELF_SIGN_FLAG_ADD_TIMESTAMP,
						     self->cancellable,
						     error);
			if (sig == NULL)
				return FALSE;
			sig_blob = g_bytes_new(sig, strlen(sig));
			sig_img = fu_firmware_new_from_bytes(sig_blob);
			fu_firmware_set_id(sig_img, "report.json.p7c");
			fu_firmware_add_image(archive, sig_img);
		}

		/* save to local file */
		fu_archive_firmware_set_format(FU_ARCHIVE_FIRMWARE(archive), FU_ARCHIVE_FORMAT_ZIP);
		fu_archive_firmware_set_compression(FU_ARCHIVE_FIRMWARE(archive),
						    FU_ARCHIVE_COMPRESSION_GZIP);
		filename = g_strdup_printf("%s.fwupdreport", fwupd_device_get_id(dev));
		file = g_file_new_for_path(filename);
		if (!fu_firmware_write_file(archive, file, error))
			return FALSE;

		/* TRANSLATORS: key for a offline report filename */
		fu_console_print_kv(self->console, _("Saved report"), filename);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_report_history_full(FuUtil *self, gboolean only_automatic_reports, GError **error)
{
	guint cnt = 0;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) remotes = NULL;

	/* get all devices from the history database, then filter them,
	 * adding to a hash map of report-ids */
	devices = fwupd_client_get_history(self->client, self->cancellable, error);
	if (devices == NULL)
		return FALSE;
	g_debug("%u devices with history", devices->len);

	/* ignore the previous reported flag */
	if (self->flags & FWUPD_INSTALL_FLAG_FORCE) {
		for (guint i = 0; i < devices->len; i++) {
			FwupdDevice *dev = g_ptr_array_index(devices, i);
			fwupd_device_remove_flag(dev, FWUPD_DEVICE_FLAG_REPORTED);
		}
	}

	/* needs an extra action, show something to the user */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		if (fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)) {
			g_autofree gchar *cmd = g_strdup_printf("%s activate", g_get_prgname());
			fu_console_print(
			    self->console,
			    /* TRANSLATORS: %1 is a device name, e.g. "ThinkPad Universal
			     * ThunderBolt 4 Dock" and %2 is "fwupdmgr activate" */
			    _("%s is pending activation; use %s to complete the update."),
			    fwupd_device_get_name(dev),
			    cmd);
		}
	}

	/* get all remotes */
	remotes = fwupd_client_get_remotes(self->client, self->cancellable, error);
	if (remotes == NULL)
		return FALSE;
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index(remotes, i);
		g_autoptr(GError) error_local = NULL;

		/* filter this so we can use it from fwupd-refresh */
		if (only_automatic_reports &&
		    !fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_AUTOMATIC_REPORTS)) {
			g_debug("%s has no AutomaticReports set", fwupd_remote_get_id(remote));
			continue;
		}

		/* try to upload */
		if (!fu_util_report_history_for_remote(self,
						       devices,
						       remote, /* filter */
						       remote, /* upload */
						       &error_local)) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO))
				continue;
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}

		/* keep track to make sure *something* worked */
		cnt += 1;
	}

	/* nothing to report, but try harder with --force */
	if (cnt == 0) {
		if (!only_automatic_reports && self->flags & FWUPD_INSTALL_FLAG_FORCE)
			return fu_util_report_history_force(self, devices, error);
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "No reports require uploading");
		return FALSE;
	}

	/* mark each device as reported */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		g_debug("setting flag on %s", fwupd_device_get_id(dev));
		if (!fwupd_client_modify_device(self->client,
						fwupd_device_get_id(dev),
						"Flags",
						"reported",
						self->cancellable,
						error))
			return FALSE;
	}

	/* TRANSLATORS: where the user has uploaded success and/or failure report to the server */
	fu_console_print_literal(self->console, "Successfully uploaded report");
	return TRUE;
}

static gboolean
fu_util_report_history(FuUtil *self, gchar **values, GError **error)
{
	if (values != NULL && g_strv_length(values) != 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}
	return fu_util_report_history_full(self, FALSE, error);
}

static gboolean
fu_util_get_history(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(FuUtilNode) root = g_node_new(NULL);

	/* get all devices from the history database */
	devices = fwupd_client_get_history(self->client, self->cancellable, error);
	if (devices == NULL)
		return FALSE;

	/* not for human consumption */
	if (self->as_json) {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		json_builder_begin_object(builder);
		fwupd_codec_array_to_json(devices, "Devices", builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
		return fu_util_print_builder(self->console, builder, error);
	}

	/* show each device */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		FwupdRelease *rel;
		FuUtilNode *child;

		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		child = g_node_append_data(root, g_object_ref(dev));

		rel = fwupd_device_get_release_default(dev);
		if (rel == NULL)
			continue;
		g_node_append_data(child, g_object_ref(rel));
	}

	fu_util_print_node(self->console, self->client, root);

	return TRUE;
}

static FwupdDevice *
fu_util_get_device_by_id(FuUtil *self, const gchar *id, GError **error)
{
	if (fwupd_guid_is_valid(id)) {
		g_autoptr(GPtrArray) devices = NULL;
		devices =
		    fwupd_client_get_devices_by_guid(self->client, id, self->cancellable, error);
		if (devices == NULL)
			return NULL;
		return fu_util_prompt_for_device(self, devices, error);
	}
	/* did this look like a GUID? */
	for (guint i = 0; id[i] != '\0'; i++) {
		if (id[i] == '-') {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_ARGS,
					    "Invalid arguments");
			return NULL;
		}
	}
	return fwupd_client_get_device_by_id(self->client, id, self->cancellable, error);
}

static FwupdDevice *
fu_util_get_device_or_prompt(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;

	/* get device to use */
	if (g_strv_length(values) >= 1) {
		if (g_strv_length(values) > 1) {
			for (guint i = 1; i < g_strv_length(values); i++)
				g_debug("ignoring extra input %s", values[i]);
		}
		return fu_util_get_device_by_id(self, values[0], error);
	}

	if (self->as_json) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "device ID required");
		return NULL;
	}

	/* get all devices from daemon */
	devices = fwupd_client_get_devices(self->client, self->cancellable, error);
	if (devices == NULL)
		return NULL;
	return fu_util_prompt_for_device(self, devices, error);
}

static FwupdRelease *
fu_util_get_release_for_device_version(FuUtil *self,
				       FwupdDevice *device,
				       const gchar *version,
				       GError **error)
{
	g_autoptr(GPtrArray) releases = NULL;

	/* get all releases */
	releases = fwupd_client_get_releases(self->client,
					     fwupd_device_get_id(device),
					     self->cancellable,
					     error);
	if (releases == NULL)
		return NULL;

	/* find using vercmp */
	for (guint j = 0; j < releases->len; j++) {
		FwupdRelease *release = g_ptr_array_index(releases, j);
		if (fu_version_compare(fwupd_release_get_version(release),
				       version,
				       fwupd_device_get_version_format(device)) == 0) {
			return g_object_ref(release);
		}
	}

	/* did not find */
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_SUPPORTED,
		    "Unable to locate release %s for %s",
		    version,
		    fwupd_device_get_name(device));
	return NULL;
}

static gboolean
fu_util_clear_results(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	return fwupd_client_clear_results(self->client,
					  fwupd_device_get_id(dev),
					  self->cancellable,
					  error);
}

static gboolean
fu_util_verify_update(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	self->filter_device_include |= FWUPD_DEVICE_FLAG_CAN_VERIFY;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	if (!fwupd_client_verify_update(self->client,
					fwupd_device_get_id(dev),
					self->cancellable,
					error)) {
		g_prefix_error(error, "failed to verify update %s: ", fwupd_device_get_name(dev));
		return FALSE;
	}

	/* TRANSLATORS: success message when user refreshes device checksums */
	fu_console_print_literal(self->console, _("Successfully updated device checksums"));

	return TRUE;
}

static gboolean
fu_util_download_metadata_enable_lvfs(FuUtil *self, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;

	/* is the LVFS available but disabled? */
	remote = fwupd_client_get_remote_by_id(self->client, "lvfs", self->cancellable, error);
	if (remote == NULL)
		return TRUE;
	fu_console_print_literal(
	    self->console,
	    /* TRANSLATORS: explain why no metadata available */
	    _("No remotes are currently enabled so no metadata is available."));
	fu_console_print_literal(
	    self->console,
	    /* TRANSLATORS: explain why no metadata available */
	    _("Metadata can be obtained from the Linux Vendor Firmware Service."));

	/* TRANSLATORS: Turn on the remote */
	if (!fu_console_input_bool(self->console, TRUE, "%s", _("Enable this remote?")))
		return TRUE;
	if (!fwupd_client_modify_remote(self->client,
					fwupd_remote_get_id(remote),
					"Enabled",
					"true",
					self->cancellable,
					error))
		return FALSE;
	if (!fu_util_modify_remote_warning(self->console, remote, self->assume_yes, error))
		return FALSE;

	/* refresh the newly-enabled remote */
	return fwupd_client_refresh_remote(self->client,
					   remote,
					   self->download_flags,
					   self->cancellable,
					   error);
}

static gboolean
fu_util_check_oldest_remote(FuUtil *self, guint64 *age_oldest, GError **error)
{
	g_autoptr(GPtrArray) remotes = NULL;
	gboolean checked = FALSE;

	/* get the age of the oldest enabled remotes */
	remotes = fwupd_client_get_remotes(self->client, self->cancellable, error);
	if (remotes == NULL)
		return FALSE;
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index(remotes, i);
		if (!fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_ENABLED))
			continue;
		if (fwupd_remote_get_kind(remote) != FWUPD_REMOTE_KIND_DOWNLOAD)
			continue;
		checked = TRUE;
		if (!fwupd_remote_needs_refresh(remote))
			continue;
		g_debug("%s is age %u",
			fwupd_remote_get_id(remote),
			(guint)fwupd_remote_get_age(remote));
		if (fwupd_remote_get_age(remote) > *age_oldest)
			*age_oldest = fwupd_remote_get_age(remote);
	}
	if (!checked) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: error message for a user who ran fwupdmgr
				       refresh recently but no remotes */
				    "No remotes enabled.");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_util_download_metadata(FuUtil *self, GError **error)
{
	gboolean download_remote_enabled = FALSE;
	guint devices_supported_cnt = 0;
	guint devices_updatable_cnt = 0;
	guint refresh_cnt = 0;
	g_autoptr(GPtrArray) devs = NULL;
	g_autoptr(GPtrArray) remotes = NULL;
	g_autoptr(GString) str = g_string_new(NULL);
	g_autoptr(GError) error_local = NULL;

	remotes = fwupd_client_get_remotes(self->client, self->cancellable, error);
	if (remotes == NULL)
		return FALSE;
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index(remotes, i);
		if (!fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_ENABLED))
			continue;
		if (fwupd_remote_get_kind(remote) != FWUPD_REMOTE_KIND_DOWNLOAD)
			continue;
		download_remote_enabled = TRUE;
		if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
		    !fwupd_remote_needs_refresh(remote)) {
			g_debug("skipping as remote %s age is %us",
				fwupd_remote_get_id(remote),
				(guint)fwupd_remote_get_age(remote));
			continue;
		}
		if (!self->as_json)
			fu_console_print(self->console,
					 "%s %s",
					 _("Updating"),
					 fwupd_remote_get_id(remote));
		if (!fwupd_client_refresh_remote(self->client,
						 remote,
						 self->download_flags,
						 self->cancellable,
						 error))
			return FALSE;
		refresh_cnt++;
	}

	/* no web remote is declared; try to enable LVFS */
	if (!download_remote_enabled) {
		/* we don't want to ask anything */
		if (self->no_remote_check) {
			g_debug("skipping remote check");
			return TRUE;
		}

		if (!fu_util_download_metadata_enable_lvfs(self, error))
			return FALSE;
	}

	/* metadata refreshed recently */
	if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 && refresh_cnt == 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOTHING_TO_DO,
			    /* TRANSLATORS: error message for a user who ran fwupdmgr
			     * refresh recently -- %1 is '--force' */
			    _("Metadata is up to date; use %s to refresh again."),
			    "--force");
		return FALSE;
	}

	if (self->as_json)
		return TRUE;

	/* get devices from daemon */
	devs = fwupd_client_get_devices(self->client, self->cancellable, error);
	if (devs == NULL)
		return FALSE;

	/* get results */
	for (guint i = 0; i < devs->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devs, i);
		if (fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_SUPPORTED))
			devices_supported_cnt++;
		if (fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE))
			devices_updatable_cnt++;
	}

	/* TRANSLATORS: success message -- where 'metadata' is information
	 * about available firmware on the remote server */
	g_string_append(str, _("Successfully downloaded new metadata: "));

	g_string_append_printf(str,
			       /* TRANSLATORS: how many local devices can expect updates now */
			       ngettext("Updates have been published for %u local device",
					"Updates have been published for %u of %u local devices",
					devices_supported_cnt),
			       devices_supported_cnt,
			       devices_updatable_cnt);
	fu_console_print_literal(self->console, str->str);

	/* auto-upload any reports */
	if (!fu_util_report_history_full(self, TRUE, &error_local)) {
		if (!g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}
		g_debug("failed to auto-upload reports: %s", error_local->message);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_refresh(FuUtil *self, gchar **values, GError **error)
{
	if (g_strv_length(values) == 0)
		return fu_util_download_metadata(self, error);
	if (g_strv_length(values) != 3) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* open file */
	if (!fwupd_client_update_metadata(self->client,
					  values[2],
					  values[0],
					  values[1],
					  self->cancellable,
					  error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: success message -- the user can do this by-hand too */
	fu_console_print_literal(self->console, _("Successfully refreshed metadata manually"));
	return TRUE;
}

static gboolean
fu_util_get_results(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *tmp = NULL;
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(FwupdDevice) rel = NULL;

	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	rel = fwupd_client_get_results(self->client,
				       fwupd_device_get_id(dev),
				       self->cancellable,
				       error);
	if (rel == NULL)
		return FALSE;
	if (self->as_json) {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		json_builder_begin_object(builder);
		fwupd_codec_to_json(FWUPD_CODEC(rel), builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
		return fu_util_print_builder(self->console, builder, error);
	}
	tmp = fu_util_device_to_string(self->client, rel, 0);
	fu_console_print_literal(self->console, tmp);
	return TRUE;
}

static gboolean
fu_util_get_releases(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(GPtrArray) rels = NULL;

	self->filter_device_include |= FWUPD_DEVICE_FLAG_SUPPORTED;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	/* get the releases for this device */
	rels = fwupd_client_get_releases(self->client,
					 fwupd_device_get_id(dev),
					 self->cancellable,
					 error);
	if (rels == NULL)
		return FALSE;

	/* not for human consumption */
	if (self->as_json)
		return fu_util_get_releases_as_json(self, rels, error);

	if (rels->len == 0) {
		/* TRANSLATORS: no repositories to download from */
		fu_console_print_literal(self->console, _("No releases available"));
		return TRUE;
	}
	if (g_getenv("FWUPD_VERBOSE") != NULL) {
		for (guint i = 0; i < rels->len; i++) {
			FwupdRelease *rel = g_ptr_array_index(rels, i);
			g_autofree gchar *tmp = NULL;
			if (!fwupd_release_match_flags(rel,
						       self->filter_release_include,
						       self->filter_release_exclude))
				continue;
			tmp = fwupd_codec_to_string(FWUPD_CODEC(rel));
			fu_console_print_literal(self->console, tmp);
		}
	} else {
		g_autoptr(FuUtilNode) root = g_node_new(NULL);
		for (guint i = 0; i < rels->len; i++) {
			FwupdRelease *rel = g_ptr_array_index(rels, i);
			if (!fwupd_release_match_flags(rel,
						       self->filter_release_include,
						       self->filter_release_exclude))
				continue;
			g_node_append_data(root, g_object_ref(rel));
		}
		fu_util_print_node(self->console, self->client, root);
	}

	return TRUE;
}

static FwupdRelease *
fu_util_prompt_for_release(FuUtil *self, GPtrArray *rels_unfiltered, GError **error)
{
	FwupdRelease *rel;
	guint idx;
	g_autoptr(GPtrArray) rels = NULL;

	/* filter */
	rels = fwupd_release_array_filter_flags(rels_unfiltered,
						self->filter_release_include,
						self->filter_release_exclude,
						error);
	if (rels == NULL)
		return NULL;

	/* exactly one */
	if (rels->len == 1) {
		rel = g_ptr_array_index(rels, 0);
		return g_object_ref(rel);
	}

	/* TRANSLATORS: this is to abort the interactive prompt */
	fu_console_print(self->console, "0.\t%s", _("Cancel"));
	for (guint i = 0; i < rels->len; i++) {
		FwupdRelease *rel_tmp = g_ptr_array_index(rels, i);
		fu_console_print(self->console,
				 "%u.\t%s",
				 i + 1,
				 fwupd_release_get_version(rel_tmp));
	}
	/* TRANSLATORS: get interactive prompt */
	idx = fu_console_input_uint(self->console, rels->len, "%s", _("Choose release"));
	if (idx == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "Request canceled");
		return NULL;
	}
	rel = g_ptr_array_index(rels, idx - 1);
	return g_object_ref(rel);
}

static gboolean
fu_util_verify(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	self->filter_device_include |= FWUPD_DEVICE_FLAG_CAN_VERIFY;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	if (!fwupd_client_verify(self->client,
				 fwupd_device_get_id(dev),
				 self->cancellable,
				 error)) {
		g_prefix_error(error, "failed to verify %s: ", fwupd_device_get_name(dev));
		return FALSE;
	}

	/* TRANSLATORS: success message when user verified device checksums */
	fu_console_print_literal(self->console, _("Successfully verified device checksums"));

	return TRUE;
}

static gboolean
fu_util_unlock(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	self->filter_device_include |= FWUPD_DEVICE_FLAG_LOCKED;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	if (!fwupd_client_unlock(self->client, fwupd_device_get_id(dev), self->cancellable, error))
		return FALSE;

	/* check flags after unlocking in case the operation changes them */
	if (fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN))
		self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN;
	if (fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_NEEDS_REBOOT))
		self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_REBOOT;

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_perhaps_refresh_remotes(FuUtil *self, GError **error)
{
	guint64 age_oldest = 0;
	const guint64 age_limit_days = 30;

	/* we don't want to ask anything */
	if (self->no_metadata_check || self->as_json) {
		g_debug("skipping metadata check");
		return TRUE;
	}

	if (!fu_util_check_oldest_remote(self, &age_oldest, NULL))
		return TRUE;

	/* metadata is new enough */
	if (age_oldest < 60 * 60 * 24 * age_limit_days)
		return TRUE;

	/* ask for permission */
	if (!self->assume_yes) {
		fu_console_print(
		    self->console,
		    /* TRANSLATORS: the metadata is very out of date; %u is a number > 1 */
		    ngettext("Firmware metadata has not been updated for %u"
			     " day and may not be up to date.",
			     "Firmware metadata has not been updated for %u"
			     " days and may not be up to date.",
			     (gint)age_limit_days),
		    (guint)age_limit_days);
		if (!fu_console_input_bool(self->console,
					   FALSE,
					   "%s (%s)",
					   /* TRANSLATORS: ask if we can update metadata */
					   _("Update now?"),
					   /* TRANSLATORS: metadata is downloaded */
					   _("Requires internet connection")))
			return TRUE;
	}

	/* downloads new metadata */
	return fu_util_download_metadata(self, error);
}

static gboolean
fu_util_get_updates_as_json(FuUtil *self, GPtrArray *devices, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "Devices");
	json_builder_begin_array(builder);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;

		/* not going to have results, so save a D-Bus round-trip */
		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_SUPPORTED))
			continue;

		/* get the releases for this device and filter for validity */
		rels = fwupd_client_get_upgrades(self->client,
						 fwupd_device_get_id(dev),
						 self->cancellable,
						 &error_local);
		if (rels == NULL) {
			g_debug("no upgrades: %s", error_local->message);
			continue;
		}
		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel = g_ptr_array_index(rels, j);
			if (!fwupd_release_match_flags(rel,
						       self->filter_release_include,
						       self->filter_release_exclude))
				continue;
			fwupd_device_add_release(dev, rel);
		}

		/* add to builder */
		json_builder_begin_object(builder);
		fwupd_codec_to_json(FWUPD_CODEC(dev), builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return fu_util_print_builder(self->console, builder, error);
}

static gboolean
fu_util_get_updates(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	gboolean supported = FALSE;
	g_autoptr(FuUtilNode) root = g_node_new(NULL);
	g_autoptr(GPtrArray) devices_no_support = g_ptr_array_new();
	g_autoptr(GPtrArray) devices_no_upgrades = g_ptr_array_new();

	/* are the remotes very old */
	if (!fu_util_perhaps_refresh_remotes(self, error))
		return FALSE;

	/* handle both forms */
	if (g_strv_length(values) == 0) {
		devices = fwupd_client_get_devices(self->client, self->cancellable, error);
		if (devices == NULL)
			return FALSE;
	} else {
		devices = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
		for (guint idx = 0; idx < g_strv_length(values); idx++) {
			FwupdDevice *device = fu_util_get_device_by_id(self, values[idx], error);
			if (device == NULL) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_ARGS,
					    "'%s' is not a valid GUID nor DEVICE-ID",
					    values[idx]);
				return FALSE;
			}
			g_ptr_array_add(devices, device);
		}
	}
	g_ptr_array_sort(devices, fu_util_sort_devices_by_flags_cb);

	/* not for human consumption */
	if (self->as_json)
		return fu_util_get_updates_as_json(self, devices, error);

	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;
		FuUtilNode *child;

		/* not going to have results, so save a D-Bus round-trip */
		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE) &&
		    !fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN))
			continue;
		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_SUPPORTED)) {
			g_ptr_array_add(devices_no_support, dev);
			continue;
		}
		supported = TRUE;

		/* get the releases for this device and filter for validity */
		rels = fwupd_client_get_upgrades(self->client,
						 fwupd_device_get_id(dev),
						 self->cancellable,
						 &error_local);
		if (rels == NULL) {
			g_ptr_array_add(devices_no_upgrades, dev);
			/* discard the actual reason from user, but leave for debugging */
			g_debug("%s", error_local->message);
			continue;
		}
		child = g_node_append_data(root, g_object_ref(dev));

		/* add all releases */
		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel = g_ptr_array_index(rels, j);
			if (!fwupd_release_match_flags(rel,
						       self->filter_release_include,
						       self->filter_release_exclude))
				continue;
			g_node_append_data(child, g_object_ref(rel));
		}
	}

	/* devices that have no updates available for whatever reason */
	if (devices_no_support->len > 0) {
		fu_console_print_literal(self->console,
					 /* TRANSLATORS: message letting the user know no device
					  * upgrade available due to missing on LVFS */
					 _("Devices with no available firmware updates: "));
		for (guint i = 0; i < devices_no_support->len; i++) {
			FwupdDevice *dev = g_ptr_array_index(devices_no_support, i);
			fu_console_print(self->console, " • %s", fwupd_device_get_name(dev));
		}
	}
	if (devices_no_upgrades->len > 0) {
		fu_console_print_literal(
		    self->console,
		    /* TRANSLATORS: message letting the user know no device upgrade available */
		    _("Devices with the latest available firmware version:"));
		for (guint i = 0; i < devices_no_upgrades->len; i++) {
			FwupdDevice *dev = g_ptr_array_index(devices_no_upgrades, i);
			fu_console_print(self->console, " • %s", fwupd_device_get_name(dev));
		}
	}

	/* nag? */
	if (!fu_util_perhaps_show_unreported(self, error))
		return FALSE;

	/* no devices supported by LVFS or all are filtered */
	if (!supported) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: this is an error string */
				    _("No updatable devices"));
		return FALSE;
	}
	/* no updates available */
	if (g_node_n_nodes(root, G_TRAVERSE_ALL) <= 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: this is an error string */
				    _("No updates available"));
		return FALSE;
	}

	fu_util_print_node(self->console, self->client, root);

	/* success */
	return TRUE;
}

static gboolean
fu_util_get_remotes(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuUtilNode) root = g_node_new(NULL);
	g_autoptr(GPtrArray) remotes = NULL;

	remotes = fwupd_client_get_remotes(self->client, self->cancellable, error);
	if (remotes == NULL)
		return FALSE;
	if (self->as_json) {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		json_builder_begin_object(builder);
		fwupd_codec_array_to_json(remotes, "Remotes", builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
		return fu_util_print_builder(self->console, builder, error);
	}

	if (remotes->len == 0) {
		/* TRANSLATORS: no repositories to download from */
		fu_console_print_literal(self->console, _("No remotes available"));
		return TRUE;
	}

	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote_tmp = g_ptr_array_index(remotes, i);
		g_node_append_data(root, g_object_ref(remote_tmp));
	}
	fu_util_print_node(self->console, self->client, root);

	return TRUE;
}

static FwupdRelease *
fu_util_get_release_with_tag(FuUtil *self, FwupdDevice *dev, const gchar *host_bkc, GError **error)
{
	g_autoptr(GPtrArray) rels = NULL;
	g_auto(GStrv) host_bkcs = g_strsplit(host_bkc, ",", -1);

	/* find the newest release that matches */
	rels = fwupd_client_get_releases(self->client,
					 fwupd_device_get_id(dev),
					 self->cancellable,
					 error);
	if (rels == NULL)
		return NULL;
	for (guint i = 0; i < rels->len; i++) {
		FwupdRelease *rel = g_ptr_array_index(rels, i);
		if (!fwupd_release_match_flags(rel,
					       self->filter_release_include,
					       self->filter_release_exclude))
			continue;
		for (guint j = 0; host_bkcs[j] != NULL; j++) {
			if (fwupd_release_has_tag(rel, host_bkcs[j]))
				return g_object_ref(rel);
		}
	}

	/* no match */
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "no matching releases for device");
	return NULL;
}

static FwupdRelease *
fu_util_get_release_with_branch(FuUtil *self, FwupdDevice *dev, const gchar *branch, GError **error)
{
	g_autoptr(GPtrArray) rels = NULL;

	/* find the newest release that matches */
	rels = fwupd_client_get_releases(self->client,
					 fwupd_device_get_id(dev),
					 self->cancellable,
					 error);
	if (rels == NULL)
		return NULL;
	for (guint i = 0; i < rels->len; i++) {
		FwupdRelease *rel = g_ptr_array_index(rels, i);
		if (!fwupd_release_match_flags(rel,
					       self->filter_release_include,
					       self->filter_release_exclude))
			continue;
		if (g_strcmp0(branch, fwupd_release_get_branch(rel)) == 0)
			return g_object_ref(rel);
	}

	/* no match */
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "no matching releases for device");
	return NULL;
}

static gboolean
fu_util_prompt_warning_bkc(FuUtil *self, FwupdDevice *dev, FwupdRelease *rel, GError **error)
{
	const gchar *host_bkc = fwupd_client_get_host_bkc(self->client);
	g_autofree gchar *cmd = g_strdup_printf("%s sync", g_get_prgname());
	g_autoptr(FwupdRelease) rel_bkc = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GString) str = g_string_new(NULL);

	/* nothing to do */
	if (host_bkc == NULL)
		return TRUE;

	/* get the release that corresponds with the host BKC */
	rel_bkc = fu_util_get_release_with_tag(self, dev, host_bkc, &error_local);
	if (rel_bkc == NULL) {
		if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED) ||
		    g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
			g_debug("ignoring %s: %s", fwupd_device_get_id(dev), error_local->message);
			return TRUE;
		}
		g_propagate_error(error, g_steal_pointer(&error_local));
		return FALSE;
	}

	/* device is already on a different release */
	if (g_strcmp0(fwupd_device_get_version(dev), fwupd_release_get_version(rel)) != 0)
		return TRUE;

	/* TRANSLATORS: BKC is the industry name for the best known configuration and is a set
	 * of firmware that works together */
	g_string_append_printf(str, _("Your system is set up to the BKC of %s."), host_bkc);
	g_string_append(str, "\n\n");
	g_string_append_printf(
	    str,
	    /* TRANSLATORS: %1 is the current device version number, and %2 is the
	       command name, e.g. `fwupdmgr sync` */
	    _("This device will be reverted back to %s when the %s command is performed."),
	    fwupd_release_get_version(rel),
	    cmd);

	fu_console_box(
	    self->console,
	    /* TRANSLATORS: the best known configuration is a set of software that we know works
	     * well together. In the OEM and ODM industries it is often called a BKC */
	    _("Deviate from the best known configuration?"),
	    str->str,
	    80);

	/* TRANSLATORS: prompt to apply the update */
	if (!fu_console_input_bool(self->console, TRUE, "%s", _("Perform operation?"))) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "Request canceled");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_prompt_warning_composite(FuUtil *self, FwupdDevice *dev, FwupdRelease *rel, GError **error)
{
	const gchar *rel_csum;
	g_autoptr(GPtrArray) devices = NULL;

	/* get the default checksum */
	rel_csum = fwupd_checksum_get_best(fwupd_release_get_checksums(rel));
	if (rel_csum == NULL) {
		g_debug("no checksum for release!");
		return TRUE;
	}

	/* find other devices matching the composite ID and the release checksum */
	devices = fwupd_client_get_devices(self->client, self->cancellable, error);
	if (devices == NULL)
		return FALSE;
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev_tmp = g_ptr_array_index(devices, i);
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GPtrArray) rels = NULL;

		/* not the parent device */
		if (g_strcmp0(fwupd_device_get_id(dev), fwupd_device_get_id(dev_tmp)) == 0)
			continue;

		/* not the same composite device */
		if (g_strcmp0(fwupd_device_get_composite_id(dev),
			      fwupd_device_get_composite_id(dev_tmp)) != 0)
			continue;

		/* get releases */
		if (!fwupd_device_has_flag(dev_tmp, FWUPD_DEVICE_FLAG_UPDATABLE))
			continue;
		rels = fwupd_client_get_releases(self->client,
						 fwupd_device_get_id(dev_tmp),
						 self->cancellable,
						 &error_local);
		if (rels == NULL) {
			g_debug("ignoring: %s", error_local->message);
			continue;
		}

		/* do any releases match this checksum */
		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel_tmp = g_ptr_array_index(rels, j);
			if (fwupd_release_has_checksum(rel_tmp, rel_csum)) {
				g_autofree gchar *title =
				    g_strdup_printf("%s %s",
						    fwupd_client_get_host_product(self->client),
						    fwupd_client_get_host_product(self->client));
				if (!fu_util_prompt_warning(self->console,
							    dev_tmp,
							    rel_tmp,
							    title,
							    error))
					return FALSE;
				break;
			}
		}
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_update_device_with_release(FuUtil *self,
				   FwupdDevice *dev,
				   FwupdRelease *rel,
				   GError **error)
{
	if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE)) {
		const gchar *name = fwupd_device_get_name(dev);
		g_autofree gchar *str = NULL;

		/* TRANSLATORS: the device has a reason it can't update, e.g. laptop lid closed */
		str = g_strdup_printf(_("%s is not currently updatable"), name);
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOTHING_TO_DO,
			    "%s: %s",
			    str,
			    fwupd_device_get_update_error(dev));
		return FALSE;
	}
	if (!self->as_json && !self->no_safety_check && !self->assume_yes) {
		const gchar *title = fwupd_client_get_host_product(self->client);
		if (!fu_util_prompt_warning(self->console, dev, rel, title, error))
			return FALSE;
		if (!fu_util_prompt_warning_fde(self->console, dev, error))
			return FALSE;
		if (!fu_util_prompt_warning_composite(self, dev, rel, error))
			return FALSE;
		if (!fu_util_prompt_warning_bkc(self, dev, rel, error))
			return FALSE;
	}
	return fwupd_client_install_release(self->client,
					    dev,
					    rel,
					    self->flags,
					    self->download_flags,
					    self->cancellable,
					    error);
}

static gboolean
fu_util_maybe_send_reports(FuUtil *self, FwupdRelease *rel, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;
	g_autoptr(GError) error_local = NULL;
	if (fwupd_release_get_remote_id(rel) == NULL) {
		g_debug("not sending reports, no remote");
		return TRUE;
	}
	remote = fwupd_client_get_remote_by_id(self->client,
					       fwupd_release_get_remote_id(rel),
					       self->cancellable,
					       error);
	if (remote == NULL)
		return FALSE;
	if (fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_AUTOMATIC_REPORTS)) {
		if (!fu_util_report_history(self, NULL, &error_local))
			if (!g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED))
				g_warning("%s", error_local->message);
	}

	return TRUE;
}

static gboolean
fu_util_update(FuUtil *self, gchar **values, GError **error)
{
	gboolean supported = FALSE;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_latest = g_ptr_array_new();
	g_autoptr(GPtrArray) devices_pending = g_ptr_array_new();
	g_autoptr(GPtrArray) devices_unsupported = g_ptr_array_new();

	if (self->flags & FWUPD_INSTALL_FLAG_ALLOW_OLDER) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "--allow-older is not supported for this command");
		return FALSE;
	}

	if (self->flags & FWUPD_INSTALL_FLAG_ALLOW_REINSTALL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "--allow-reinstall is not supported for this command");
		return FALSE;
	}

	/* DEVICE-ID and GUID are acceptable args to update */
	for (guint idx = 0; idx < g_strv_length(values); idx++) {
		if (!fwupd_guid_is_valid(values[idx]) && !fwupd_device_id_is_valid(values[idx])) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "'%s' is not a valid GUID nor DEVICE-ID",
				    values[idx]);
			return FALSE;
		}
	}

	/* get devices from daemon */
	devices = fwupd_client_get_devices(self->client, self->cancellable, error);
	if (devices == NULL)
		return FALSE;
	self->current_operation = FU_UTIL_OPERATION_UPDATE;
	g_ptr_array_sort(devices, fu_util_sort_devices_by_flags_cb);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		const gchar *device_id = fwupd_device_get_id(dev);
		g_autoptr(FwupdRelease) rel = NULL;
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_install = NULL;
		g_autoptr(GError) error_report = NULL;
		gboolean dev_skip_byid = TRUE;
		gboolean ret;

		/* not going to have results, so save a D-Bus round-trip */
		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE) &&
		    !fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN))
			continue;
		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_SUPPORTED)) {
			g_ptr_array_add(devices_unsupported, dev);
			continue;
		}

		/* only process particular DEVICE-ID or GUID if specified */
		for (guint idx = 0; idx < g_strv_length(values); idx++) {
			const gchar *tmpid = values[idx];
			if (fwupd_device_has_guid(dev, tmpid) || g_strcmp0(device_id, tmpid) == 0) {
				dev_skip_byid = FALSE;
				break;
			}
		}
		if (g_strv_length(values) > 0 && dev_skip_byid)
			continue;
		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		supported = TRUE;

		/* get the releases for this device and filter for validity */
		rels = fwupd_client_get_upgrades(self->client,
						 fwupd_device_get_id(dev),
						 self->cancellable,
						 &error_install);
		if (rels == NULL) {
			g_ptr_array_add(devices_latest, dev);
			/* discard the actual reason from user, but leave for debugging */
			g_debug("%s", error_install->message);
			continue;
		}
		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel_tmp = g_ptr_array_index(rels, j);
			if (!fwupd_release_match_flags(rel_tmp,
						       self->filter_release_include,
						       self->filter_release_exclude))
				continue;
			rel = g_object_ref(rel_tmp);
			break;
		}
		if (rel == NULL)
			continue;

		/* something is wrong */
		if (fwupd_device_get_problems(dev) != FWUPD_DEVICE_PROBLEM_NONE) {
			g_ptr_array_add(devices_pending, dev);
			continue;
		}

		ret = fu_util_update_device_with_release(self, dev, rel, &error_install);
		if (!ret &&
		    g_error_matches(error_install, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
			g_debug("ignoring %s: %s",
				fwupd_device_get_id(dev),
				error_install->message);
			continue;
		}
		if (ret)
			fu_util_display_current_message(self);

		/* send report if we're supposed to */
		if (!fu_util_maybe_send_reports(self, rel, &error_report)) {
			/* install failed, report failed */
			if (!ret) {
				g_warning("%s", error_report->message);
				/* install succeeded, but report failed */
			} else {
				g_propagate_error(error, g_steal_pointer(&error_report));
				return FALSE;
			}
		}

		if (!ret) {
			g_propagate_error(error, g_steal_pointer(&error_install));
			return FALSE;
		}
	}

	/* show warnings */
	if (devices_latest->len > 0 && !self->as_json) {
		fu_console_print_literal(self->console,
					 /* TRANSLATORS: message letting the user know no device
					  * upgrade available */
					 _("Devices with the latest available firmware version:"));
		for (guint i = 0; i < devices_latest->len; i++) {
			FwupdDevice *dev = g_ptr_array_index(devices_latest, i);
			fu_console_print(self->console, " • %s", fwupd_device_get_name(dev));
		}
	}
	if (devices_unsupported->len > 0 && !self->as_json) {
		fu_console_print_literal(self->console,
					 /* TRANSLATORS: message letting the user know no
					  * device upgrade available due to missing on LVFS */
					 _("Devices with no available firmware updates: "));
		for (guint i = 0; i < devices_unsupported->len; i++) {
			FwupdDevice *dev = g_ptr_array_index(devices_unsupported, i);
			fu_console_print(self->console, " • %s", fwupd_device_get_name(dev));
		}
	}
	if (devices_pending->len > 0 && !self->as_json) {
		fu_console_print_literal(
		    self->console,
		    /* TRANSLATORS: message letting the user there is an update
		     * waiting, but there is a reason it cannot be deployed */
		    _("Devices with firmware updates that need user action: "));
		for (guint i = 0; i < devices_pending->len; i++) {
			FwupdDevice *dev = g_ptr_array_index(devices_pending, i);
			fu_console_print(self->console, " • %s", fwupd_device_get_name(dev));
			for (guint j = 0; j < 64; j++) {
				FwupdDeviceProblem problem = (guint64)1 << j;
				g_autofree gchar *desc = NULL;
				if (!fwupd_device_has_problem(dev, problem))
					continue;
				desc = fu_util_device_problem_to_string(self->client, dev, problem);
				if (desc == NULL)
					continue;
				fu_console_print(self->console, "   ‣ %s", desc);
			}
		}
	}

	/* no devices supported by LVFS or all are filtered */
	if (!supported) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "No updatable devices");
		return FALSE;
	}

	/* we don't want to ask anything */
	if (self->no_reboot_check || self->as_json) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_remote_modify(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;
	if (g_strv_length(values) < 3) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* ensure the remote exists */
	remote = fwupd_client_get_remote_by_id(self->client, values[0], self->cancellable, error);
	if (remote == NULL)
		return FALSE;
	if (!fwupd_client_modify_remote(self->client,
					fwupd_remote_get_id(remote),
					values[1],
					values[2],
					self->cancellable,
					error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: success message for a per-remote setting change */
	fu_console_print_literal(self->console, _("Successfully modified remote"));
	return TRUE;
}

static gboolean
fu_util_remote_enable(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}
	remote = fwupd_client_get_remote_by_id(self->client, values[0], self->cancellable, error);
	if (remote == NULL)
		return FALSE;
	if (!fu_util_modify_remote_warning(self->console, remote, self->assume_yes, error))
		return FALSE;
	if (!fwupd_client_modify_remote(self->client,
					fwupd_remote_get_id(remote),
					"Enabled",
					"true",
					self->cancellable,
					error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	/* ask for permission to refresh */
	if (self->no_remote_check || fwupd_remote_get_kind(remote) != FWUPD_REMOTE_KIND_DOWNLOAD) {
		/* TRANSLATORS: success message */
		fu_console_print_literal(self->console, _("Successfully enabled remote"));
		return TRUE;
	}
	if (!self->assume_yes) {
		if (!fu_console_input_bool(self->console,
					   TRUE,
					   "%s (%s)",
					   /* TRANSLATORS: ask if we can update the metadata */
					   _("Do you want to refresh this remote now?"),
					   /* TRANSLATORS: metadata is downloaded */
					   _("Requires internet connection"))) {
			/* TRANSLATORS: success message */
			fu_console_print_literal(self->console, _("Successfully enabled remote"));
			return TRUE;
		}
	}
	if (!fwupd_client_refresh_remote(self->client,
					 remote,
					 self->download_flags,
					 self->cancellable,
					 error))
		return FALSE;

	/* TRANSLATORS: success message */
	fu_console_print_literal(self->console, _("Successfully enabled and refreshed remote"));
	return TRUE;
}

static gboolean
fu_util_remote_disable(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;

	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* ensure the remote exists */
	remote = fwupd_client_get_remote_by_id(self->client, values[0], self->cancellable, error);
	if (remote == NULL)
		return FALSE;
	if (!fwupd_client_modify_remote(self->client,
					values[0],
					"Enabled",
					"false",
					self->cancellable,
					error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: success message */
	fu_console_print_literal(self->console, _("Successfully disabled remote"));
	return TRUE;
}

static gboolean
fu_util_downgrade(FuUtil *self, gchar **values, GError **error)
{
	gboolean ret;
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;
	g_autoptr(GError) error_report = NULL;

	if (self->flags & FWUPD_INSTALL_FLAG_ALLOW_REINSTALL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "--allow-reinstall is not supported for this command");
		return FALSE;
	}

	self->filter_device_include |= FWUPD_DEVICE_FLAG_SUPPORTED;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	/* get the releases for this device and filter for validity */
	rels = fwupd_client_get_downgrades(self->client,
					   fwupd_device_get_id(dev),
					   self->cancellable,
					   error);
	if (rels == NULL) {
		g_autofree gchar *downgrade_str =
		    /* TRANSLATORS: message letting the user know no device downgrade available
		     * %1 is the device name */
		    g_strdup_printf(_("No downgrades for %s"), fwupd_device_get_name(dev));
		g_prefix_error(error, "%s: ", downgrade_str);
		return FALSE;
	}

	/* get the chosen release */
	rel = fu_util_prompt_for_release(self, rels, error);
	if (rel == NULL)
		return FALSE;

	/* update the console if composite devices are also updated */
	self->current_operation = FU_UTIL_OPERATION_DOWNGRADE;
	self->flags |= FWUPD_INSTALL_FLAG_ALLOW_OLDER;
	ret = fu_util_update_device_with_release(self, dev, rel, error);
	if (ret)
		fu_util_display_current_message(self);

	/* send report if we're supposed to */
	if (!fu_util_maybe_send_reports(self, rel, &error_report)) {
		/* install failed, report failed */
		if (!ret) {
			g_warning("%s", error_report->message);
			/* install succeeded, but report failed */
		} else {
			g_propagate_error(error, g_steal_pointer(&error_report));
			return FALSE;
		}
	}

	/* install failed */
	if (!ret)
		return FALSE;

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_reinstall(FuUtil *self, gchar **values, GError **error)
{
	gboolean ret;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(GError) error_report = NULL;

	self->filter_device_include |= FWUPD_DEVICE_FLAG_SUPPORTED;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	/* try to lookup/match release from client */
	rel =
	    fu_util_get_release_for_device_version(self, dev, fwupd_device_get_version(dev), error);
	if (rel == NULL)
		return FALSE;

	/* update the console if composite devices are also updated */
	self->current_operation = FU_UTIL_OPERATION_INSTALL;
	self->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	ret = fu_util_update_device_with_release(self, dev, rel, error);
	if (ret)
		fu_util_display_current_message(self);

	/* send report if we're supposed to */
	if (!fu_util_maybe_send_reports(self, rel, &error_report)) {
		/* install failed, report failed */
		if (!ret) {
			g_warning("%s", error_report->message);
			/* install succeeded, but report failed */
		} else {
			g_propagate_error(error, g_steal_pointer(&error_report));
			return FALSE;
		}
	}

	/* install failed */
	if (!ret)
		return FALSE;

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_install(FuUtil *self, gchar **values, GError **error)
{
	gboolean ret;
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GError) error_report = NULL;

	/* fall back for CLI compatibility */
	if (g_strv_length(values) >= 1) {
		if (g_file_test(values[0], G_FILE_TEST_EXISTS) || fu_util_is_url(values[0]))
			return fu_util_local_install(self, values, error);
	}

	/* find device */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_SUPPORTED;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	/* find release */
	if (g_strv_length(values) >= 2) {
		rel = fu_util_get_release_for_device_version(self, dev, values[1], error);
		if (rel == NULL)
			return FALSE;
	} else {
		g_autoptr(GPtrArray) rels = NULL;
		rels = fwupd_client_get_releases(self->client,
						 fwupd_device_get_id(dev),
						 self->cancellable,
						 error);
		if (rels == NULL)
			return FALSE;
		rel = fu_util_prompt_for_release(self, rels, error);
		if (rel == NULL)
			return FALSE;
	}

	/* allow all actions */
	self->current_operation = FU_UTIL_OPERATION_INSTALL;
	ret = fu_util_update_device_with_release(self, dev, rel, error);
	if (ret)
		fu_util_display_current_message(self);

	/* send report if we're supposed to */
	if (!fu_util_maybe_send_reports(self, rel, &error_report)) {
		/* install failed, report failed */
		if (!ret) {
			g_warning("%s", error_report->message);
			/* install succeeded, but report failed */
		} else {
			g_propagate_error(error, g_steal_pointer(&error_report));
			return FALSE;
		}
	}

	/* install failed */
	if (!ret)
		return FALSE;

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
_g_str_equal0(gconstpointer str1, gconstpointer str2)
{
	return g_strcmp0(str1, str2) == 0;
}

static gboolean
fu_util_switch_branch(FuUtil *self, gchar **values, GError **error)
{
	const gchar *branch;
	gboolean ret;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;
	g_autoptr(GPtrArray) branches = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(GError) error_report = NULL;

	/* find the device and check it has multiple branches */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES;
	self->filter_device_include |= FWUPD_DEVICE_FLAG_UPDATABLE;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;

	/* get all releases, including the alternate branch versions */
	rels = fwupd_client_get_releases(self->client,
					 fwupd_device_get_id(dev),
					 self->cancellable,
					 error);
	if (rels == NULL)
		return FALSE;

	/* get all the unique branches */
	for (guint i = 0; i < rels->len; i++) {
		FwupdRelease *rel_tmp = g_ptr_array_index(rels, i);
		const gchar *branch_tmp = fwupd_release_get_branch(rel_tmp);
		if (!fwupd_release_match_flags(rel_tmp,
					       self->filter_release_include,
					       self->filter_release_exclude))
			continue;
		if (g_ptr_array_find_with_equal_func(branches, branch_tmp, _g_str_equal0, NULL))
			continue;
		g_ptr_array_add(branches, g_strdup(branch_tmp));
	}

	/* branch name is optional */
	if (g_strv_length(values) > 1) {
		branch = values[1];
	} else if (branches->len == 1) {
		branch = g_ptr_array_index(branches, 0);
	} else {
		guint idx;

		/* TRANSLATORS: this is to abort the interactive prompt */
		fu_console_print(self->console, "0.\t%s", _("Cancel"));
		for (guint i = 0; i < branches->len; i++) {
			const gchar *branch_tmp = g_ptr_array_index(branches, i);
			fu_console_print(self->console,
					 "%u.\t%s",
					 i + 1,
					 fu_util_branch_for_display(branch_tmp));
		}
		/* TRANSLATORS: get interactive prompt, where branch is the
		 * supplier of the firmware, e.g. "non-free" or "free" */
		idx = fu_console_input_uint(self->console, branches->len, "%s", _("Choose branch"));
		if (idx == 0) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOTHING_TO_DO,
					    "Request canceled");
			return FALSE;
		}
		branch = g_ptr_array_index(branches, idx - 1);
	}

	/* sanity check */
	if (g_strcmp0(branch, fwupd_device_get_branch(dev)) == 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "Device %s is already on branch %s",
			    fwupd_device_get_name(dev),
			    fu_util_branch_for_display(branch));
		return FALSE;
	}

	/* the releases are ordered by version */
	for (guint j = 0; j < rels->len; j++) {
		FwupdRelease *rel_tmp = g_ptr_array_index(rels, j);
		if (g_strcmp0(fwupd_release_get_branch(rel_tmp), branch) == 0) {
			rel = g_object_ref(rel_tmp);
			break;
		}
	}
	if (rel == NULL) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "No releases for branch %s",
			    fu_util_branch_for_display(branch));
		return FALSE;
	}

	/* we're switching branch */
	if (!fu_util_switch_branch_warning(self->console, dev, rel, self->assume_yes, error))
		return FALSE;

	/* update the console if composite devices are also updated */
	self->current_operation = FU_UTIL_OPERATION_INSTALL;
	self->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	self->flags |= FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH;
	ret = fu_util_update_device_with_release(self, dev, rel, error);
	if (ret)
		fu_util_display_current_message(self);

	/* send report if we're supposed to */
	if (!fu_util_maybe_send_reports(self, rel, &error_report)) {
		/* install failed, report failed */
		if (!ret) {
			g_warning("%s", error_report->message);
			/* install succeeded, but report failed */
		} else {
			g_propagate_error(error, g_steal_pointer(&error_report));
			return FALSE;
		}
	}

	/* install failed */
	if (!ret)
		return FALSE;

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_activate(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	gboolean has_pending = FALSE;

	/* handle both forms */
	if (g_strv_length(values) == 0) {
		/* activate anything with _NEEDS_ACTIVATION */
		devices = fwupd_client_get_devices(self->client, self->cancellable, error);
		if (devices == NULL)
			return FALSE;
		for (guint i = 0; i < devices->len; i++) {
			FwupdDevice *device = g_ptr_array_index(devices, i);
			if (fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)) {
				has_pending = TRUE;
				break;
			}
		}
	} else if (g_strv_length(values) == 1) {
		FwupdDevice *device = fu_util_get_device_by_id(self, values[0], error);
		if (device == NULL)
			return FALSE;
		devices = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
		g_ptr_array_add(devices, device);
		if (fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION))
			has_pending = TRUE;
	} else {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* nothing to do */
	if (!has_pending) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "No firmware to activate");
		return FALSE;
	}

	/* activate anything with _NEEDS_ACTIVATION */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *device = g_ptr_array_index(devices, i);
		if (!fwupd_device_match_flags(device,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		if (!fwupd_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION))
			continue;
		fu_console_print(
		    self->console,
		    "%s %s…",
		    /* TRANSLATORS: shown when shutting down to switch to the new version */
		    _("Activating firmware update for"),
		    fwupd_device_get_name(device));
		if (!fwupd_client_activate(self->client,
					   self->cancellable,
					   fwupd_device_get_id(device),
					   error))
			return FALSE;
	}

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: success message -- where activation is making the new
	 * firmware take effect, usually after updating offline */
	fu_console_print_literal(self->console, _("Successfully activated all devices"));
	return TRUE;
}

static gboolean
fu_util_set_approved_firmware(FuUtil *self, gchar **values, GError **error)
{
	g_auto(GStrv) checksums = NULL;

	/* check args */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: filename or list of checksums expected");
		return FALSE;
	}

	/* filename */
	if (g_file_test(values[0], G_FILE_TEST_EXISTS)) {
		g_autofree gchar *data = NULL;
		if (!g_file_get_contents(values[0], &data, NULL, error))
			return FALSE;
		checksums = g_strsplit(data, "\n", -1);
	} else {
		checksums = g_strsplit(values[0], ",", -1);
	}

	/* call into daemon */
	return fwupd_client_set_approved_firmware(self->client,
						  checksums,
						  self->cancellable,
						  error);
}

static gboolean
fu_util_get_checksums_as_json(FuUtil *self, gchar **csums, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "Checksums");
	json_builder_begin_array(builder);
	for (guint i = 0; csums[i] != NULL; i++)
		json_builder_add_string_value(builder, csums[i]);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return fu_util_print_builder(self->console, builder, error);
}

static gboolean
fu_util_get_approved_firmware(FuUtil *self, gchar **values, GError **error)
{
	g_auto(GStrv) checksums = NULL;

	/* check args */
	if (g_strv_length(values) != 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: none expected");
		return FALSE;
	}

	/* call into daemon */
	checksums = fwupd_client_get_approved_firmware(self->client, self->cancellable, error);
	if (checksums == NULL)
		return FALSE;
	if (self->as_json)
		return fu_util_get_checksums_as_json(self, checksums, error);
	if (g_strv_length(checksums) == 0) {
		/* TRANSLATORS: approved firmware has been checked by
		 * the domain administrator */
		fu_console_print_literal(self->console, _("There is no approved firmware."));
	} else {
		fu_console_print_literal(
		    self->console,
		    /* TRANSLATORS: approved firmware has been checked by
		     * the domain administrator */
		    ngettext("Approved firmware:", "Approved firmware:", g_strv_length(checksums)));
		for (guint i = 0; checksums[i] != NULL; i++)
			fu_console_print(self->console, " * %s", checksums[i]);
	}
	return TRUE;
}

static gboolean
fu_util_modify_config(FuUtil *self, gchar **values, GError **error)
{
	/* check args */
	if (g_strv_length(values) == 3) {
		if (!fwupd_client_modify_config(self->client,
						values[0],
						values[1],
						values[2],
						self->cancellable,
						error))
			return FALSE;
	} else if (g_strv_length(values) == 2) {
		if (!fwupd_client_modify_config(self->client,
						"fwupd",
						values[0],
						values[1],
						self->cancellable,
						error))
			return FALSE;
	} else {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: [SECTION] KEY VALUE expected");
		return FALSE;
	}

	if (self->as_json)
		return TRUE;

	if (!self->assume_yes) {
		if (!fu_console_input_bool(self->console,
					   FALSE,
					   "%s",
					   /* TRANSLATORS: changes only take effect on restart */
					   _("Restart the daemon to make the change effective?")))
			return TRUE;
	}

	if (!fu_util_quit(self, NULL, error))
		return FALSE;
	if (!fwupd_client_connect(self->client, self->cancellable, error))
		return FALSE;

	/* TRANSLATORS: success message -- a per-system setting value */
	fu_console_print_literal(self->console, _("Successfully modified configuration value"));
	return TRUE;
}

static gboolean
fu_util_reset_config(FuUtil *self, gchar **values, GError **error)
{
	/* check args */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: SECTION expected");
		return FALSE;
	}
	if (!fwupd_client_reset_config(self->client, values[0], self->cancellable, error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	if (!self->assume_yes) {
		if (!fu_console_input_bool(self->console,
					   FALSE,
					   "%s",
					   /* TRANSLATORS: changes only take effect on restart */
					   _("Restart the daemon to make the change effective?")))
			return TRUE;
	}
	if (!fu_util_quit(self, NULL, error))
		return FALSE;
	if (!fwupd_client_connect(self->client, self->cancellable, error))
		return FALSE;

	/* TRANSLATORS: success message -- a per-system setting value */
	fu_console_print_literal(self->console, _("Successfully reset configuration values"));
	return TRUE;
}

static FwupdRemote *
fu_util_get_remote_with_report_uri(FuUtil *self, GError **error)
{
	g_autoptr(GPtrArray) remotes = NULL;

	/* get all remotes */
	remotes = fwupd_client_get_remotes(self->client, self->cancellable, error);
	if (remotes == NULL)
		return NULL;

	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index(remotes, i);
		if (!fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_ENABLED))
			continue;
		if (fwupd_remote_get_report_uri(remote) != NULL)
			return g_object_ref(remote);
	}

	/* failed */
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "No remotes specified ReportURI");
	return NULL;
}

static gboolean
fu_util_upload_security(FuUtil *self, GPtrArray *attrs, GError **error)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *report_uri = NULL;
	g_autofree gchar *sig = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(FwupdRemote) remote = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GHashTable) metadata = NULL;

	/* can we find a remote with a security attr */
	remote = fu_util_get_remote_with_report_uri(self, &error_local);
	if (remote == NULL) {
		g_debug("failed to find suitable remote: %s", error_local->message);
		return TRUE;
	}

	/* export as a string */
	metadata = fwupd_client_get_report_metadata(self->client, self->cancellable, error);
	if (metadata == NULL)
		return FALSE;
	data = fwupd_client_build_report_security(self->client, attrs, metadata, error);
	if (data == NULL)
		return FALSE;

	/* ask for permission */
	if (!self->assume_yes &&
	    !fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_AUTOMATIC_SECURITY_REPORTS)) {
		if (!fu_console_input_bool(self->console,
					   FALSE,
					   /* TRANSLATORS: ask the user to share, %s is something
					    * like: "Linux Vendor Firmware Service" */
					   _("Upload these anonymous results to the %s to help "
					     "other users?"),
					   fwupd_remote_get_title(remote))) {
			return TRUE;
		}
	}

	/* self sign data */
	if (self->sign) {
		sig = fwupd_client_self_sign(self->client,
					     data,
					     FWUPD_SELF_SIGN_FLAG_ADD_TIMESTAMP,
					     self->cancellable,
					     error);
		if (sig == NULL)
			return FALSE;
	}

	/* ask for permission */
	if (!self->assume_yes &&
	    !fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_AUTOMATIC_SECURITY_REPORTS)) {
		fu_console_print_kv(self->console,
				    _("Target"),
				    fwupd_remote_get_report_uri(remote));
		fu_console_print_kv(self->console, _("Payload"), data);
		if (sig != NULL)
			fu_console_print_kv(self->console, _("Signature"), sig);
		if (!fu_console_input_bool(self->console, TRUE, "%s", _("Proceed with upload?"))) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_PERMISSION_DENIED,
					    "User declined action");
			return FALSE;
		}
	}

	/* POST request */
	report_uri = fwupd_remote_build_report_uri(remote, error);
	if (report_uri == NULL)
		return FALSE;
	uri = fwupd_client_upload_report(self->client,
					 report_uri,
					 data,
					 sig,
					 FWUPD_CLIENT_UPLOAD_FLAG_ALWAYS_MULTIPART,
					 self->cancellable,
					 error);
	if (uri == NULL)
		return FALSE;
	fu_console_print_literal(self->console,
				 /* TRANSLATORS: success, so say thank you to the user */
				 _("Host Security ID attributes uploaded successfully, thanks!"));

	/* as this worked, ask if the user want to do this every time */
	if (!fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_AUTOMATIC_SECURITY_REPORTS)) {
		if (fu_console_input_bool(self->console,
					  FALSE,
					  "%s",
					  /* TRANSLATORS: can we JFDI? */
					  _("Automatically upload every time?"))) {
			if (!fwupd_client_modify_remote(self->client,
							fwupd_remote_get_id(remote),
							"AutomaticSecurityReports",
							"true",
							self->cancellable,
							error))
				return FALSE;
		}
	}

	return TRUE;
}

static gboolean
fu_util_security_as_json(FuUtil *self,
			 GPtrArray *attrs,
			 GPtrArray *events,
			 GPtrArray *devices,
			 GError **error)
{
	g_autoptr(GPtrArray) devices_issues = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);

	/* attrs */
	fwupd_codec_array_to_json(attrs, "SecurityAttributes", builder, FWUPD_CODEC_FLAG_TRUSTED);

	/* events */
	if (events != NULL && events->len > 0) {
		fwupd_codec_array_to_json(events,
					  "SecurityEvents",
					  builder,
					  FWUPD_CODEC_FLAG_TRUSTED);
	}

	/* devices */
	devices_issues = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	for (guint i = 0; devices != NULL && i < devices->len; i++) {
		FwupdDevice *device = g_ptr_array_index(devices, i);
		GPtrArray *issues = fwupd_device_get_issues(device);
		if (issues->len == 0)
			continue;
		g_ptr_array_add(devices_issues, g_object_ref(device));
	}
	if (devices_issues->len > 0) {
		fwupd_codec_array_to_json(devices_issues,
					  "Devices",
					  builder,
					  FWUPD_CODEC_FLAG_TRUSTED);
	}

	json_builder_end_object(builder);
	return fu_util_print_builder(self->console, builder, error);
}

static gboolean
fu_util_sync(FuUtil *self, gchar **values, GError **error)
{
	const gchar *host_bkc = fwupd_client_get_host_bkc(self->client);
	guint cnt = 0;
	g_autoptr(GPtrArray) devices = NULL;

	/* update the console if composite devices are also updated */
	self->current_operation = FU_UTIL_OPERATION_INSTALL;
	self->flags |= FWUPD_INSTALL_FLAG_ALLOW_OLDER;

	devices = fwupd_client_get_devices(self->client, NULL, error);
	if (devices == NULL)
		return FALSE;
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		g_autoptr(FwupdRelease) rel = NULL;
		g_autoptr(GError) error_local = NULL;

		/* find the release that matches the tag */
		if (host_bkc != NULL) {
			rel = fu_util_get_release_with_tag(self, dev, host_bkc, &error_local);
		} else if (fu_device_get_branch(dev) != NULL) {
			rel = fu_util_get_release_with_branch(self,
							      dev,
							      fu_device_get_branch(dev),
							      &error_local);
		} else {
			g_set_error_literal(&error_local,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "No device branch or system HostBkc set");
			/* nocheck:error-false-return */
		}
		if (rel == NULL) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED) ||
			    g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
				g_debug("ignoring %s: %s",
					fwupd_device_get_id(dev),
					error_local->message);
				continue;
			}
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}

		/* ignore if already on that release */
		if (g_strcmp0(fwupd_device_get_version(dev), fwupd_release_get_version(rel)) == 0)
			continue;

		/* install this new release */
		g_debug("need to move %s from %s to %s",
			fwupd_device_get_id(dev),
			fwupd_device_get_version(dev),
			fwupd_release_get_version(rel));
		if (!fu_util_update_device_with_release(self, dev, rel, &error_local)) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
				g_debug("ignoring %s: %s",
					fwupd_device_get_id(dev),
					error_local->message);
				continue;
			}
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}
		fu_util_display_current_message(self);
		cnt++;
	}

	/* nothing was done */
	if (cnt == 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOTHING_TO_DO,
			    "No devices required modification");
		return FALSE;
	}

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	/* show reboot if needed */
	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_security_fix_attr(FuUtil *self, FwupdSecurityAttr *attr, GError **error)
{
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GString) title = g_string_new(NULL);

	g_string_append_printf(title,
			       "%s: %s",
			       /* TRANSLATORS: title prefix for the BIOS settings dialog */
			       _("Configuration Change Suggested"),
			       fwupd_security_attr_get_title(attr));

	g_string_append(body, fwupd_security_attr_get_description(attr));

	if (fwupd_security_attr_get_bios_setting_id(attr) != NULL &&
	    fwupd_security_attr_get_bios_setting_current_value(attr) != NULL &&
	    fwupd_security_attr_get_bios_setting_target_value(attr) != NULL) {
		g_string_append(body, "\n\n");
		g_string_append_printf(body,
				       /* TRANSLATORS: the %1 is a BIOS setting name.
					* %2 and %3 are the values, e.g. "True" or "Windows10" */
				       _("This tool can change the BIOS setting '%s' from '%s' "
					 "to '%s' automatically, but it will only be active after "
					 "restarting the computer."),
				       fwupd_security_attr_get_bios_setting_id(attr),
				       fwupd_security_attr_get_bios_setting_current_value(attr),
				       fwupd_security_attr_get_bios_setting_target_value(attr));
		g_string_append(body, "\n\n");
		g_string_append(body,
				/* TRANSLATORS: the user has to manually recover; we can't do it */
				_("You should ensure you are comfortable restoring the setting "
				  "from the system firmware setup, as this change may cause the "
				  "system to not boot into Linux or cause other system "
				  "instability."));
	} else if (fwupd_security_attr_get_kernel_target_value(attr) != NULL) {
		g_string_append(body, "\n\n");
		if (fwupd_security_attr_get_kernel_current_value(attr) != NULL) {
			g_string_append_printf(
			    body,
			    /* TRANSLATORS: the %1 is a kernel command line key=value */
			    _("This tool can change the kernel argument from '%s' to '%s', but "
			      "it will only be active after restarting the computer."),
			    fwupd_security_attr_get_kernel_current_value(attr),
			    fwupd_security_attr_get_kernel_target_value(attr));
		} else {
			g_string_append_printf(
			    body,
			    /* TRANSLATORS: the %1 is a kernel command line key=value */
			    _("This tool can add a kernel argument of '%s', but it will "
			      "only be active after restarting the computer."),
			    fwupd_security_attr_get_kernel_target_value(attr));
		}
		g_string_append(body, "\n\n");
		g_string_append(body,
				/* TRANSLATORS: the user has to manually recover; we can't do it */
				_("You should ensure you are comfortable restoring the setting "
				  "from a recovery or installation disk, as this change may cause "
				  "the system to not boot into Linux or cause other system "
				  "instability."));
	}

	fu_console_box(self->console, title->str, body->str, 80);

	/* TRANSLATORS: prompt to apply the update */
	if (!fu_console_input_bool(self->console, FALSE, "%s", _("Perform operation?")))
		return TRUE;
	if (!fwupd_client_fix_host_security_attr(self->client,
						 fwupd_security_attr_get_appstream_id(attr),
						 self->cancellable,
						 error))
		return FALSE;

	/* do not offer to upload the report */
	self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_REBOOT;
	return TRUE;
}

static gboolean
fu_util_security(FuUtil *self, gchar **values, GError **error)
{
	FuSecurityAttrToStringFlags flags = FU_SECURITY_ATTR_TO_STRING_FLAG_NONE;
	g_autoptr(GPtrArray) attrs = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autofree gchar *str = NULL;

#ifndef HAVE_HSI
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_SUPPORTED,
		    /* TRANSLATORS: error message for unsupported feature */
		    _("Host Security ID (HSI) is not supported"));
	return FALSE;
#endif /* HAVE_HSI */

	/* the "why" */
	attrs = fwupd_client_get_host_security_attrs(self->client, self->cancellable, error);
	if (attrs == NULL)
		return FALSE;

	/* the "when" */
	events = fwupd_client_get_host_security_events(self->client,
						       10,
						       self->cancellable,
						       &error_local);
	if (events == NULL) {
		if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
			g_debug("ignoring failed events: %s", error_local->message);
		} else {
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}
	}

	/* the "also" */
	devices = fwupd_client_get_devices(self->client, self->cancellable, &error_local);
	if (devices == NULL) {
		if (!g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}
	}

	/* not for human consumption */
	if (self->as_json)
		return fu_util_security_as_json(self, attrs, events, devices, error);

	fu_console_print(self->console,
			 "%s \033[1m%s\033[0m",
			 /* TRANSLATORS: this is a string like 'HSI:2-U' */
			 _("Host Security ID:"),
			 fwupd_client_get_host_security_id(self->client));

	/* show or hide different elements */
	if (self->show_all) {
		flags |= FU_SECURITY_ATTR_TO_STRING_FLAG_SHOW_OBSOLETES;
		flags |= FU_SECURITY_ATTR_TO_STRING_FLAG_SHOW_URLS;
	}
	str = fu_util_security_attrs_to_string(attrs, flags);
	fu_console_print_literal(self->console, str);

	/* events */
	if (events != NULL && events->len > 0) {
		g_autofree gchar *estr = fu_util_security_events_to_string(events, flags);
		if (estr != NULL)
			fu_console_print_literal(self->console, estr);
	}

	/* known CVEs */
	if (devices != NULL && devices->len > 0) {
		g_autofree gchar *estr = fu_util_security_issues_to_string(devices);
		if (estr != NULL)
			fu_console_print_literal(self->console, estr);
	}

	/* host emulation */
	for (guint j = 0; j < attrs->len; j++) {
		FwupdSecurityAttr *attr = g_ptr_array_index(attrs, j);
		if (g_strcmp0(fwupd_security_attr_get_appstream_id(attr),
			      FWUPD_SECURITY_ATTR_ID_HOST_EMULATION) == 0) {
			self->no_unreported_check = TRUE;
			break;
		}
	}

	/* any things we can fix? */
	if (!self->no_security_fix) {
		for (guint j = 0; j < attrs->len; j++) {
			FwupdSecurityAttr *attr = g_ptr_array_index(attrs, j);
			if (fwupd_security_attr_has_flag(attr, FWUPD_SECURITY_ATTR_FLAG_CAN_FIX) &&
			    !fwupd_security_attr_has_flag(attr, FWUPD_SECURITY_ATTR_FLAG_SUCCESS)) {
				if (!fu_util_security_fix_attr(self, attr, error))
					return FALSE;
			}
		}
	}

	/* upload, with confirmation */
	if (!self->no_unreported_check) {
		if (!fu_util_upload_security(self, attrs, error))
			return FALSE;
	}

	/* reboot is required? */
	if (!self->no_reboot_check &&
	    (self->completion_flags & FWUPD_DEVICE_FLAG_NEEDS_REBOOT) > 0) {
		if (!fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error))
			return FALSE;
	}

	/* success */
	return TRUE;
}

static void
fu_util_ignore_cb(const gchar *log_domain,
		  GLogLevelFlags log_level,
		  const gchar *message,
		  gpointer user_data)
{
}

#ifdef HAVE_GIO_UNIX
static gboolean
fu_util_sigint_cb(gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;
	g_debug("Handling SIGINT");
	g_cancellable_cancel(self->cancellable);
	return FALSE;
}
#endif

static void
fu_util_setup_signal_handlers(FuUtil *self)
{
#ifdef HAVE_GIO_UNIX
	g_autoptr(GSource) source = g_unix_signal_source_new(SIGINT);
	g_source_set_callback(source, fu_util_sigint_cb, self, NULL);
	g_source_attach(g_steal_pointer(&source), self->main_ctx);
#endif
}

static void
fu_util_private_free(FuUtil *self)
{
	if (self->client != NULL) {
		/* when destroying GDBusProxy in a custom GMainContext, the context must be
		 * iterated enough after finalization of the proxies that any pending D-Bus traffic
		 * can be freed */
		fwupd_client_disconnect(self->client, NULL);
		while (g_main_context_iteration(self->main_ctx, FALSE)) {
			/* nothing needs to be done here */
		};
		g_object_unref(self->client);
	}
	if (self->current_device != NULL)
		g_object_unref(self->current_device);
	g_ptr_array_unref(self->post_requests);
	g_main_loop_unref(self->loop);
	g_main_context_unref(self->main_ctx);
	g_object_unref(self->cancellable);
	g_object_unref(self->console);
	g_option_context_free(self->context);
	g_free(self);
}

static gboolean
fu_util_check_daemon_version(FuUtil *self, GError **error)
{
	const gchar *daemon = fwupd_client_get_daemon_version(self->client);

	if (daemon == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    /* TRANSLATORS: error message */
				    _("Unable to connect to service"));
		return FALSE;
	}

	if (g_strcmp0(daemon, PACKAGE_VERSION) != 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    /* TRANSLATORS: error message */
			    _("Unsupported daemon version %s, client version is %s"),
			    daemon,
			    PACKAGE_VERSION);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_util_check_polkit_actions(GError **error)
{
#ifdef HAVE_POLKIT
	g_autofree gchar *directory = NULL;
	g_autofree gchar *filename = NULL;

	if (g_getenv("FWUPD_POLKIT_NOCHECK") != NULL)
		return TRUE;

	directory = fu_path_from_kind(FU_PATH_KIND_POLKIT_ACTIONS);
	filename = g_build_filename(directory, "org.freedesktop.fwupd.policy", NULL);
	if (!g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
		g_set_error_literal(
		    error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_AUTH_FAILED,
		    "PolicyKit files are missing, see "
		    "https://github.com/fwupd/fwupd/wiki/PolicyKit-files-are-missing");
		return FALSE;
	}
#endif

	return TRUE;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuUtil, fu_util_private_free)
#pragma clang diagnostic pop

static gchar *
fu_util_get_history_checksum(FuUtil *self, GError **error)
{
	const gchar *csum;
	g_autoptr(FwupdDevice) device = NULL;
	g_autoptr(FwupdRelease) release = NULL;
	g_autoptr(GPtrArray) devices = NULL;

	devices = fwupd_client_get_history(self->client, self->cancellable, error);
	if (devices == NULL)
		return NULL;
	device = fu_util_prompt_for_device(self, devices, error);
	if (device == NULL)
		return NULL;
	release = fu_util_prompt_for_release(self, fwupd_device_get_releases(device), error);
	if (release == NULL)
		return NULL;
	csum = fwupd_checksum_get_best(fwupd_release_get_checksums(release));
	if (csum == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "No suitable checksums");
		return NULL;
	}
	return g_strdup(csum);
}

static gboolean
fu_util_block_firmware(FuUtil *self, gchar **values, GError **error)
{
	guint idx = 0;
	g_autofree gchar *csum = NULL;
	g_auto(GStrv) csums_new = NULL;
	g_auto(GStrv) csums = NULL;

	/* get existing checksums */
	csums = fwupd_client_get_blocked_firmware(self->client, self->cancellable, error);
	if (csums == NULL)
		return FALSE;

	/* get new value */
	if (g_strv_length(values) == 0) {
		csum = fu_util_get_history_checksum(self, error);
		if (csum == NULL)
			return FALSE;
	} else {
		csum = g_strdup(values[0]);
	}

	/* ensure it's not already there */
	if (g_strv_contains((const gchar *const *)csums, csum)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: user selected something not possible */
				    _("Firmware is already blocked"));
		return FALSE;
	}

	/* TRANSLATORS: we will not offer this firmware to the user */
	fu_console_print(self->console, "%s %s", _("Blocking firmware:"), csum);

	/* remove it from the new list */
	csums_new = g_new0(gchar *, g_strv_length(csums) + 2);
	for (guint i = 0; csums[i] != NULL; i++) {
		if (g_strcmp0(csums[i], csum) != 0)
			csums_new[idx++] = g_strdup(csums[i]);
	}
	csums_new[idx] = g_strdup(csum);
	return fwupd_client_set_blocked_firmware(self->client, csums_new, self->cancellable, error);
}

static gboolean
fu_util_unblock_firmware(FuUtil *self, gchar **values, GError **error)
{
	guint idx = 0;
	g_auto(GStrv) csums = NULL;
	g_auto(GStrv) csums_new = NULL;
	g_autofree gchar *csum = NULL;

	/* get existing checksums */
	csums = fwupd_client_get_blocked_firmware(self->client, self->cancellable, error);
	if (csums == NULL)
		return FALSE;

	/* empty list */
	if (g_strv_length(csums) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: nothing to show */
				    _("There are no blocked firmware files"));
		return FALSE;
	}

	/* get new value */
	if (g_strv_length(values) == 0) {
		csum = fu_util_get_history_checksum(self, error);
		if (csum == NULL)
			return FALSE;
	} else {
		csum = g_strdup(values[0]);
	}

	/* ensure it's there */
	if (!g_strv_contains((const gchar *const *)csums, csum)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: user selected something not possible */
				    _("Firmware is not already blocked"));
		return FALSE;
	}

	/* TRANSLATORS: we will now offer this firmware to the user */
	fu_console_print(self->console, "%s %s", _("Unblocking firmware:"), csum);

	/* remove it from the new list */
	csums_new = g_new0(gchar *, g_strv_length(csums));
	for (guint i = 0; csums[i] != NULL; i++) {
		if (g_strcmp0(csums[i], csum) != 0)
			csums_new[idx++] = g_strdup(csums[i]);
	}
	return fwupd_client_set_blocked_firmware(self->client, csums_new, self->cancellable, error);
}

static gboolean
fu_util_get_blocked_firmware(FuUtil *self, gchar **values, GError **error)
{
	g_auto(GStrv) csums = NULL;

	/* get checksums */
	csums = fwupd_client_get_blocked_firmware(self->client, self->cancellable, error);
	if (csums == NULL)
		return FALSE;
	if (self->as_json)
		return fu_util_get_checksums_as_json(self, csums, error);

	/* empty list */
	if (g_strv_length(csums) == 0) {
		/* TRANSLATORS: nothing to show */
		fu_console_print_literal(self->console, _("There are no blocked firmware files"));
		return TRUE;
	}

	/* TRANSLATORS: there follows a list of hashes */
	fu_console_print_literal(self->console, _("Blocked firmware files:"));
	for (guint i = 0; csums[i] != NULL; i++) {
		fu_console_print(self->console, "%u.\t%s", i + 1, csums[i]);
	}

	/* success */
	return TRUE;
}

static void
fu_util_show_plugin_warnings(FuUtil *self)
{
	FwupdPluginFlags flags = FWUPD_PLUGIN_FLAG_NONE;
	g_autoptr(GPtrArray) plugins = NULL;

	if (self->as_json)
		return;

	/* get plugins from daemon, ignoring if the daemon is too old */
	plugins = fwupd_client_get_plugins(self->client, self->cancellable, NULL);
	if (plugins == NULL)
		return;

	/* get a superset so we do not show the same message more than once */
	for (guint i = 0; i < plugins->len; i++) {
		FwupdPlugin *plugin = g_ptr_array_index(plugins, i);
		if (fwupd_plugin_has_flag(plugin, FWUPD_PLUGIN_FLAG_DISABLED))
			continue;
		if (!fwupd_plugin_has_flag(plugin, FWUPD_PLUGIN_FLAG_USER_WARNING))
			continue;
		flags |= fwupd_plugin_get_flags(plugin);
	}

	/* never show these, they're way too generic */
	flags &= ~FWUPD_PLUGIN_FLAG_DISABLED;
	flags &= ~FWUPD_PLUGIN_FLAG_NO_HARDWARE;
	flags &= ~FWUPD_PLUGIN_FLAG_REQUIRE_HWID;
	flags &= ~FWUPD_PLUGIN_FLAG_MEASURE_SYSTEM_INTEGRITY;
	flags &= ~FWUPD_PLUGIN_FLAG_READY;

	/* print */
	for (guint i = 0; i < 64; i++) {
		FwupdPluginFlags flag = (guint64)1 << i;
		g_autofree gchar *tmp = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GString) str = g_string_new(NULL);
		if ((flags & flag) == 0)
			continue;
		tmp = fu_util_plugin_flag_to_string(flag);
		if (tmp == NULL)
			continue;
		g_string_append_printf(str, "%s\n", tmp);
		url = g_strdup_printf("https://github.com/fwupd/fwupd/wiki/PluginFlag:%s",
				      fwupd_plugin_flag_to_string(flag));
		/* TRANSLATORS: %s is a link to a website */
		g_string_append_printf(str, _("See %s for more information."), url);
		fu_console_print_full(self->console,
				      FU_CONSOLE_PRINT_FLAG_WARNING,
				      "%s\n",
				      str->str);
	}
}

static gboolean
fu_util_set_bios_setting(FuUtil *self, gchar **input, GError **error)
{
	g_autoptr(GHashTable) settings = fu_util_bios_settings_parse_argv(input, error);

	if (settings == NULL)
		return FALSE;

	if (!fwupd_client_modify_bios_setting(self->client, settings, self->cancellable, error)) {
		if (!g_error_matches(*error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO))
			g_prefix_error(error, "failed to set BIOS setting: ");
		return FALSE;
	}

	if (!self->as_json) {
		gpointer key, value;
		GHashTableIter iter;

		g_hash_table_iter_init(&iter, settings);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			g_autofree gchar *msg =
			    /* TRANSLATORS: Configured a BIOS setting to a value */
			    g_strdup_printf(_("Set BIOS setting '%s' using '%s'."),
					    (const gchar *)key,
					    (const gchar *)value);
			fu_console_print_literal(self->console, msg);
		}
	}
	self->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_REBOOT;

	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_get_bios_setting(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) attrs = NULL;
	gboolean found = FALSE;

	attrs = fwupd_client_get_bios_settings(self->client, self->cancellable, error);
	if (attrs == NULL)
		return FALSE;
	if (self->as_json)
		return fu_util_bios_setting_console_print(self->console, values, attrs, error);

	for (guint i = 0; i < attrs->len; i++) {
		FwupdBiosSetting *attr = g_ptr_array_index(attrs, i);
		if (fu_util_bios_setting_matches_args(attr, values)) {
			g_autofree gchar *tmp = fu_util_bios_setting_to_string(attr, 0);
			fu_console_print_literal(self->console, tmp);
			found = TRUE;
		}
	}
	if (attrs->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: error message */
				    _("This system doesn't support firmware settings"));
		return FALSE;
	}
	if (!found) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    /* TRANSLATORS: error message */
				    _("Unable to find attribute"));
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_util_security_fix(FuUtil *self, gchar **values, GError **error)
{
#ifndef HAVE_HSI
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_SUPPORTED,
		    /* TRANSLATORS: error message for unsupported feature */
		    _("Host Security ID (HSI) is not supported"));
	return FALSE;
#endif /* HAVE_HSI */

	if (g_strv_length(values) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    /* TRANSLATOR: This is the error message for
				     * incorrect parameter */
				    _("Invalid arguments, expected an AppStream ID"));
		return FALSE;
	}
	if (!fwupd_client_fix_host_security_attr(self->client, values[0], self->cancellable, error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: we've fixed a security problem on the machine */
	fu_console_print_literal(self->console, _("Fixed successfully"));
	return TRUE;
}

static gboolean
fu_util_report_devices(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *report_uri = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(FwupdRemote) remote = NULL;
	g_autoptr(GHashTable) metadata = NULL;
	g_autoptr(GPtrArray) devices = NULL;

	/* we only know how to upload to the LVFS */
	remote = fwupd_client_get_remote_by_id(self->client, "lvfs", self->cancellable, error);
	if (remote == NULL)
		return FALSE;
	report_uri = fwupd_remote_build_report_uri(remote, error);
	if (report_uri == NULL)
		return FALSE;

	/* include all the devices */
	devices = fwupd_client_get_devices(self->client, self->cancellable, error);
	if (devices == NULL)
		return FALSE;
	metadata = fwupd_client_get_report_metadata(self->client, self->cancellable, error);
	if (metadata == NULL)
		return FALSE;
	data = fwupd_client_build_report_devices(self->client, devices, metadata, error);
	if (data == NULL)
		return FALSE;

	if (self->as_json) {
		if (!self->assume_yes) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_ARGS,
					    "pass --yes to enable uploads");
			return FALSE;
		}
	} else {
		/* show the user the entire data blob */
		fu_console_print_kv(self->console, _("Target"), report_uri);
		fu_console_print_kv(self->console, _("Payload"), data);
		fu_console_print(
		    self->console,
		    /* TRANSLATORS: explain why we want to upload */
		    _("Uploading a device list allows the %s team to know what hardware "
		      "exists, and allows us to put pressure on vendors that do not upload "
		      "firmware updates for their hardware."),
		    fwupd_remote_get_title(remote));
		if (!fu_console_input_bool(self->console,
					   TRUE,
					   "%s (%s)",
					   /* TRANSLATORS: ask the user to upload */
					   _("Upload data now?"),
					   /* TRANSLATORS: metadata is downloaded */
					   _("Requires internet connection"))) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOTHING_TO_DO,
					    "Declined upload");
			return FALSE;
		}
	}

	/* send to the LVFS */
	uri = fwupd_client_upload_report(self->client,
					 report_uri,
					 data,
					 NULL,
					 FWUPD_CLIENT_UPLOAD_FLAG_ALWAYS_MULTIPART,
					 self->cancellable,
					 error);
	if (uri == NULL)
		return FALSE;

	/* success */
	if (!self->as_json) {
		fu_console_print_literal(self->console,
					 /* TRANSLATORS: success, so say thank you to the user */
					 _("Device list uploaded successfully, thanks!"));
	}

	return TRUE;
}

static gboolean
fu_util_security_undo(FuUtil *self, gchar **values, GError **error)
{
#ifndef HAVE_HSI
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_SUPPORTED,
		    /* TRANSLATORS: error message for unsupported feature */
		    _("Host Security ID (HSI) is not supported"));
	return FALSE;
#endif /* HAVE_HSI */

	if (g_strv_length(values) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    /* TRANSLATOR: This is the error message for
				     * incorrect parameter */
				    _("Invalid arguments, expected an AppStream ID"));
		return FALSE;
	}
	if (!fwupd_client_undo_host_security_attr(self->client,
						  values[0],
						  self->cancellable,
						  error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: we've fixed a security problem on the machine */
	fu_console_print_literal(self->console, _("Fix reverted successfully"));
	return TRUE;
}

static gboolean
fu_util_emulation_tag(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	/* set the flag */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_CAN_EMULATION_TAG;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;
	return fwupd_client_modify_device(self->client,
					  fwupd_device_get_id(dev),
					  "Flags",
					  "emulation-tag",
					  self->cancellable,
					  error);
}

static gboolean
fu_util_emulation_untag(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	/* set the flag */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_EMULATION_TAG;
	dev = fu_util_get_device_or_prompt(self, values, error);
	if (dev == NULL)
		return FALSE;
	return fwupd_client_modify_device(self->client,
					  fwupd_device_get_id(dev),
					  "Flags",
					  "~emulation-tag",
					  self->cancellable,
					  error);
}

static gboolean
fu_util_emulation_save(FuUtil *self, gchar **values, GError **error)
{
	/* check args */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments, expected FILENAME");
		return FALSE;
	}

	/* save */
	return fwupd_client_emulation_save(self->client, values[0], self->cancellable, error);
}

static gboolean
fu_util_emulation_load(FuUtil *self, gchar **values, GError **error)
{
	/* check args */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments, expected FILENAME");
		return FALSE;
	}
	return fwupd_client_emulation_load(self->client, values[0], self->cancellable, error);
}

static gboolean
fu_util_version(FuUtil *self, GError **error)
{
	g_autoptr(GHashTable) metadata = NULL;
	g_autofree gchar *str = NULL;

	/* get metadata */
	metadata = fwupd_client_get_report_metadata(self->client, self->cancellable, error);
	if (metadata == NULL)
		return FALSE;

	/* dump to the screen in the most appropriate format */
	if (self->as_json)
		return fu_util_project_versions_as_json(self->console, metadata, error);
	str = fu_util_project_versions_to_string(metadata);
	fu_console_print_literal(self->console, str);
	return TRUE;
}

static gboolean
fu_util_setup_interactive(FuUtil *self, GError **error)
{
	if (self->as_json) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "using --json");
		return FALSE;
	}
	return fu_console_setup(self->console, error);
}

static void
fu_util_cancelled_cb(GCancellable *cancellable, gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;
	if (!g_main_loop_is_running(self->loop))
		return;
	/* TRANSLATORS: this is from ctrl+c */
	fu_console_print_literal(self->console, _("Cancelled"));
	g_main_loop_quit(self->loop);
}

static void
fu_util_print_error(FuUtil *self, const GError *error)
{
	if (self->as_json) {
		fu_util_print_error_as_json(self->console, error);
		return;
	}
	fu_console_print_full(self->console, FU_CONSOLE_PRINT_FLAG_STDERR, "%s\n", error->message);
}

int
main(int argc, char *argv[])
{ /* nocheck:lines */
	gboolean force = FALSE;
	gboolean allow_branch_switch = FALSE;
	gboolean allow_older = FALSE;
	gboolean allow_reinstall = FALSE;
	gboolean only_emulated = FALSE;
	gboolean only_p2p = FALSE;
	gboolean is_interactive = FALSE;
	gboolean no_history = FALSE;
	gboolean no_authenticate = FALSE;
	gboolean ret;
	gboolean verbose = FALSE;
	gboolean version = FALSE;
	guint download_retries = 0;
	g_autoptr(FuUtil) self = g_new0(FuUtil, 1);
	g_autoptr(GDateTime) dt_now = g_date_time_new_now_utc();
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) error_console = NULL;
	g_autoptr(GPtrArray) cmd_array = fu_util_cmd_array_new();
#ifdef HAVE_POLKIT
	g_autoptr(FuPolkitAgent) polkit_agent = fu_polkit_agent_new();
#endif
	g_autofree gchar *cmd_descriptions = NULL;
	g_autofree gchar *filter_device = NULL;
	g_autofree gchar *filter_release = NULL;
	const GOptionEntry options[] = {
	    {"verbose",
	     'v',
	     0,
	     G_OPTION_ARG_NONE,
	     &verbose,
	     /* TRANSLATORS: command line option */
	     N_("Show extra debugging information"),
	     NULL},
	    {"version",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &version,
	     /* TRANSLATORS: command line option */
	     N_("Show client and daemon versions"),
	     NULL},
	    {"download-retries",
	     '\0',
	     0,
	     G_OPTION_ARG_INT,
	     &download_retries,
	     /* TRANSLATORS: command line option */
	     N_("Set the download retries for transient errors"),
	     NULL},
	    {"allow-reinstall",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &allow_reinstall,
	     /* TRANSLATORS: command line option */
	     N_("Allow reinstalling existing firmware versions"),
	     NULL},
	    {"allow-older",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &allow_older,
	     /* TRANSLATORS: command line option */
	     N_("Allow downgrading firmware versions"),
	     NULL},
	    {"allow-branch-switch",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &allow_branch_switch,
	     /* TRANSLATORS: command line option */
	     N_("Allow switching firmware branch"),
	     NULL},
	    {"only-emulated",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &only_emulated,
	     /* TRANSLATORS: command line option */
	     N_("Only install onto emulated devices"),
	     NULL},
	    {"force",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &force,
	     /* TRANSLATORS: command line option */
	     N_("Force the action by relaxing some runtime checks"),
	     NULL},
	    {"assume-yes",
	     'y',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->assume_yes,
	     /* TRANSLATORS: command line option */
	     N_("Answer yes to all questions"),
	     NULL},
	    {"sign",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->sign,
	     /* TRANSLATORS: command line option */
	     N_("Sign the uploaded data with the client certificate"),
	     NULL},
	    {"no-unreported-check",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->no_unreported_check,
	     /* TRANSLATORS: command line option */
	     N_("Do not check for unreported history"),
	     NULL},
	    {"no-metadata-check",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->no_metadata_check,
	     /* TRANSLATORS: command line option */
	     N_("Do not check for old metadata"),
	     NULL},
	    {"no-remote-check",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->no_remote_check,
	     /* TRANSLATORS: command line option */
	     N_("Do not check if download remotes should be enabled"),
	     NULL},
	    {"no-reboot-check",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->no_reboot_check,
	     /* TRANSLATORS: command line option */
	     N_("Do not check or prompt for reboot after update"),
	     NULL},
	    {"no-safety-check",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->no_safety_check,
	     /* TRANSLATORS: command line option */
	     N_("Do not perform device safety checks"),
	     NULL},
	    {"no-device-prompt",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->no_device_prompt,
	     /* TRANSLATORS: command line option */
	     N_("Do not prompt for devices"),
	     NULL},
	    {"no-history",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &no_history,
	     /* TRANSLATORS: command line option */
	     N_("Do not write to the history database"),
	     NULL},
	    {"show-all",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->show_all,
	     /* TRANSLATORS: command line option */
	     N_("Show all results"),
	     NULL},
	    {"show-all-devices",
	     '\0',
	     G_OPTION_FLAG_HIDDEN,
	     G_OPTION_ARG_NONE,
	     &self->show_all,
	     /* TRANSLATORS: command line option */
	     N_("Show devices that are not updatable"),
	     NULL},
	    {"disable-ssl-strict",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->disable_ssl_strict,
	     /* TRANSLATORS: command line option */
	     N_("Ignore SSL strict checks when downloading files"),
	     NULL},
	    {"p2p",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &only_p2p,
	     /* TRANSLATORS: command line option */
	     N_("Only use peer-to-peer networking when downloading files"),
	     NULL},
	    {"filter",
	     '\0',
	     0,
	     G_OPTION_ARG_STRING,
	     &filter_device,
	     /* TRANSLATORS: command line option */
	     N_("Filter with a set of device flags using a ~ prefix to "
		"exclude, e.g. 'internal,~needs-reboot'"),
	     NULL},
	    {"filter-release",
	     '\0',
	     0,
	     G_OPTION_ARG_STRING,
	     &filter_release,
	     /* TRANSLATORS: command line option */
	     N_("Filter with a set of release flags using a ~ prefix to "
		"exclude, e.g. 'trusted-release,~trusted-metadata'"),
	     NULL},
	    {"json",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->as_json,
	     /* TRANSLATORS: command line option */
	     N_("Output in JSON format (disables all interactive prompts)"),
	     NULL},
	    {"no-security-fix",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->no_security_fix,
	     /* TRANSLATORS: command line option */
	     N_("Do not prompt to fix security issues"),
	     NULL},
	    {"no-authenticate",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &no_authenticate,
	     /* TRANSLATORS: command line option */
	     N_("Don't prompt for authentication (less details may be shown)"),
	     NULL},
	    {NULL}};
	FwupdFeatureFlags feature_flags =
	    FWUPD_FEATURE_FLAG_CAN_REPORT | FWUPD_FEATURE_FLAG_SWITCH_BRANCH |
	    FWUPD_FEATURE_FLAG_FDE_WARNING | FWUPD_FEATURE_FLAG_COMMUNITY_TEXT |
	    FWUPD_FEATURE_FLAG_SHOW_PROBLEMS;

#ifdef _WIN32
	/* workaround Windows setting the codepage to 1252 */
	(void)g_setenv("LANG", "C.UTF-8", FALSE);
#endif

	setlocale(LC_ALL, "");

	bindtextdomain(GETTEXT_PACKAGE, FWUPD_LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
	textdomain(GETTEXT_PACKAGE);
	g_set_prgname(fu_util_get_prgname(argv[0]));

	/* ensure D-Bus errors are registered */
	(void)fwupd_error_quark();

	/* create helper object */
	self->main_ctx = g_main_context_new();
	self->loop = g_main_loop_new(self->main_ctx, FALSE);
	self->console = fu_console_new();
	self->post_requests = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	fu_console_set_main_context(self->console, self->main_ctx);

	/* add commands */
	fu_util_cmd_array_add(cmd_array,
			      "check-reboot-needed",
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Check if any devices are pending a reboot to complete update"),
			      fu_util_check_reboot_needed);
	fu_util_cmd_array_add(cmd_array,
			      "get-devices,get-topology",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Get all devices that support firmware updates"),
			      fu_util_get_devices);
	fu_util_cmd_array_add(cmd_array,
			      "get-history",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Show history of firmware updates"),
			      fu_util_get_history);
	fu_util_cmd_array_add(cmd_array,
			      "report-history",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Share firmware history with the developers"),
			      fu_util_report_history);
	fu_util_cmd_array_add(cmd_array,
			      "report-export",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Export firmware history for manual upload"),
			      fu_util_report_export);
	fu_util_cmd_array_add(cmd_array,
			      "install",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID] [VERSION]"),
			      /* TRANSLATORS: command description */
			      _("Install a specific firmware file on all devices that match"),
			      fu_util_install);
	fu_util_cmd_array_add(cmd_array,
			      "local-install",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILE [DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Install a firmware file in cabinet format on this hardware"),
			      fu_util_local_install);
	fu_util_cmd_array_add(cmd_array,
			      "get-details",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILE"),
			      /* TRANSLATORS: command description */
			      _("Gets details about a firmware file"),
			      fu_util_get_details);
	fu_util_cmd_array_add(cmd_array,
			      "get-updates,get-upgrades",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Gets the list of updates for all specified devices, or all "
				"devices if unspecified"),
			      fu_util_get_updates);
	fu_util_cmd_array_add(cmd_array,
			      "update,upgrade",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Updates all specified devices to latest firmware version, or all "
				"devices if unspecified"),
			      fu_util_update);
	fu_util_cmd_array_add(cmd_array,
			      "verify",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Checks cryptographic hash matches firmware"),
			      fu_util_verify);
	fu_util_cmd_array_add(cmd_array,
			      "unlock",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("DEVICE-ID|GUID"),
			      /* TRANSLATORS: command description */
			      _("Unlocks the device for firmware access"),
			      fu_util_unlock);
	fu_util_cmd_array_add(cmd_array,
			      "clear-results",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("DEVICE-ID|GUID"),
			      /* TRANSLATORS: command description */
			      _("Clears the results from the last update"),
			      fu_util_clear_results);
	fu_util_cmd_array_add(cmd_array,
			      "get-results",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("DEVICE-ID|GUID"),
			      /* TRANSLATORS: command description */
			      _("Gets the results from the last update"),
			      fu_util_get_results);
	fu_util_cmd_array_add(cmd_array,
			      "get-releases",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Gets the releases for a device"),
			      fu_util_get_releases);
	fu_util_cmd_array_add(cmd_array,
			      "get-remotes",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Gets the configured remotes"),
			      fu_util_get_remotes);
	fu_util_cmd_array_add(cmd_array,
			      "downgrade",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Downgrades the firmware on a device"),
			      fu_util_downgrade);
	fu_util_cmd_array_add(cmd_array,
			      "refresh",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[FILE FILE_SIG REMOTE-ID]"),
			      /* TRANSLATORS: command description */
			      _("Refresh metadata from remote server"),
			      fu_util_refresh);
	fu_util_cmd_array_add(cmd_array,
			      "verify-update",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Update the stored cryptographic hash with current ROM contents"),
			      fu_util_verify_update);
	fu_util_cmd_array_add(cmd_array,
			      "modify-remote",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("REMOTE-ID KEY VALUE"),
			      /* TRANSLATORS: command description */
			      _("Modifies a given remote"),
			      fu_util_remote_modify);
	fu_util_cmd_array_add(cmd_array,
			      "enable-remote",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("REMOTE-ID"),
			      /* TRANSLATORS: command description */
			      _("Enables a given remote"),
			      fu_util_remote_enable);
	fu_util_cmd_array_add(cmd_array,
			      "disable-remote",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("REMOTE-ID"),
			      /* TRANSLATORS: command description */
			      _("Disables a given remote"),
			      fu_util_remote_disable);
	fu_util_cmd_array_add(cmd_array,
			      "activate",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Activate devices"),
			      fu_util_activate);
	fu_util_cmd_array_add(cmd_array,
			      "get-approved-firmware",
			      NULL,
			      /* TRANSLATORS: firmware approved by the admin */
			      _("Gets the list of approved firmware"),
			      fu_util_get_approved_firmware);
	fu_util_cmd_array_add(cmd_array,
			      "set-approved-firmware",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME|CHECKSUM1[,CHECKSUM2][,CHECKSUM3]"),
			      /* TRANSLATORS: firmware approved by the admin */
			      _("Sets the list of approved firmware"),
			      fu_util_set_approved_firmware);
	fu_util_cmd_array_add(cmd_array,
			      "modify-config",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[SECTION] KEY VALUE"),
			      /* TRANSLATORS: sets something in the daemon configuration file */
			      _("Modifies a daemon configuration value"),
			      fu_util_modify_config);
	fu_util_cmd_array_add(cmd_array,
			      "reset-config",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("SECTION"),
			      /* TRANSLATORS: sets something in the daemon configuration file */
			      _("Resets a daemon configuration section"),
			      fu_util_reset_config);
	fu_util_cmd_array_add(cmd_array,
			      "reinstall",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Reinstall current firmware on the device"),
			      fu_util_reinstall);
	fu_util_cmd_array_add(cmd_array,
			      "switch-branch",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID] [BRANCH]"),
			      /* TRANSLATORS: command description */
			      _("Switch the firmware branch on the device"),
			      fu_util_switch_branch);
	fu_util_cmd_array_add(cmd_array,
			      "security",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Gets the host security attributes"),
			      fu_util_security);
	fu_util_cmd_array_add(cmd_array,
			      "sync,sync-bkc",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Sync firmware versions to the chosen configuration"),
			      fu_util_sync);
	fu_util_cmd_array_add(cmd_array,
			      "block-firmware",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[CHECKSUM]"),
			      /* TRANSLATORS: command description */
			      _("Blocks a specific firmware from being installed"),
			      fu_util_block_firmware);
	fu_util_cmd_array_add(cmd_array,
			      "unblock-firmware",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[CHECKSUM]"),
			      /* TRANSLATORS: command description */
			      _("Unblocks a specific firmware from being installed"),
			      fu_util_unblock_firmware);
	fu_util_cmd_array_add(cmd_array,
			      "get-blocked-firmware",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Gets the list of blocked firmware"),
			      fu_util_get_blocked_firmware);
	fu_util_cmd_array_add(cmd_array,
			      "get-plugins",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Get all enabled plugins registered with the system"),
			      fu_util_get_plugins);
	fu_util_cmd_array_add(cmd_array,
			      "download",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("LOCATION"),
			      /* TRANSLATORS: command description */
			      _("Download a file"),
			      fu_util_download);
	fu_util_cmd_array_add(cmd_array,
			      "device-test",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[FILENAME1] [FILENAME2]"),
			      /* TRANSLATORS: command description */
			      _("Test a device using a JSON manifest"),
			      fu_util_device_test);
	fu_util_cmd_array_add(cmd_array,
			      "device-emulate",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[FILENAME1] [FILENAME2]"),
			      /* TRANSLATORS: command description */
			      _("Emulate a device using a JSON manifest"),
			      fu_util_device_emulate);
	fu_util_cmd_array_add(cmd_array,
			      "inhibit",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[REASON] [TIMEOUT]"),
			      /* TRANSLATORS: command description */
			      _("Inhibit the system to prevent upgrades"),
			      fu_util_inhibit);
	fu_util_cmd_array_add(cmd_array,
			      "uninhibit",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("INHIBIT-ID"),
			      /* TRANSLATORS: command description */
			      _("Uninhibit the system to allow upgrades"),
			      fu_util_uninhibit);
	fu_util_cmd_array_add(cmd_array,
			      "device-wait",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("GUID|DEVICE-ID"),
			      /* TRANSLATORS: command description */
			      _("Wait for a device to appear"),
			      fu_util_device_wait);
	fu_util_cmd_array_add(cmd_array,
			      "quit",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Asks the daemon to quit"),
			      fu_util_quit);
	fu_util_cmd_array_add(
	    cmd_array,
	    "get-bios-settings,get-bios-setting",
	    /* TRANSLATORS: command argument: uppercase, spaces->dashes */
	    _("[SETTING1] [SETTING2] [--no-authenticate]"),
	    /* TRANSLATORS: command description */
	    _("Retrieve BIOS settings.  If no arguments are passed all settings are returned"),
	    fu_util_get_bios_setting);
	fu_util_cmd_array_add(cmd_array,
			      "set-bios-setting",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("SETTING1 VALUE1 [SETTING2] [VALUE2]"),
			      /* TRANSLATORS: command description */
			      _("Sets one or more BIOS settings"),
			      fu_util_set_bios_setting);
	fu_util_cmd_array_add(cmd_array,
			      "emulation-load",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME"),
			      /* TRANSLATORS: command description */
			      _("Load device emulation data"),
			      fu_util_emulation_load);
	fu_util_cmd_array_add(cmd_array,
			      "emulation-save",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME"),
			      /* TRANSLATORS: command description */
			      _("Save device emulation data"),
			      fu_util_emulation_save);
	fu_util_cmd_array_add(cmd_array,
			      "emulation-tag",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Adds devices to watch for future emulation"),
			      fu_util_emulation_tag);
	fu_util_cmd_array_add(cmd_array,
			      "emulation-untag",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Removes devices to watch for future emulation"),
			      fu_util_emulation_untag);
	fu_util_cmd_array_add(cmd_array,
			      "security-fix",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[APPSTREAM_ID]"),
			      /* TRANSLATORS: command description */
			      _("Fix a specific host security attribute"),
			      fu_util_security_fix);
	fu_util_cmd_array_add(cmd_array,
			      "security-undo",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[APPSTREAM_ID]"),
			      /* TRANSLATORS: command description */
			      _("Undo the host security attribute fix"),
			      fu_util_security_undo);
	fu_util_cmd_array_add(cmd_array,
			      "report-devices",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Upload the list of updatable devices to a remote server"),
			      fu_util_report_devices);

	/* do stuff on ctrl+c */
	self->cancellable = g_cancellable_new();
	g_signal_connect(G_CANCELLABLE(self->cancellable),
			 "cancelled",
			 G_CALLBACK(fu_util_cancelled_cb),
			 self);

	/* sort by command name */
	fu_util_cmd_array_sort(cmd_array);

	/* non-TTY consoles cannot answer questions */
	if (!fu_util_setup_interactive(self, &error_console)) {
		g_info("failed to initialize interactive console: %s", error_console->message);
		self->no_unreported_check = TRUE;
		self->no_metadata_check = TRUE;
		self->no_reboot_check = TRUE;
		self->no_safety_check = TRUE;
		self->no_remote_check = TRUE;
		self->no_device_prompt = TRUE;
		self->no_emulation_check = TRUE;
		self->no_security_fix = TRUE;
	} else {
		is_interactive = TRUE;
	}
	fu_console_set_interactive(self->console, is_interactive);

	/* get a list of the commands */
	self->context = g_option_context_new(NULL);
	cmd_descriptions = fu_util_cmd_array_to_string(cmd_array);
	g_option_context_set_summary(self->context, cmd_descriptions);
	g_option_context_set_description(
	    self->context,
	    /* TRANSLATORS: CLI description */
	    _("This tool allows an administrator to query and control the "
	      "fwupd daemon, allowing them to perform actions such as "
	      "installing or downgrading firmware."));

	/* TRANSLATORS: program name */
	g_set_application_name(_("Firmware Utility"));
	g_option_context_add_main_entries(self->context, options, NULL);
	ret = g_option_context_parse(self->context, &argc, &argv, &error);
	if (!ret) {
		fu_console_print(self->console,
				 "%s: %s",
				 /* TRANSLATORS: the user didn't read the man page */
				 _("Failed to parse arguments"),
				 error->message);
		return EXIT_FAILURE;
	}

	/* allow disabling SSL strict mode for broken corporate proxies */
	if (self->disable_ssl_strict) {
		fu_console_print_full(self->console,
				      FU_CONSOLE_PRINT_FLAG_WARNING,
				      "%s\n",
				      /* TRANSLATORS: try to help */
				      _("Ignoring SSL strict checks, "
					"to do this automatically in the future "
					"export DISABLE_SSL_STRICT in your environment"));
		(void)g_setenv("DISABLE_SSL_STRICT", "1", TRUE);
	}

	/* this doesn't have to be precise (e.g. using the build-year) as we just
	 * want to check the clock is not set to the default of 1970-01-01... */
	if (g_date_time_get_year(dt_now) < 2021) {
		fu_console_print_full(
		    self->console,
		    FU_CONSOLE_PRINT_FLAG_WARNING,
		    "%s\n",
		    /* TRANSLATORS: try to help */
		    _("The system clock has not been set correctly and downloading "
		      "files may fail."));
	}

	/* parse filter flags */
	if (filter_device != NULL) {
		if (!fu_util_parse_filter_device_flags(filter_device,
						       &self->filter_device_include,
						       &self->filter_device_exclude,
						       &error)) {
			g_autofree gchar *str =
			    /* TRANSLATORS: the user didn't read the man page, %1 is '--filter' */
			    g_strdup_printf(_("Failed to parse flags for %s"), "--filter");
			g_prefix_error(&error, "%s: ", str);
			fu_util_print_error(self, error);
			return EXIT_FAILURE;
		}
	}
	if (filter_release != NULL) {
		if (!fu_util_parse_filter_release_flags(filter_release,
							&self->filter_release_include,
							&self->filter_release_exclude,
							&error)) {
			g_autofree gchar *str =
			    /* TRANSLATORS: the user didn't read the man page,
			     * %1 is '--filter-release' */
			    g_strdup_printf(_("Failed to parse flags for %s"), "--filter-release");
			g_prefix_error(&error, "%s: ", str);
			fu_util_print_error(self, error);
			return EXIT_FAILURE;
		}
	}

	/* set verbose? */
	if (verbose) {
		(void)g_setenv("G_MESSAGES_DEBUG", "all", FALSE);
		(void)g_setenv("FWUPD_VERBOSE", "1", FALSE);
	} else {
		g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, fu_util_ignore_cb, NULL);
	}

	/* set up ctrl+c */
	fu_util_setup_signal_handlers(self);

	/* set flags */
	if (allow_reinstall)
		self->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	if (allow_older)
		self->flags |= FWUPD_INSTALL_FLAG_ALLOW_OLDER;
	if (allow_branch_switch)
		self->flags |= FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH;
	if (only_emulated)
		self->flags |= FWUPD_INSTALL_FLAG_ONLY_EMULATED;
	if (force) {
		self->flags |= FWUPD_INSTALL_FLAG_FORCE;
		self->flags |= FWUPD_INSTALL_FLAG_IGNORE_REQUIREMENTS;
	}
	if (no_history)
		self->flags |= FWUPD_INSTALL_FLAG_NO_HISTORY;

	/* use peer-to-peer for metadata and firmware *only* if specified */
	if (only_p2p)
		self->download_flags |= FWUPD_CLIENT_DOWNLOAD_FLAG_ONLY_P2P;

#ifdef HAVE_POLKIT
	/* start polkit tty agent to listen for password requests */
	if (is_interactive) {
		g_autoptr(GError) error_polkit = NULL;
		if (!fu_polkit_agent_open(polkit_agent, &error_polkit)) {
			fu_console_print(self->console,
					 "Failed to open polkit agent: %s",
					 error_polkit->message);
		}
	}
#endif

	/* connect to the daemon */
	self->client = fwupd_client_new();
	fwupd_client_set_main_context(self->client, self->main_ctx);
	fwupd_client_download_set_retries(self->client, download_retries);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "notify::percentage",
			 G_CALLBACK(fu_util_client_notify_cb),
			 self);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "notify::status",
			 G_CALLBACK(fu_util_client_notify_cb),
			 self);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "device-changed",
			 G_CALLBACK(fu_util_update_device_changed_cb),
			 self);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "device-request",
			 G_CALLBACK(fu_util_update_device_request_cb),
			 self);

	/* show a warning if the daemon is tainted */
	if (!fwupd_client_connect(self->client, self->cancellable, &error)) {
#ifdef _WIN32
		fu_console_print_literal(
		    self->console,
		    /* TRANSLATORS: error message for Windows */
		    _("Failed to connect to Windows service, please ensure it's running."));
		g_debug("%s", error->message);
#else
		/* TRANSLATORS: could not contact the fwupd service over D-Bus */
		g_prefix_error(&error, "%s: ", _("Failed to connect to daemon"));
		fu_util_print_error(self, error);
#endif
		return EXIT_FAILURE;
	}
	if (fwupd_client_get_tainted(self->client)) {
		fu_console_print_full(self->console,
				      FU_CONSOLE_PRINT_FLAG_WARNING,
				      "%s\n",
				      /* TRANSLATORS: the user is SOL for support... */
				      _("The daemon has loaded 3rd party code and "
					"is no longer supported by the upstream developers!"));
	}

	/* just show versions and exit */
	if (version) {
		if (!fu_util_version(self, &error)) {
			fu_util_print_error(self, error);
			return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
	}

	if (!self->as_json) {
		/* show user-visible warnings from the plugins */
		fu_util_show_plugin_warnings(self);

		/* show any unsupported warnings */
		fu_util_show_unsupported_warning(self->console);
	}

	/* we know the runtime daemon version now */
	fwupd_client_set_user_agent_for_package(self->client, g_get_prgname(), PACKAGE_VERSION);

	/* check that we have at least this version daemon running */
	if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    !fu_util_check_daemon_version(self, &error)) {
		fu_util_print_error(self, error);
		return EXIT_FAILURE;
	}

	/* make sure polkit actions were installed */
	if (!fu_util_check_polkit_actions(&error)) {
		fu_util_print_error(self, error);
		return EXIT_FAILURE;
	}

	/* send our implemented feature set */
	if (is_interactive) {
		feature_flags |=
		    FWUPD_FEATURE_FLAG_REQUESTS | FWUPD_FEATURE_FLAG_REQUESTS_NON_GENERIC |
		    FWUPD_FEATURE_FLAG_UPDATE_ACTION | FWUPD_FEATURE_FLAG_DETACH_ACTION;
		if (!no_authenticate)
			feature_flags |= FWUPD_FEATURE_FLAG_ALLOW_AUTHENTICATION;
	}
	if (!fwupd_client_set_feature_flags(self->client,
					    feature_flags,
					    self->cancellable,
					    &error)) {
		/* TRANSLATORS: a feature is something like "can show an image" */
		g_prefix_error(&error, "%s: ", _("Failed to set front-end features"));
		fu_util_print_error(self, error);
		return EXIT_FAILURE;
	}

	/* run the specified command */
	ret = fu_util_cmd_array_run(cmd_array, self, argv[1], (gchar **)&argv[2], &error);
	if (!ret) {
#ifdef SUPPORTED_BUILD
		/* sanity check */
		if (error == NULL) {
			g_critical("exec failed but no error set!");
			return EXIT_FAILURE;
		}
#endif
		fu_util_print_error(self, error);
		if (!self->as_json &&
		    g_error_matches(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_ARGS)) {
			g_autofree gchar *cmd = g_strdup_printf("%s --help", g_get_prgname());
			g_autoptr(GString) str = g_string_new("\n");
			/* TRANSLATORS: explain how to get help,
			 * where $1 is something like 'fwupdmgr --help' */
			g_string_append_printf(str, _("Use %s for help"), cmd);
			fu_console_print_literal(self->console, str->str);
		} else if (g_error_matches(error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO))
			return EXIT_NOTHING_TO_DO;
		else if (g_error_matches(error, FWUPD_ERROR, FWUPD_ERROR_NOT_REACHABLE))
			return EXIT_NOT_REACHABLE;
		else if (g_error_matches(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND))
			return EXIT_NOT_FOUND;
		return EXIT_FAILURE;
	}

	/* success */
	return EXIT_SUCCESS;
}
