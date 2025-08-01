/*
 * Copyright 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuMain"

#include "config.h"

#include <fwupdplugin.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#ifdef HAVE_GIO_UNIX
#include <glib-unix.h>
#endif
#include <fcntl.h>
#include <jcat.h>
#include <locale.h>
#include <stdlib.h>
#include <unistd.h>

#include "fwupd-enums-private.h"
#include "fwupd-remote-private.h"

#include "fu-bios-settings-private.h"
#include "fu-cabinet.h"
#include "fu-console.h"
#include "fu-context-private.h"
#include "fu-debug.h"
#include "fu-device-private.h"
#include "fu-engine-helper.h"
#include "fu-engine-requirements.h"
#include "fu-engine.h"
#include "fu-history.h"
#include "fu-plugin-private.h"
#include "fu-security-attrs-private.h"
#include "fu-smbios-private.h"
#include "fu-util-bios-setting.h"
#include "fu-util-common.h"

#ifdef HAVE_SYSTEMD
#include "fu-systemd.h"
#define SYSTEMD_FWUPD_UNIT	"fwupd.service"
#define SYSTEMD_SNAP_FWUPD_UNIT "snap.fwupd.fwupd.service"
#endif

typedef enum {
	FU_UTIL_OPERATION_UNKNOWN,
	FU_UTIL_OPERATION_UPDATE,
	FU_UTIL_OPERATION_INSTALL,
	FU_UTIL_OPERATION_READ,
	FU_UTIL_OPERATION_LAST
} FuUtilOperation;

struct FuUtil {
	GCancellable *cancellable;
	GMainContext *main_ctx;
	GMainLoop *loop;
	GOptionContext *context;
	FuContext *ctx;
	FuEngine *engine;
	FuEngineRequest *request;
	FuProgress *progress;
	FuConsole *console;
	FwupdClient *client;
	gboolean as_json;
	gboolean no_reboot_check;
	gboolean no_safety_check;
	gboolean no_device_prompt;
	gboolean prepare_blob;
	gboolean cleanup_blob;
	gboolean enable_json_state;
	gboolean interactive;
	FwupdInstallFlags flags;
	FuFirmwareParseFlags parse_flags;
	gboolean show_all;
	gboolean disable_ssl_strict;
	gint lock_fd;
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
fu_util_show_plugin_warnings(FuUtil *self)
{
	FwupdPluginFlags flags = FWUPD_PLUGIN_FLAG_NONE;
	GPtrArray *plugins;

	if (self->as_json)
		return;

	/* get a superset so we do not show the same message more than once */
	plugins = fu_engine_get_plugins(self->engine);
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
		if ((flags & flag) == 0)
			continue;
		tmp = fu_util_plugin_flag_to_string((guint64)1 << i);
		if (tmp == NULL)
			continue;
		fu_console_print_full(self->console, FU_CONSOLE_PRINT_FLAG_WARNING, "%s\n", tmp);
		url = g_strdup_printf("https://github.com/fwupd/fwupd/wiki/PluginFlag:%s",
				      fwupd_plugin_flag_to_string(flag));
		/* TRANSLATORS: %s is a link to a website */
		fu_console_print(self->console, _("See %s for more information."), url);
	}
}

static gboolean
fu_util_lock(FuUtil *self, GError **error)
{
#ifdef HAVE_WRLCK
	struct flock lockp = {
	    .l_type = F_WRLCK,
	    .l_whence = SEEK_SET,
	};
	g_autofree gchar *lockfn = NULL;
	gboolean use_user = FALSE;

#ifdef HAVE_GETUID
	if (getuid() != 0 || geteuid() != 0)
		use_user = TRUE;
#endif

	/* open file */
	if (use_user) {
		lockfn = fu_util_get_user_cache_path("fwupdtool");
	} else {
		g_autofree gchar *lockdir = fu_path_from_kind(FU_PATH_KIND_LOCKDIR);
		lockfn = g_build_filename(lockdir, "fwupdtool", NULL);
	}
	if (!fu_path_mkdir_parent(lockfn, error))
		return FALSE;
	self->lock_fd = g_open(lockfn, O_RDWR | O_CREAT, S_IRWXU);
	if (self->lock_fd < 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "failed to open %s",
			    lockfn);
		return FALSE;
	}

	/* write lock */
#ifdef HAVE_OFD
	if (fcntl(self->lock_fd, F_OFD_SETLK, &lockp) < 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "another instance has locked %s",
			    lockfn);
		return FALSE;
	}
#else
	if (fcntl(self->lock_fd, F_SETLK, &lockp) < 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "another instance has locked %s",
			    lockfn);
		return FALSE;
	}
#endif

	/* success */
	g_debug("locked %s", lockfn);
#endif
	return TRUE;
}

#ifdef HAVE_SYSTEMD
static const gchar *
fu_util_get_systemd_unit(void)
{
	if (g_strcmp0(g_getenv("SNAP_NAME"), "fwupd") == 0)
		return SYSTEMD_SNAP_FWUPD_UNIT;
	return SYSTEMD_FWUPD_UNIT;
}
#endif

static gboolean
fu_util_start_engine(FuUtil *self, FuEngineLoadFlags flags, FuProgress *progress, GError **error)
{
	/* already done */
	if (fu_engine_get_loaded(self->engine))
		return TRUE;

	if (!fu_util_lock(self, error)) {
		/* TRANSLATORS: another fwupdtool instance is already running */
		g_prefix_error(error, "%s: ", _("Failed to lock"));
		return FALSE;
	}
#ifdef HAVE_SYSTEMD
	if (getuid() != 0 || geteuid() != 0) {
		g_info("not attempting to stop daemon when running as user");
	} else {
		g_autoptr(GError) error_local = NULL;
		if (!fu_systemd_unit_stop(fu_util_get_systemd_unit(), &error_local))
			g_info("failed to stop daemon: %s", error_local->message);
	}
#endif
	flags |= FU_ENGINE_LOAD_FLAG_NO_IDLE_SOURCES;
	flags |= FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS;
	flags |= FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS;
	if (!fu_engine_load(self->engine, flags, progress, error))
		return FALSE;

	if (!self->as_json) {
		fu_util_show_plugin_warnings(self);
		fu_util_show_unsupported_warning(self->console);
	}

	/* copy properties from engine to client */
	if (flags & FU_ENGINE_LOAD_FLAG_HWINFO) {
		g_object_set(self->client,
			     "host-vendor",
			     fu_engine_get_host_vendor(self->engine),
			     "host-product",
			     fu_engine_get_host_product(self->engine),
			     "battery-level",
			     fu_context_get_battery_level(fu_engine_get_context(self->engine)),
			     "battery-threshold",
			     fu_context_get_battery_threshold(fu_engine_get_context(self->engine)),
			     NULL);
	}

	/* success */
	return TRUE;
}

static void
fu_util_maybe_prefix_sandbox_error(const gchar *value, GError **error)
{
	g_autofree gchar *path = g_path_get_dirname(value);
	if (!g_file_test(path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR)) {
		g_prefix_error(error,
			       "Unable to access %s. You may need to copy %s to %s: ",
			       path,
			       value,
			       g_getenv("HOME"));
	}
}

static void
fu_util_cancelled_cb(GCancellable *cancellable, gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;
	/* TRANSLATORS: this is when a device ctrl+c's a watch */
	fu_console_print_literal(self->console, _("Cancelled"));
	g_main_loop_quit(self->loop);
}

static gboolean
fu_util_smbios_dump(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *tmp = NULL;
	g_autoptr(FuSmbios) smbios = NULL;
	if (g_strv_length(values) < 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}
	smbios = fu_smbios_new();
	if (!fu_smbios_setup_from_file(smbios, values[0], error))
		return FALSE;
	tmp = fu_firmware_to_string(FU_FIRMWARE(smbios));
	fu_console_print_literal(self->console, tmp);
	return TRUE;
}

#ifdef HAVE_GIO_UNIX
static gboolean
fu_util_sigint_cb(gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;
	g_info("handling SIGINT");
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
	if (self->current_device != NULL)
		g_object_unref(self->current_device);
	if (self->ctx != NULL)
		g_object_unref(self->ctx);
	if (self->engine != NULL)
		g_object_unref(self->engine);
	if (self->request != NULL)
		g_object_unref(self->request);
	if (self->client != NULL)
		g_object_unref(self->client);
	if (self->main_ctx != NULL)
		g_main_context_unref(self->main_ctx);
	if (self->loop != NULL)
		g_main_loop_unref(self->loop);
	if (self->cancellable != NULL)
		g_object_unref(self->cancellable);
	if (self->console != NULL)
		g_object_unref(self->console);
	if (self->progress != NULL)
		g_object_unref(self->progress);
	if (self->context != NULL)
		g_option_context_free(self->context);
	if (self->lock_fd >= 0)
		g_close(self->lock_fd, NULL);
	g_ptr_array_unref(self->post_requests);
	g_free(self);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuUtil, fu_util_private_free)
#pragma clang diagnostic pop

static void
fu_util_update_device_request_cb(FwupdClient *client, FwupdRequest *request, FuUtil *self)
{
	/* action has not been assigned yet */
	if (self->current_operation == FU_UTIL_OPERATION_UNKNOWN)
		return;

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
fu_util_engine_device_added_cb(FuEngine *engine, FuDevice *device, FuUtil *self)
{
	if (g_getenv("FWUPD_VERBOSE") != NULL) {
		g_autofree gchar *tmp = fu_device_to_string(device);
		g_debug("ADDED:\n%s", tmp);
	}
}

static void
fu_util_engine_device_removed_cb(FuEngine *engine, FuDevice *device, FuUtil *self)
{
	if (g_getenv("FWUPD_VERBOSE") != NULL) {
		g_autofree gchar *tmp = fu_device_to_string(device);
		g_debug("REMOVED:\n%s", tmp);
	}
}

static void
fu_util_engine_status_changed_cb(FuEngine *engine, FwupdStatus status, FuUtil *self)
{
	if (self->as_json)
		return;
	fu_console_set_progress(self->console, status, 0);
}

static void
fu_util_progress_percentage_changed_cb(FuProgress *progress, guint percentage, FuUtil *self)
{
	if (self->as_json)
		return;
	fu_console_set_progress(self->console, fu_progress_get_status(progress), percentage);
}

static void
fu_util_progress_status_changed_cb(FuProgress *progress, FwupdStatus status, FuUtil *self)
{
	if (self->as_json)
		return;
	fu_console_set_progress(self->console, status, fu_progress_get_percentage(progress));
}

static gboolean
fu_util_watch(FuUtil *self, gchar **values, GError **error)
{
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG,
				  self->progress,
				  error))
		return FALSE;
	g_main_loop_run(self->loop);
	return TRUE;
}

static gint
fu_util_verfmt_sort_cb(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar **)a, *(const gchar **)b);
}

static gboolean
fu_util_get_verfmts(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) verfmts = g_ptr_array_new_with_free_func((GDestroyNotify)g_free);

	for (guint i = FWUPD_VERSION_FORMAT_PLAIN; i < FWUPD_VERSION_FORMAT_LAST; i++) {
		g_autofree gchar *format = g_strdup(fwupd_version_format_to_string(i));
		if (format == NULL)
			continue;
		g_ptr_array_add(verfmts, g_steal_pointer(&format));
	}
	g_ptr_array_sort(verfmts, (GCompareFunc)fu_util_verfmt_sort_cb);

	/* print */
	if (self->as_json) {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		json_builder_begin_array(builder);
		for (guint i = 0; i < verfmts->len; i++) {
			const gchar *verfmt = g_ptr_array_index(verfmts, i);
			json_builder_add_string_value(builder, verfmt);
		}
		json_builder_end_array(builder);
		return fu_util_print_builder(self->console, builder, error);
	}

	/* print */
	for (guint i = 0; i < verfmts->len; i++) {
		const gchar *verfmt = g_ptr_array_index(verfmts, i);
		fu_console_print_literal(self->console, verfmt);
	}

	return TRUE;
}

static gboolean
fu_util_get_plugins(FuUtil *self, gchar **values, GError **error)
{
	GPtrArray *plugins;

	/* load engine */
	if (!fu_util_start_engine(
		self,
		FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
		    FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS | FU_ENGINE_LOAD_FLAG_HWINFO,
		self->progress,
		error))
		return FALSE;

	/* print */
	plugins = fu_engine_get_plugins(self->engine);
	g_ptr_array_sort(plugins, (GCompareFunc)fu_util_plugin_name_sort_cb);
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

	return TRUE;
}

static FuDevice *
fu_util_prompt_for_device(FuUtil *self, GPtrArray *devices_opt, GError **error)
{
	FuDevice *dev;
	guint idx;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_filtered = NULL;

	/* get devices from daemon */
	if (devices_opt != NULL) {
		devices = g_ptr_array_ref(devices_opt);
	} else {
		devices = fu_engine_get_devices(self->engine, error);
		if (devices == NULL)
			return NULL;
	}
	fwupd_device_array_ensure_parents(devices);

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
			    fu_device_get_name(dev));
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
				 fu_device_get_id(dev),
				 fu_device_get_name(dev));
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

static FuDevice *
fu_util_get_device(FuUtil *self, const gchar *id, GError **error)
{
	if (fwupd_guid_is_valid(id)) {
		g_autoptr(GPtrArray) devices = NULL;
		devices = fu_engine_get_devices_by_guid(self->engine, id, error);
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
	return fu_engine_get_device(self->engine, id, error);
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

		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_SUPPORTED))
			continue;

		/* get the releases for this device and filter for validity */
		rels = fu_engine_get_upgrades(self->engine,
					      self->request,
					      fwupd_device_get_id(dev),
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
	g_autoptr(FuUtilNode) root = g_node_new(NULL);
	g_autoptr(GPtrArray) devices_no_support = g_ptr_array_new();
	g_autoptr(GPtrArray) devices_no_upgrades = g_ptr_array_new();

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_REMOTES |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* parse arguments */
	if (g_strv_length(values) == 0) {
		devices = fu_engine_get_devices(self->engine, error);
		if (devices == NULL)
			return FALSE;
	} else {
		devices = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
		for (guint idx = 0; idx < g_strv_length(values); idx++) {
			FuDevice *device = fu_util_get_device(self, values[idx], error);
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

	/* not for human consumption */
	if (self->as_json)
		return fu_util_get_updates_as_json(self, devices, error);

	fwupd_device_array_ensure_parents(devices);
	g_ptr_array_sort(devices, fu_util_sort_devices_by_flags_cb);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;
		FuUtilNode *child;

		/* not going to have results, so save a engine round-trip */
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

		/* get the releases for this device and filter for validity */
		rels = fu_engine_get_upgrades(self->engine,
					      self->request,
					      fwupd_device_get_id(dev),
					      &error_local);
		if (rels == NULL) {
			g_ptr_array_add(devices_no_upgrades, dev);
			/* discard the actual reason from user, but leave for debugging */
			g_debug("%s", error_local->message);
			continue;
		}
		child = g_node_append_data(root, g_object_ref(dev));

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

	/* updates */
	if (g_node_n_nodes(root, G_TRAVERSE_ALL) <= 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    _("No updates available for remaining devices"));
		return FALSE;
	}

	fu_util_print_node(self->console, self->client, root);
	return TRUE;
}

static gboolean
fu_util_get_details(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) array = NULL;
	g_autoptr(FuUtilNode) root = g_node_new(NULL);
	g_autoptr(GInputStream) stream = NULL;

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_REMOTES |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

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

	/* open file */
	stream = fu_input_stream_from_path(values[0], error);
	if (stream == NULL) {
		fu_util_maybe_prefix_sandbox_error(values[0], error);
		return FALSE;
	}
	array = fu_engine_get_details(self->engine, self->request, stream, error);
	if (array == NULL)
		return FALSE;
	for (guint i = 0; i < array->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(array, i);
		FwupdRelease *rel;
		FuUtilNode *child;
		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		child = g_node_append_data(root, g_object_ref(dev));
		rel = fwupd_device_get_release_default(dev);
		if (rel != NULL)
			g_node_append_data(child, g_object_ref(rel));
	}
	fu_util_print_node(self->console, self->client, root);

	return TRUE;
}

static gboolean
fu_util_get_device_flags(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GString) str = g_string_new(NULL);

	for (FwupdDeviceFlags i = FWUPD_DEVICE_FLAG_INTERNAL; i < FWUPD_DEVICE_FLAG_UNKNOWN;
	     i <<= 1) {
		const gchar *tmp = fwupd_device_flag_to_string(i);
		if (tmp == NULL)
			break;
		if (i != FWUPD_DEVICE_FLAG_INTERNAL)
			g_string_append(str, " ");
		g_string_append(str, tmp);
		g_string_append(str, " ~");
		g_string_append(str, tmp);
	}
	fu_console_print_literal(self->console, str->str);

	return TRUE;
}

static void
fu_util_build_device_tree(FuUtil *self, FuUtilNode *root, GPtrArray *devs, FuDevice *dev)
{
	for (guint i = 0; i < devs->len; i++) {
		FuDevice *dev_tmp = g_ptr_array_index(devs, i);
		if (!fwupd_device_match_flags(FWUPD_DEVICE(dev_tmp),
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		if (!self->show_all && !fu_util_is_interesting_device(devs, FWUPD_DEVICE(dev_tmp)))
			continue;
		if (fu_device_get_parent(dev_tmp) == dev) {
			FuUtilNode *child = g_node_append_data(root, g_object_ref(dev_tmp));
			fu_util_build_device_tree(self, child, devs, dev_tmp);
		}
	}
}

static gboolean
fu_util_get_devices_as_json(FuUtil *self, GPtrArray *devs, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "Devices");
	json_builder_begin_array(builder);
	for (guint i = 0; i < devs->len; i++) {
		FuDevice *dev = g_ptr_array_index(devs, i);
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;

		/* add all releases that could be applied */
		rels = fu_engine_get_releases_for_device(self->engine,
							 self->request,
							 dev,
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
				fu_device_add_release(dev, rel);
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
fu_util_get_devices(FuUtil *self, gchar **values, GError **error)
{
	FuEngineLoadFlags load_flags =
	    FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO;
	g_autoptr(FuUtilNode) root = g_node_new(NULL);
	g_autoptr(GPtrArray) devs = NULL;

	/* show all devices, even those without assigned plugins */
	if (self->flags & FWUPD_INSTALL_FLAG_FORCE)
		load_flags |= FU_ENGINE_LOAD_FLAG_COLDPLUG_FORCE;

	/* load engine */
	if (!fu_util_start_engine(self, load_flags, self->progress, error))
		return FALSE;

	/* get devices and build tree */
	if (g_strv_length(values) > 0) {
		devs = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
		for (guint i = 0; values[i] != NULL; i++) {
			FuDevice *device = fu_util_get_device(self, values[i], error);
			if (device == NULL)
				return FALSE;
			g_ptr_array_add(devs, device);
		}
	} else {
		devs = fu_engine_get_devices(self->engine, error);
		if (devs == NULL)
			return FALSE;
	}

	/* not for human consumption */
	if (self->as_json)
		return fu_util_get_devices_as_json(self, devs, error);

	if (devs->len > 0) {
		fwupd_device_array_ensure_parents(devs);
		fu_util_build_device_tree(self, root, devs, NULL);
	}

	/* print */
	if (g_node_n_children(root) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: nothing attached that can be upgraded */
				    _("No hardware detected with firmware update capability"));
		return FALSE;
	}
	fu_util_print_node(self->console, self->client, root);

	return TRUE;
}

static void
fu_util_update_device_changed_cb(FwupdClient *client, FwupdDevice *device, FuUtil *self)
{
	g_autofree gchar *str = NULL;

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
	} else if (self->current_operation == FU_UTIL_OPERATION_INSTALL) {
		/* TRANSLATORS: %1 is a device name  */
		str = g_strdup_printf(_("Installing on %s…"), fwupd_device_get_name(device));
		fu_console_set_progress_title(self->console, str);
	} else if (self->current_operation == FU_UTIL_OPERATION_READ) {
		/* TRANSLATORS: %1 is a device name  */
		str = g_strdup_printf(_("Reading from %s…"), fwupd_device_get_name(device));
		fu_console_set_progress_title(self->console, str);
	} else {
		g_warning("no FuUtilOperation set");
	}
	g_set_object(&self->current_device, device);
}

static void
fu_util_display_current_message(FuUtil *self)
{
	if (self->as_json)
		return;

	/* print all POST requests */
	for (guint i = 0; i < self->post_requests->len; i++) {
		FwupdRequest *request = g_ptr_array_index(self->post_requests, i);
		fu_console_print_literal(self->console, fu_util_request_get_message(request));
	}
}

static gboolean
fu_util_install_blob(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *firmware_basename = NULL;
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuRelease) release = fu_release_new();
	g_autoptr(GInputStream) stream_fw = NULL;

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_flag(self->progress, FU_PROGRESS_FLAG_NO_PROFILE);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 2, "parse");
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 30, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_WRITE, 68, NULL);

	/* invalid args */
	if (g_strv_length(values) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* parse blob */
	stream_fw = fu_input_stream_from_path(values[0], error);
	if (stream_fw == NULL) {
		fu_util_maybe_prefix_sandbox_error(values[0], error);
		return FALSE;
	}
	fu_release_set_stream(release, stream_fw);
	fu_progress_step_done(self->progress);

	/* some plugins need the firmware name */
	firmware_basename = g_path_get_basename(values[0]);
	fu_release_set_firmware_basename(release, firmware_basename);

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* get device */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_UPDATABLE;
	if (g_strv_length(values) >= 2) {
		device = fu_util_get_device(self, values[1], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device(self, NULL, error);
		if (device == NULL)
			return FALSE;
	}

	/* optional version */
	if (g_strv_length(values) >= 3)
		fu_release_set_version(release, values[2]);

	self->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-changed",
			 G_CALLBACK(fu_util_update_device_changed_cb),
			 self);

	/* write bare firmware */
	if (self->prepare_blob) {
		g_autoptr(GPtrArray) devices = NULL;
		devices = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
		g_ptr_array_add(devices, g_object_ref(device));
		if (!fu_engine_composite_prepare(self->engine, devices, error)) {
			g_prefix_error(error, "failed to prepare composite action: ");
			return FALSE;
		}
	}
	self->flags |= FWUPD_INSTALL_FLAG_NO_HISTORY;
	if (!fu_engine_install_blob(self->engine,
				    device,
				    release,
				    fu_progress_get_child(self->progress),
				    self->flags,
				    fu_engine_request_get_feature_flags(self->request),
				    error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* cleanup */
	if (self->cleanup_blob) {
		g_autoptr(FuDevice) device_new = NULL;
		g_autoptr(GError) error_local = NULL;

		/* get the possibly new device from the old ID */
		device_new = fu_util_get_device(self, fu_device_get_id(device), &error_local);
		if (device_new == NULL) {
			g_debug("failed to find new device: %s", error_local->message);
		} else {
			g_autoptr(GPtrArray) devices_new =
			    g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
			g_ptr_array_add(devices_new, g_steal_pointer(&device_new));
			if (!fu_engine_composite_cleanup(self->engine, devices_new, error)) {
				g_prefix_error(error, "failed to cleanup composite action: ");
				return FALSE;
			}
		}
	}

	fu_util_display_current_message(self);

	/* success */
	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_firmware_sign(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuCabinet) cabinet = fu_cabinet_new();
	g_autoptr(GBytes) archive_blob_new = NULL;
	g_autoptr(GBytes) cert = NULL;
	g_autoptr(GBytes) privkey = NULL;
	g_autoptr(GFile) archive_file_old = NULL;

	/* invalid args */
	if (g_strv_length(values) != 3) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments, expected firmware.cab "
				    "certificate.pem privatekey.pfx");
		return FALSE;
	}

	/* load arguments */
	cert = fu_bytes_get_contents(values[1], error);
	if (cert == NULL)
		return FALSE;
	privkey = fu_bytes_get_contents(values[2], error);
	if (privkey == NULL)
		return FALSE;

	/* load, sign, export */
	archive_file_old = g_file_new_for_path(values[0]);
	if (!fu_firmware_parse_file(FU_FIRMWARE(cabinet),
				    archive_file_old,
				    FU_FIRMWARE_PARSE_FLAG_CACHE_STREAM,
				    error))
		return FALSE;
	if (!fu_cabinet_sign(cabinet, cert, privkey, FU_CABINET_SIGN_FLAG_NONE, error))
		return FALSE;
	archive_blob_new = fu_firmware_write(FU_FIRMWARE(cabinet), error);
	if (archive_blob_new == NULL)
		return FALSE;
	return fu_bytes_set_contents(values[0], archive_blob_new, error);
}

static gboolean
fu_util_firmware_dump(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GBytes) blob_empty = g_bytes_new(NULL, 0);
	g_autoptr(GBytes) blob_fw = NULL;

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_flag(self->progress, FU_PROGRESS_FLAG_NO_PROFILE);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 5, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_READ, 95, NULL);

	/* invalid args */
	if (g_strv_length(values) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* file already exists */
	if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    g_file_test(values[0], G_FILE_TEST_EXISTS)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Filename already exists");
		return FALSE;
	}

	/* write a zero length file to ensure the destination is writable to
	 * avoid failing at the end of a potentially lengthy operation */
	if (!fu_bytes_set_contents(values[0], blob_empty, error))
		return FALSE;

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* get device */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE;
	if (g_strv_length(values) >= 2) {
		device = fu_util_get_device(self, values[1], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device(self, NULL, error);
		if (device == NULL)
			return FALSE;
	}
	self->current_operation = FU_UTIL_OPERATION_READ;
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-changed",
			 G_CALLBACK(fu_util_update_device_changed_cb),
			 self);

	/* dump firmware */
	blob_fw = fu_engine_firmware_dump(self->engine,
					  device,
					  fu_progress_get_child(self->progress),
					  self->flags,
					  error);
	if (blob_fw == NULL)
		return FALSE;
	fu_progress_step_done(self->progress);
	return fu_bytes_set_contents(values[0], blob_fw, error);
}

static gboolean
fu_util_firmware_read(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuFirmware) fw = NULL;
	g_autoptr(GBytes) blob_empty = g_bytes_new(NULL, 0);
	g_autoptr(GBytes) blob_fw = NULL;

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_flag(self->progress, FU_PROGRESS_FLAG_NO_PROFILE);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 5, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_READ, 95, NULL);

	/* invalid args */
	if (g_strv_length(values) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* file already exists */
	if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    g_file_test(values[0], G_FILE_TEST_EXISTS)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Filename already exists");
		return FALSE;
	}

	/* write a zero length file to ensure the destination is writable to
	 * avoid failing at the end of a potentially lengthy operation */
	if (!fu_bytes_set_contents(values[0], blob_empty, error))
		return FALSE;

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* get device */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE;
	if (g_strv_length(values) >= 2) {
		device = fu_util_get_device(self, values[1], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device(self, NULL, error);
		if (device == NULL)
			return FALSE;
	}
	self->current_operation = FU_UTIL_OPERATION_READ;
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-changed",
			 G_CALLBACK(fu_util_update_device_changed_cb),
			 self);

	/* read firmware into the container format */
	fw = fu_engine_firmware_read(self->engine,
				     device,
				     fu_progress_get_child(self->progress),
				     self->flags,
				     error);
	if (fw == NULL)
		return FALSE;
	blob_fw = fu_firmware_write(fw, error);
	if (blob_fw == NULL)
		return FALSE;
	fu_progress_step_done(self->progress);
	return fu_bytes_set_contents(values[0], blob_fw, error);
}

static gint
fu_util_release_sort_cb(gconstpointer a, gconstpointer b)
{
	FuRelease *release1 = *((FuRelease **)a);
	FuRelease *release2 = *((FuRelease **)b);
	return fu_release_compare(release1, release2);
}

static gchar *
fu_util_download_if_required(FuUtil *self, const gchar *perhapsfn, GError **error)
{
	g_autofree gchar *filename = NULL;
	g_autoptr(GFile) file = NULL;

	/* a local file */
	if (g_file_test(perhapsfn, G_FILE_TEST_EXISTS))
		return g_strdup(perhapsfn);
	if (!fu_util_is_url(perhapsfn))
		return g_strdup(perhapsfn);

	/* download the firmware to a cachedir */
	filename = fu_util_get_user_cache_path(perhapsfn);
	if (!fu_path_mkdir_parent(filename, error))
		return NULL;
	file = g_file_new_for_path(filename);
	if (!fwupd_client_download_file(self->client,
					perhapsfn,
					file,
					FWUPD_CLIENT_DOWNLOAD_FLAG_NONE,
					self->cancellable,
					error))
		return NULL;
	return g_steal_pointer(&filename);
}

static gboolean
fu_util_install_stream(FuUtil *self,
		       GInputStream *stream,
		       GPtrArray *devices,
		       FuProgress *progress,
		       GError **error)
{
	g_autoptr(FuCabinet) cabinet = NULL;
	g_autoptr(GPtrArray) components = NULL;
	g_autoptr(GPtrArray) errors = NULL;
	g_autoptr(GPtrArray) releases = NULL;

	cabinet = fu_engine_build_cabinet_from_stream(self->engine, stream, error);
	if (cabinet == NULL)
		return FALSE;
	components = fu_cabinet_get_components(cabinet, error);
	if (components == NULL)
		return FALSE;

	/* for each component in the silo */
	errors = g_ptr_array_new_with_free_func((GDestroyNotify)g_error_free);
	releases = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	for (guint i = 0; i < components->len; i++) {
		XbNode *component = g_ptr_array_index(components, i);

		/* do any devices pass the requirements */
		for (guint j = 0; j < devices->len; j++) {
			FuDevice *device = g_ptr_array_index(devices, j);
			g_autoptr(FuRelease) release = fu_release_new();
			g_autoptr(GError) error_local = NULL;

			/* is this component valid for the device */
			fu_release_set_device(release, device);
			fu_release_set_request(release, self->request);
			if (!fu_engine_load_release(self->engine,
						    release,
						    cabinet,
						    component,
						    NULL,
						    self->flags,
						    &error_local)) {
				g_debug("loading release failed on %s:%s failed: %s",
					fu_device_get_id(device),
					xb_node_query_text(component, "id", NULL),
					error_local->message);
				g_ptr_array_add(errors, g_steal_pointer(&error_local));
				continue;
			}
			if (!fu_engine_requirements_check(self->engine,
							  release,
							  self->flags,
							  &error_local)) {
				g_debug("requirement on %s:%s failed: %s",
					fu_device_get_id(device),
					xb_node_query_text(component, "id", NULL),
					error_local->message);
				g_ptr_array_add(errors, g_steal_pointer(&error_local));
				continue;
			}

			/* if component should have an update message from CAB */
			fu_device_ensure_from_component(device, component);
			fu_device_incorporate_from_component(device, component);

			/* success */
			g_ptr_array_add(releases, g_steal_pointer(&release));
		}
	}

	/* order the install tasks by the device priority */
	g_ptr_array_sort(releases, fu_util_release_sort_cb);

	/* nothing suitable */
	if (releases->len == 0) {
		GError *error_tmp = fu_engine_error_array_get_best(errors);
		g_propagate_error(error, error_tmp);
		return FALSE;
	}

	self->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-changed",
			 G_CALLBACK(fu_util_update_device_changed_cb),
			 self);

	/* install all the tasks */
	return fu_engine_install_releases(self->engine,
					  self->request,
					  releases,
					  cabinet,
					  fu_progress_get_child(self->progress),
					  self->flags,
					  error);
}

static gboolean
fu_util_install(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *filename = NULL;
	g_autoptr(GInputStream) stream = NULL;
	g_autoptr(GPtrArray) devices_possible = NULL;

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_flag(self->progress, FU_PROGRESS_FLAG_NO_PROFILE);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 50, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_WRITE, 50, NULL);

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* handle both forms */
	if (g_strv_length(values) == 1) {
		devices_possible = fu_engine_get_devices(self->engine, error);
		if (devices_possible == NULL)
			return FALSE;
		fwupd_device_array_ensure_parents(devices_possible);
	} else if (g_strv_length(values) == 2) {
		FuDevice *device = fu_util_get_device(self, values[1], error);
		if (device == NULL)
			return FALSE;
		if (!self->no_safety_check) {
			if (!fu_util_prompt_warning_fde(self->console, FWUPD_DEVICE(device), error))
				return FALSE;
		}
		devices_possible =
		    fu_engine_get_devices_by_composite_id(self->engine,
							  fu_device_get_composite_id(device),
							  error);
		if (devices_possible == NULL)
			return FALSE;

		g_ptr_array_add(devices_possible, device);
	} else {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* download if required */
	filename = fu_util_download_if_required(self, values[0], error);
	if (filename == NULL)
		return FALSE;
	stream = fu_input_stream_from_path(filename, error);
	if (stream == NULL) {
		fu_util_maybe_prefix_sandbox_error(filename, error);
		return FALSE;
	}
	if (!fu_util_install_stream(self,
				    stream,
				    devices_possible,
				    fu_progress_get_child(self->progress),
				    error))
		return FALSE;
	fu_progress_step_done(self->progress);

	fu_util_display_current_message(self);

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_install_release(FuUtil *self, FwupdDevice *dev, FwupdRelease *rel, GError **error)
{
	FwupdRemote *remote;
	GPtrArray *locations;
	const gchar *remote_id;
	const gchar *uri_tmp;
	g_auto(GStrv) argv = NULL;

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

	/* get the default release only until other parts of fwupd can cope */
	locations = fwupd_release_get_locations(rel);
	if (locations->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "release missing URI");
		return FALSE;
	}
	uri_tmp = g_ptr_array_index(locations, 0);
	remote_id = fwupd_release_get_remote_id(rel);
	if (remote_id == NULL) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_FILE,
			    "failed to find remote for %s",
			    uri_tmp);
		return FALSE;
	}

	remote = fu_engine_get_remote_by_id(self->engine, remote_id, error);
	if (remote == NULL)
		return FALSE;

	argv = g_new0(gchar *, 2);
	/* local remotes may have the firmware already */
	if (fwupd_remote_get_kind(remote) == FWUPD_REMOTE_KIND_LOCAL && !fu_util_is_url(uri_tmp)) {
		const gchar *fn_cache = fwupd_remote_get_filename_cache(remote);
		g_autofree gchar *path = g_path_get_dirname(fn_cache);
		argv[0] = g_build_filename(path, uri_tmp, NULL);
	} else if (fwupd_remote_get_kind(remote) == FWUPD_REMOTE_KIND_DIRECTORY) {
		argv[0] = g_strdup(uri_tmp + 7);
		/* web remote, fu_util_install will download file */
	} else {
		argv[0] = fwupd_remote_build_firmware_uri(remote, uri_tmp, error);
	}

	/* reset progress before reusing it. */
	fu_progress_reset(self->progress);

	return fu_util_install(self, argv, error);
}

static gboolean
fu_util_update(FuUtil *self, gchar **values, GError **error)
{
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

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

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

	self->current_operation = FU_UTIL_OPERATION_UPDATE;

	devices = fu_engine_get_devices(self->engine, error);
	if (devices == NULL)
		return FALSE;
	fwupd_device_array_ensure_parents(devices);
	g_ptr_array_sort(devices, fu_util_sort_devices_by_flags_cb);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		FwupdRelease *rel;
		const gchar *device_id = fu_device_get_id(dev);
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;
		gboolean dev_skip_byid = TRUE;

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
		if (!fu_util_is_interesting_device(devices, dev))
			continue;

		/* only show stuff that has metadata available */
		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE) &&
		    !fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN))
			continue;
		if (!fwupd_device_has_flag(dev, FWUPD_DEVICE_FLAG_SUPPORTED)) {
			g_ptr_array_add(devices_unsupported, dev);
			continue;
		}
		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;

		rels = fu_engine_get_upgrades(self->engine, self->request, device_id, &error_local);
		if (rels == NULL) {
			g_ptr_array_add(devices_latest, dev);
			/* discard the actual reason from user, but leave for debugging */
			g_debug("%s", error_local->message);
			continue;
		}

		/* something is wrong */
		if (fwupd_device_get_problems(dev) != FWUPD_DEVICE_PROBLEM_NONE) {
			g_ptr_array_add(devices_pending, dev);
			continue;
		}

		rel = g_ptr_array_index(rels, 0);
		if (!self->no_safety_check) {
			g_autofree gchar *title =
			    g_strdup_printf("%s %s",
					    fu_engine_get_host_vendor(self->engine),
					    fu_engine_get_host_product(self->engine));
			if (!fu_util_prompt_warning(self->console, dev, rel, title, error))
				return FALSE;
			if (!fu_util_prompt_warning_fde(self->console, dev, error))
				return FALSE;
		}

		if (!fu_util_install_release(self, dev, rel, &error_local)) {
			fu_console_print_literal(self->console, error_local->message);
			continue;
		}
		fu_util_display_current_message(self);
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

	/* we don't want to ask anything */
	if (self->no_reboot_check || self->as_json) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_reinstall(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;
	g_autoptr(FuDevice) dev = NULL;

	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	dev = fu_util_get_device(self, values[0], error);
	if (dev == NULL)
		return FALSE;

	/* try to lookup/match release from client */
	rels = fu_engine_get_releases_for_device(self->engine, self->request, dev, error);
	if (rels == NULL)
		return FALSE;

	for (guint j = 0; j < rels->len; j++) {
		FwupdRelease *rel_tmp = g_ptr_array_index(rels, j);
		if (!fwupd_release_match_flags(rel_tmp,
					       self->filter_release_include,
					       self->filter_release_exclude))
			continue;
		if (fu_version_compare(fwupd_release_get_version(rel_tmp),
				       fu_device_get_version(dev),
				       fu_device_get_version_format(dev)) == 0) {
			rel = g_object_ref(rel_tmp);
			break;
		}
	}
	if (rel == NULL) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "Unable to locate release for %s version %s",
			    fu_device_get_name(dev),
			    fu_device_get_version(dev));
		return FALSE;
	}

	/* update the console if composite devices are also updated */
	self->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-changed",
			 G_CALLBACK(fu_util_update_device_changed_cb),
			 self);
	self->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	if (!fu_util_install_release(self, FWUPD_DEVICE(dev), rel, error))
		return FALSE;
	fu_util_display_current_message(self);

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_detach(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 95, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_BUSY, 5, NULL);

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* get device */
	self->filter_device_exclude |= FWUPD_DEVICE_FLAG_IS_BOOTLOADER;
	if (g_strv_length(values) >= 1) {
		device = fu_util_get_device(self, values[0], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device(self, NULL, error);
		if (device == NULL)
			return FALSE;
	}

	/* run vfunc */
	locker = fu_device_locker_new(device, error);
	if (locker == NULL)
		return FALSE;
	if (!fu_device_detach_full(device, fu_progress_get_child(self->progress), error))
		return FALSE;
	fu_progress_step_done(self->progress);
	return TRUE;
}

static gboolean
fu_util_unbind_driver(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* get device */
	if (g_strv_length(values) == 1) {
		device = fu_util_get_device(self, values[0], error);
	} else {
		device = fu_util_prompt_for_device(self, NULL, error);
	}
	if (device == NULL)
		return FALSE;

	/* run vfunc */
	locker = fu_device_locker_new(device, error);
	if (locker == NULL)
		return FALSE;
	return fu_device_unbind_driver(device, error);
}

static gboolean
fu_util_bind_driver(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* get device */
	if (g_strv_length(values) == 3) {
		device = fu_util_get_device(self, values[2], error);
		if (device == NULL)
			return FALSE;
	} else if (g_strv_length(values) == 2) {
		device = fu_util_prompt_for_device(self, NULL, error);
		if (device == NULL)
			return FALSE;
	} else {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* run vfunc */
	locker = fu_device_locker_new(device, error);
	if (locker == NULL)
		return FALSE;
	return fu_device_bind_driver(device, values[0], values[1], error);
}

static gboolean
fu_util_attach(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 95, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_BUSY, 5, NULL);

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* get device */
	if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0)
		self->filter_device_include |= FWUPD_DEVICE_FLAG_IS_BOOTLOADER;
	if (g_strv_length(values) >= 1) {
		device = fu_util_get_device(self, values[0], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device(self, NULL, error);
		if (device == NULL)
			return FALSE;
	}

	/* run vfunc */
	locker = fu_device_locker_new(device, error);
	if (locker == NULL)
		return FALSE;
	if (!fu_device_attach_full(device, fu_progress_get_child(self->progress), error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* success */
	return TRUE;
}

static void
fu_util_report_metadata_to_string(GHashTable *metadata, guint idt, GString *str)
{
	g_autoptr(GList) keys =
	    g_list_sort(g_hash_table_get_keys(metadata), (GCompareFunc)g_strcmp0);
	for (GList *l = keys; l != NULL; l = l->next) {
		const gchar *key = l->data;
		const gchar *value = g_hash_table_lookup(metadata, key);
		fwupd_codec_string_append(str, idt, key, value);
	}
}

static gboolean
fu_util_get_report_metadata_as_json(FuUtil *self, JsonBuilder *builder, GError **error)
{
	GPtrArray *plugins;
	g_autoptr(GHashTable) metadata = NULL;
	g_autoptr(GPtrArray) devices = NULL;

	/* daemon metadata */
	metadata = fu_engine_get_report_metadata(self->engine, error);
	if (metadata == NULL)
		return FALSE;
	fwupd_codec_json_append_map(builder, "daemon", metadata);

	/* device metadata */
	devices = fu_engine_get_devices(self->engine, error);
	if (devices == NULL)
		return FALSE;
	json_builder_set_member_name(builder, "devices");
	json_builder_begin_array(builder);
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index(devices, i);
		g_autoptr(FuDeviceLocker) locker = NULL;
		g_autoptr(GHashTable) metadata_post = NULL;
		g_autoptr(GHashTable) metadata_pre = NULL;

		locker = fu_device_locker_new(device, error);
		if (locker == NULL)
			return FALSE;
		metadata_pre = fu_device_report_metadata_pre(device);
		metadata_post = fu_device_report_metadata_post(device);
		if (metadata_pre == NULL && metadata_post == NULL)
			continue;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, fu_device_get_id(device));
		json_builder_begin_array(builder);
		if (metadata_pre != NULL) {
			json_builder_begin_object(builder);
			fwupd_codec_json_append_map(builder, "pre", metadata_pre);
			json_builder_end_object(builder);
		}
		if (metadata_post != NULL) {
			json_builder_begin_object(builder);
			fwupd_codec_json_append_map(builder, "post", metadata_post);
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	/* plugin metadata */
	plugins = fu_engine_get_plugins(self->engine);
	json_builder_set_member_name(builder, "plugins");
	json_builder_begin_array(builder);
	for (guint i = 0; i < plugins->len; i++) {
		FuPlugin *plugin = g_ptr_array_index(plugins, i);
		if (fu_plugin_has_flag(plugin, FWUPD_PLUGIN_FLAG_DISABLED))
			continue;
		if (fu_plugin_get_report_metadata(plugin) == NULL)
			continue;
		json_builder_begin_object(builder);
		fwupd_codec_json_append_map(builder,
					    fu_plugin_get_name(plugin),
					    fu_plugin_get_report_metadata(plugin));
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	/* success */
	return TRUE;
}

static gboolean
fu_util_get_report_metadata(FuUtil *self, gchar **values, GError **error)
{
	GPtrArray *plugins;
	g_autoptr(GHashTable) metadata = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GString) str = g_string_new(NULL);

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 95, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_BUSY, 5, NULL);

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* not for human consumption */
	if (self->as_json) {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		json_builder_begin_object(builder);
		if (!fu_util_get_report_metadata_as_json(self, builder, error))
			return FALSE;
		json_builder_end_object(builder);
		return fu_util_print_builder(self->console, builder, error);
	}

	/* daemon metadata */
	metadata = fu_engine_get_report_metadata(self->engine, error);
	if (metadata == NULL)
		return FALSE;
	fu_util_report_metadata_to_string(metadata, 0, str);

	/* device metadata */
	devices = fu_engine_get_devices(self->engine, error);
	if (devices == NULL)
		return FALSE;
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index(devices, i);
		g_autoptr(FuDeviceLocker) locker = NULL;
		g_autoptr(GHashTable) metadata_post = NULL;
		g_autoptr(GHashTable) metadata_pre = NULL;

		locker = fu_device_locker_new(device, error);
		if (locker == NULL)
			return FALSE;
		metadata_pre = fu_device_report_metadata_pre(device);
		metadata_post = fu_device_report_metadata_post(device);
		if (metadata_pre != NULL || metadata_post != NULL) {
			fwupd_codec_string_append(str,
						  0,
						  FWUPD_RESULT_KEY_DEVICE_ID,
						  fu_device_get_id(device));
		}
		if (metadata_pre != NULL) {
			fwupd_codec_string_append(str, 1, "pre", "");
			fu_util_report_metadata_to_string(metadata_pre, 3, str);
		}
		if (metadata_post != NULL) {
			fwupd_codec_string_append(str, 1, "post", "");
			fu_util_report_metadata_to_string(metadata_post, 3, str);
		}
	}

	/* plugin metadata */
	plugins = fu_engine_get_plugins(self->engine);
	for (guint i = 0; i < plugins->len; i++) {
		FuPlugin *plugin = g_ptr_array_index(plugins, i);
		if (fu_plugin_has_flag(plugin, FWUPD_PLUGIN_FLAG_DISABLED))
			continue;
		if (fu_plugin_get_report_metadata(plugin) == NULL)
			continue;
		fwupd_codec_string_append(str, 1, fu_plugin_get_name(plugin), "");
		fu_util_report_metadata_to_string(fu_plugin_get_report_metadata(plugin), 3, str);
	}
	fu_progress_step_done(self->progress);

	/* display */
	fu_console_print_literal(self->console, str->str);

	/* success */
	return TRUE;
}

static gboolean
fu_util_modify_config(FuUtil *self, gchar **values, GError **error)
{
	/* start engine */
	if (!fu_util_start_engine(self, FU_ENGINE_LOAD_FLAG_HWINFO, self->progress, error))
		return FALSE;

	/* check args */
	if (g_strv_length(values) == 3) {
		if (!fu_engine_modify_config(self->engine, values[0], values[1], values[2], error))
			return FALSE;
	} else if (g_strv_length(values) == 2) {
		if (!fu_engine_modify_config(self->engine, "fwupd", values[0], values[1], error))
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
				    "Invalid arguments: SECTION");
		return FALSE;
	}

	/* start engine */
	if (!fu_util_start_engine(self, FU_ENGINE_LOAD_FLAG_NONE, self->progress, error))
		return FALSE;

	if (!fu_engine_reset_config(self->engine, values[0], error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: success message -- a per-system setting value */
	fu_console_print_literal(self->console, _("Successfully reset configuration section"));
	return TRUE;
}

static gboolean
fu_util_remote_modify(FuUtil *self, gchar **values, GError **error)
{
	FwupdRemote *remote = NULL;

	if (g_strv_length(values) < 3) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	remote = fu_engine_get_remote_by_id(self->engine, values[0], error);
	if (remote == NULL)
		return FALSE;

	if (!fu_engine_modify_remote(self->engine,
				     fwupd_remote_get_id(remote),
				     values[1],
				     values[2],
				     error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	fu_console_print_literal(self->console, _("Successfully modified remote"));
	return TRUE;
}

static gboolean
fu_util_remote_disable(FuUtil *self, gchar **values, GError **error)
{
	FwupdRemote *remote = NULL;

	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	if (!fu_util_start_engine(self, FU_ENGINE_LOAD_FLAG_REMOTES, self->progress, error))
		return FALSE;

	remote = fu_engine_get_remote_by_id(self->engine, values[0], error);
	if (remote == NULL)
		return FALSE;

	if (!fu_engine_modify_remote(self->engine,
				     fwupd_remote_get_id(remote),
				     "Enabled",
				     "false",
				     error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	fu_console_print_literal(self->console, _("Successfully disabled remote"));
	return TRUE;
}

static gboolean
fu_util_vercmp(FuUtil *self, gchar **values, GError **error)
{
	FwupdVersionFormat verfmt = FWUPD_VERSION_FORMAT_UNKNOWN;
	gint rc;

	/* sanity check */
	if (g_strv_length(values) < 2) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments, expected VER1 VER2");
		return FALSE;
	}

	/* optional version format */
	if (g_strv_length(values) > 2) {
		verfmt = fwupd_version_format_from_string(values[2]);
		if (verfmt == FWUPD_VERSION_FORMAT_UNKNOWN) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Version format %s not supported",
				    values[2]);
			return FALSE;
		}
	}

	/* compare */
	rc = fu_version_compare(values[0], values[1], verfmt);
	if (rc > 0) {
		fu_console_print(self->console, "%s > %s", values[0], values[1]);
	} else if (rc < 0) {
		fu_console_print(self->console, "%s < %s", values[0], values[1]);
	} else {
		fu_console_print(self->console, "%s == %s", values[0], values[1]);
	}
	return TRUE;
}

static gboolean
fu_util_remote_enable(FuUtil *self, gchar **values, GError **error)
{
	FwupdRemote *remote = NULL;

	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	if (!fu_util_start_engine(self, FU_ENGINE_LOAD_FLAG_REMOTES, self->progress, error))
		return FALSE;

	remote = fu_engine_get_remote_by_id(self->engine, values[0], error);
	if (remote == NULL)
		return FALSE;

	if (!fu_util_modify_remote_warning(self->console, remote, FALSE, error))
		return FALSE;

	if (!fu_engine_modify_remote(self->engine,
				     fwupd_remote_get_id(remote),
				     "Enabled",
				     "true",
				     error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	fu_console_print_literal(self->console, _("Successfully enabled remote"));
	return TRUE;
}

static gboolean
fu_util_set_test_devices_enabled(FuUtil *self, gboolean enable, GError **error)
{
	return fu_engine_modify_config(self->engine,
				       "fwupd",
				       "TestDevices",
				       enable ? "true" : "false",
				       error);
}

static gboolean
fu_util_disable_test_devices(FuUtil *self, gchar **values, GError **error)
{
	if (!fu_util_start_engine(self, FU_ENGINE_LOAD_FLAG_HWINFO, self->progress, error))
		return FALSE;

	if (!fu_util_set_test_devices_enabled(self, FALSE, error))
		return FALSE;

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: comment explaining result of command */
	fu_console_print_literal(self->console, _("Successfully disabled test devices"));

	return TRUE;
}

static gboolean
fu_util_enable_test_devices(FuUtil *self, gchar **values, GError **error)
{
	gboolean found = FALSE;
	g_autoptr(GPtrArray) remotes = NULL;

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	if (!fu_util_set_test_devices_enabled(self, TRUE, error))
		return FALSE;

	/* verify remote is present */
	remotes = fu_engine_get_remotes(self->engine, error);
	if (remotes == NULL)
		return FALSE;
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index(remotes, i);
		if (!fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_ENABLED))
			continue;
		if (g_strcmp0(fwupd_remote_get_id(remote), "fwupd-tests") == 0) {
			found = TRUE;
			break;
		}
	}
	if (!found) {
		if (!fu_util_set_test_devices_enabled(self, FALSE, error))
			return FALSE;
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "failed to enable fwupd-tests remote");
		return FALSE;
	}

	if (self->as_json)
		return TRUE;

	/* TRANSLATORS: comment explaining result of command */
	fu_console_print_literal(self->console, _("Successfully enabled test devices"));

	return TRUE;
}

static gboolean
fu_util_check_activation_needed(FuUtil *self, GError **error)
{
	gboolean has_pending = FALSE;
	g_autoptr(FuHistory) history = fu_history_new(self->ctx);
	g_autoptr(GPtrArray) devices = fu_history_get_devices(history, error);
	if (devices == NULL)
		return FALSE;

	/* only start up the plugins needed */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *dev = g_ptr_array_index(devices, i);
		if (fu_device_has_flag(dev, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)) {
			fu_engine_add_plugin_filter(self->engine, fu_device_get_plugin(dev));
			has_pending = TRUE;
		}
	}

	if (!has_pending) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "No devices to activate");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_util_activate(FuUtil *self, gchar **values, GError **error)
{
	gboolean has_pending = FALSE;
	g_autoptr(GPtrArray) devices = NULL;

	/* check the history database before starting the daemon */
	if (!fu_util_check_activation_needed(self, error))
		return FALSE;

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 95, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_BUSY, 5, NULL);

	/* load engine */
	if (!fu_util_start_engine(
		self,
		FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_COLDPLUG |
		    FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG | FU_ENGINE_LOAD_FLAG_REMOTES |
		    FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS | FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS |
		    FU_ENGINE_LOAD_FLAG_HWINFO,
		fu_progress_get_child(self->progress),
		error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* parse arguments */
	if (g_strv_length(values) == 0) {
		devices = fu_engine_get_devices(self->engine, error);
		if (devices == NULL)
			return FALSE;
	} else if (g_strv_length(values) == 1) {
		FuDevice *device;
		device = fu_util_get_device(self, values[0], error);
		if (device == NULL)
			return FALSE;
		devices = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
		g_ptr_array_add(devices, device);
	} else {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments");
		return FALSE;
	}

	/* activate anything with _NEEDS_ACTIVATION */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index(devices, i);
		if (!fwupd_device_match_flags(FWUPD_DEVICE(device),
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		if (!fu_device_has_flag(device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION))
			continue;
		has_pending = TRUE;
		if (!self->as_json) {
			fu_console_print(
			    self->console,
			    "%s %s…",
			    /* TRANSLATORS: shown when shutting down to switch to the new version */
			    _("Activating firmware update"),
			    fu_device_get_name(device));
		}
		if (!fu_engine_activate(self->engine,
					fu_device_get_id(device),
					fu_progress_get_child(self->progress),
					error))
			return FALSE;
	}
	fu_progress_step_done(self->progress);

	if (!has_pending) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "No devices to activate");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_util_export_hwids(FuUtil *self, gchar **values, GError **error)
{
	FuContext *ctx = fu_engine_get_context(self->engine);
	FuHwids *hwids = fu_context_get_hwids(ctx);
	g_autoptr(GKeyFile) kf = g_key_file_new();
	g_autoptr(GPtrArray) hwid_keys = NULL;

	/* check args */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments, expected HWIDS-FILE");
		return FALSE;
	}

	/* setup default hwids */
	if (!fu_context_load_hwinfo(ctx, self->progress, FU_CONTEXT_HWID_FLAG_LOAD_ALL, error))
		return FALSE;

	/* save all keys */
	hwid_keys = fu_hwids_get_keys(hwids);
	for (guint i = 0; i < hwid_keys->len; i++) {
		const gchar *hwid_key = g_ptr_array_index(hwid_keys, i);
		const gchar *value = fu_hwids_get_value(hwids, hwid_key);
		if (value == NULL)
			continue;
		g_key_file_set_string(kf, "HwIds", hwid_key, value);
	}

	/* success */
	return g_key_file_save_to_file(kf, values[0], error);
}

static gboolean
fu_util_hwids(FuUtil *self, gchar **values, GError **error)
{
	FuContext *ctx = fu_engine_get_context(self->engine);
	FuHwids *hwids = fu_context_get_hwids(ctx);
	g_autoptr(GPtrArray) chid_keys = fu_hwids_get_chid_keys(hwids);
	g_autoptr(GPtrArray) hwid_keys = fu_hwids_get_keys(hwids);

	/* a keyfile with overrides */
	if (g_strv_length(values) == 1) {
		g_autoptr(GKeyFile) kf = g_key_file_new();
		if (!g_key_file_load_from_file(kf, values[0], G_KEY_FILE_NONE, error))
			return FALSE;
		for (guint i = 0; i < hwid_keys->len; i++) {
			const gchar *hwid_key = g_ptr_array_index(hwid_keys, i);
			g_autofree gchar *tmp = NULL;
			tmp = g_key_file_get_string(kf, "HwIds", hwid_key, NULL);
			fu_hwids_add_value(hwids, hwid_key, tmp);
		}
	}
	if (!fu_context_load_hwinfo(ctx, self->progress, FU_CONTEXT_HWID_FLAG_LOAD_ALL, error))
		return FALSE;

	/* show debug output */
	fu_console_print_literal(self->console, "Computer Information");
	fu_console_print_literal(self->console, "--------------------");
	for (guint i = 0; i < hwid_keys->len; i++) {
		const gchar *hwid_key = g_ptr_array_index(hwid_keys, i);
		const gchar *value = fu_hwids_get_value(hwids, hwid_key);
		if (value == NULL)
			continue;
		if (g_strcmp0(hwid_key, FU_HWIDS_KEY_BIOS_MAJOR_RELEASE) == 0 ||
		    g_strcmp0(hwid_key, FU_HWIDS_KEY_BIOS_MINOR_RELEASE) == 0) {
			guint64 val = 0;
			if (!fu_strtoull(value, &val, 0, G_MAXUINT64, FU_INTEGER_BASE_16, error))
				return FALSE;
			fu_console_print(self->console, "%s: %" G_GUINT64_FORMAT, hwid_key, val);
		} else {
			fu_console_print(self->console, "%s: %s", hwid_key, value);
		}
	}

	/* show GUIDs */
	fu_console_print_literal(self->console, "Hardware IDs");
	fu_console_print_literal(self->console, "------------");
	for (guint i = 0; i < chid_keys->len; i++) {
		const gchar *key = g_ptr_array_index(chid_keys, i);
		const gchar *keys = NULL;
		g_autofree gchar *guid = NULL;
		g_autofree gchar *keys_str = NULL;
		g_auto(GStrv) keysv = NULL;
		g_autoptr(GError) error_local = NULL;

		/* filter */
		if (!g_str_has_prefix(key, "HardwareID"))
			continue;

		/* get the GUID */
		keys = fu_hwids_get_replace_keys(hwids, key);
		guid = fu_hwids_get_guid(hwids, key, &error_local);
		if (guid == NULL) {
			fu_console_print_literal(self->console, error_local->message);
			continue;
		}

		/* show what makes up the GUID */
		keysv = g_strsplit(keys, "&", -1);
		keys_str = g_strjoinv(" + ", keysv);
		fu_console_print(self->console, "{%s}   <- %s", guid, keys_str);
	}

	/* show extra GUIDs */
	fu_console_print_literal(self->console, "Extra Hardware IDs");
	fu_console_print_literal(self->console, "------------------");
	for (guint i = 0; i < chid_keys->len; i++) {
		const gchar *key = g_ptr_array_index(chid_keys, i);
		const gchar *keys = NULL;
		g_autofree gchar *guid = NULL;
		g_autofree gchar *keys_str = NULL;
		g_auto(GStrv) keysv = NULL;
		g_autoptr(GError) error_local = NULL;

		/* filter */
		if (g_str_has_prefix(key, "HardwareID"))
			continue;

		/* get the GUID */
		keys = fu_hwids_get_replace_keys(hwids, key);
		guid = fu_hwids_get_guid(hwids, key, &error_local);
		if (guid == NULL) {
			fu_console_print_literal(self->console, error_local->message);
			continue;
		}

		/* show what makes up the GUID */
		keysv = g_strsplit(keys, "&", -1);
		keys_str = g_strjoinv(" + ", keysv);
		fu_console_print(self->console, "{%s}   <- %s", guid, keys_str);
	}

	return TRUE;
}

static gboolean
fu_util_self_sign(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *sig = NULL;

	/* check args */
	if (g_strv_length(values) != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: value expected");
		return FALSE;
	}

	/* start engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_ENSURE_CLIENT_CERT,
				  self->progress,
				  error))
		return FALSE;
	sig = fu_engine_self_sign(self->engine,
				  values[0],
				  JCAT_SIGN_FLAG_ADD_TIMESTAMP | JCAT_SIGN_FLAG_ADD_CERT,
				  error);
	if (sig == NULL)
		return FALSE;

	if (self->as_json)
		fu_console_print(self->console, "{\"signature\": \"%s\"}", sig);
	else
		fu_console_print(self->console, "%s", sig);

	return TRUE;
}

static void
fu_util_device_added_cb(FwupdClient *client, FwupdDevice *device, gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;
	g_autofree gchar *tmp = NULL;

	if (self->as_json)
		return;

	tmp = fu_util_device_to_string(self->client, device, 0);

	/* TRANSLATORS: this is when a device is hotplugged */
	fu_console_print(self->console, "%s\n%s", _("Device added:"), tmp);
}

static void
fu_util_device_removed_cb(FwupdClient *client, FwupdDevice *device, gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;
	g_autofree gchar *tmp = NULL;

	if (self->as_json)
		return;

	tmp = fu_util_device_to_string(self->client, device, 0);

	/* TRANSLATORS: this is when a device is hotplugged */
	fu_console_print(self->console, "%s\n%s", _("Device removed:"), tmp);
}

static void
fu_util_device_changed_cb(FwupdClient *client, FwupdDevice *device, gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;
	g_autofree gchar *tmp = NULL;

	if (self->as_json)
		return;

	tmp = fu_util_device_to_string(self->client, device, 0);

	/* TRANSLATORS: this is when a device has been updated */
	fu_console_print(self->console, "%s\n%s", _("Device changed:"), tmp);
}

static void
fu_util_changed_cb(FwupdClient *client, gpointer user_data)
{
	FuUtil *self = (FuUtil *)user_data;

	if (self->as_json)
		return;

	/* TRANSLATORS: this is when the daemon state changes */
	fu_console_print_literal(self->console, _("Changed"));
}

static gboolean
fu_util_monitor(FuUtil *self, gchar **values, GError **error)
{
	/* get all the devices */
	if (!fwupd_client_connect(self->client, self->cancellable, error))
		return FALSE;

	/* watch for any hotplugged device */
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "changed",
			 G_CALLBACK(fu_util_changed_cb),
			 self);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "device-added",
			 G_CALLBACK(fu_util_device_added_cb),
			 self);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "device-removed",
			 G_CALLBACK(fu_util_device_removed_cb),
			 self);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "device-changed",
			 G_CALLBACK(fu_util_device_changed_cb),
			 self);
	g_signal_connect(G_CANCELLABLE(self->cancellable),
			 "cancelled",
			 G_CALLBACK(fu_util_cancelled_cb),
			 self);
	g_main_loop_run(self->loop);
	return TRUE;
}

static gboolean
fu_util_get_firmware_types(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) firmware_types = NULL;

	/* load engine */
	if (!fu_engine_load(self->engine,
			    FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS,
			    self->progress,
			    error))
		return FALSE;

	firmware_types = fu_context_get_firmware_gtype_ids(fu_engine_get_context(self->engine));
	for (guint i = 0; i < firmware_types->len; i++) {
		const gchar *id = g_ptr_array_index(firmware_types, i);
		fu_console_print_literal(self->console, id);
	}
	if (firmware_types->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: nothing found */
				    _("No firmware IDs found"));
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_util_get_firmware_gtypes(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GArray) firmware_types = NULL;

	/* load engine */
	if (!fu_engine_load(self->engine,
			    FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS,
			    self->progress,
			    error))
		return FALSE;

	firmware_types = fu_context_get_firmware_gtypes(fu_engine_get_context(self->engine));
	for (guint i = 0; i < firmware_types->len; i++) {
		GType gtype = g_array_index(firmware_types, GType, i);
		fu_console_print_literal(self->console, g_type_name(gtype));
	}
	if (firmware_types->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: nothing found */
				    _("No firmware found"));
		return FALSE;
	}

	return TRUE;
}

static gchar *
fu_util_prompt_for_firmware_type(FuUtil *self, GPtrArray *firmware_types, GError **error)
{
	guint idx;

	/* no detected types */
	if (firmware_types->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "No detected firmware types");
		return NULL;
	}

	/* there is no point asking */
	if (firmware_types->len == 1) {
		const gchar *id = g_ptr_array_index(firmware_types, 0);
		return g_strdup(id);
	}

	/* TRANSLATORS: this is to abort the interactive prompt */
	fu_console_print(self->console, "0.\t%s", _("Cancel"));
	for (guint i = 0; i < firmware_types->len; i++) {
		const gchar *id = g_ptr_array_index(firmware_types, i);
		fu_console_print(self->console, "%u.\t%s", i + 1, id);
	}
	/* TRANSLATORS: get interactive prompt */
	idx = fu_console_input_uint(self->console, firmware_types->len, "%s", _("Choose firmware"));
	if (idx == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "Request canceled");
		return NULL;
	}

	return g_strdup(g_ptr_array_index(firmware_types, idx - 1));
}

static gboolean
fu_util_firmware_parse(FuUtil *self, gchar **values, GError **error)
{
	FuContext *ctx = fu_engine_get_context(self->engine);
	GType gtype;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(GInputStream) stream = NULL;
	g_autofree gchar *firmware_type = NULL;
	g_autofree gchar *str = NULL;

	/* check args */
	if (g_strv_length(values) == 0 || g_strv_length(values) > 2) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: filename required");
		return FALSE;
	}

	/* load file */
	stream = fu_input_stream_from_path(values[0], error);
	if (stream == NULL)
		return FALSE;

	/* load engine */
	if (!fu_engine_load(self->engine,
			    FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS,
			    self->progress,
			    error))
		return FALSE;

	/* find the GType to use */
	if (g_strv_length(values) == 1) {
		g_autoptr(GPtrArray) firmware_types = fu_context_get_firmware_gtype_ids(ctx);
		firmware_type = fu_util_prompt_for_firmware_type(self, firmware_types, error);
		if (firmware_type == NULL)
			return FALSE;
	} else if (g_strcmp0(values[1], "auto") == 0) {
		g_autoptr(GPtrArray) gtype_ids = fu_context_get_firmware_gtype_ids(ctx);
		g_autoptr(GPtrArray) firmware_auto_types = g_ptr_array_new_with_free_func(g_free);
		for (guint i = 0; i < gtype_ids->len; i++) {
			const gchar *gtype_id = g_ptr_array_index(gtype_ids, i);
			GType gtype_tmp;
			g_autofree gchar *firmware_str = NULL;
			g_autoptr(FuFirmware) firmware_tmp = NULL;
			g_autoptr(GError) error_local = NULL;

			if (g_strcmp0(gtype_id, "raw") == 0)
				continue;
			g_debug("parsing as %s", gtype_id);
			gtype_tmp = fu_context_get_firmware_gtype_by_id(ctx, gtype_id);
			if (gtype_tmp == G_TYPE_INVALID) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_FOUND,
					    "GType %s not supported",
					    gtype_id);
				return FALSE;
			}
			firmware_tmp = g_object_new(gtype_tmp, NULL);
			if (fu_firmware_has_flag(firmware_tmp, FU_FIRMWARE_FLAG_NO_AUTO_DETECTION))
				continue;
			if (!fu_firmware_parse_stream(firmware_tmp,
						      stream,
						      0x0,
						      FU_FIRMWARE_PARSE_FLAG_NO_SEARCH,
						      &error_local)) {
				g_debug("failed to parse as %s: %s",
					gtype_id,
					error_local->message);
				continue;
			}
			firmware_str = fu_firmware_to_string(firmware_tmp);
			g_debug("parsed as %s: %s", gtype_id, firmware_str);
			g_ptr_array_add(firmware_auto_types, g_strdup(gtype_id));
		}
		firmware_type = fu_util_prompt_for_firmware_type(self, firmware_auto_types, error);
		if (firmware_type == NULL)
			return FALSE;
	} else {
		firmware_type = g_strdup(values[1]);
	}
	gtype = fu_context_get_firmware_gtype_by_id(ctx, firmware_type);
	if (gtype == G_TYPE_INVALID) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "GType %s not supported",
			    firmware_type);
		return FALSE;
	}

	/* match the behavior of the daemon as we're printing the children */
	self->parse_flags |= FU_FIRMWARE_PARSE_FLAG_CACHE_STREAM;

	/* does firmware specify an internal size */
	firmware = g_object_new(gtype, NULL);
	if (fu_firmware_has_flag(firmware, FU_FIRMWARE_FLAG_HAS_STORED_SIZE)) {
		g_autoptr(FuFirmware) firmware_linear = fu_linear_firmware_new(gtype);
		g_autoptr(GPtrArray) imgs = NULL;
		if (!fu_firmware_parse_stream(firmware_linear,
					      stream,
					      0x0,
					      self->parse_flags,
					      error))
			return FALSE;
		imgs = fu_firmware_get_images(firmware_linear);
		if (imgs->len == 1) {
			g_set_object(&firmware, g_ptr_array_index(imgs, 0));
		} else {
			g_set_object(&firmware, firmware_linear);
		}
	} else {
		if (!fu_firmware_parse_stream(firmware, stream, 0x0, self->parse_flags, error))
			return FALSE;
	}

	str = fu_firmware_to_string(firmware);
	fu_console_print_literal(self->console, str);
	return TRUE;
}

static gboolean
fu_util_firmware_export(FuUtil *self, gchar **values, GError **error)
{
	FuContext *ctx = fu_engine_get_context(self->engine);
	FuFirmwareExportFlags flags = FU_FIRMWARE_EXPORT_FLAG_NONE;
	GType gtype;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(GFile) file = NULL;
	g_autofree gchar *firmware_type = NULL;
	g_autofree gchar *str = NULL;

	/* check args */
	if (g_strv_length(values) == 0 || g_strv_length(values) > 2) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: filename required");
		return FALSE;
	}

	if (g_strv_length(values) == 2)
		firmware_type = g_strdup(values[1]);

	/* load engine */
	if (!fu_engine_load(self->engine,
			    FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS,
			    self->progress,
			    error))
		return FALSE;

	/* find the GType to use */
	if (firmware_type == NULL) {
		g_autoptr(GPtrArray) firmware_types = fu_context_get_firmware_gtype_ids(ctx);
		firmware_type = fu_util_prompt_for_firmware_type(self, firmware_types, error);
	}
	if (firmware_type == NULL)
		return FALSE;
	gtype = fu_context_get_firmware_gtype_by_id(ctx, firmware_type);
	if (gtype == G_TYPE_INVALID) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "GType %s not supported",
			    firmware_type);
		return FALSE;
	}
	firmware = g_object_new(gtype, NULL);
	file = g_file_new_for_path(values[0]);
	if (!fu_firmware_parse_file(firmware, file, self->parse_flags, error))
		return FALSE;
	if (self->show_all)
		flags |= FU_FIRMWARE_EXPORT_FLAG_INCLUDE_DEBUG;
	str = fu_firmware_export_to_xml(firmware, flags, error);
	if (str == NULL)
		return FALSE;
	fu_console_print_literal(self->console, str);
	return TRUE;
}

static gboolean
fu_util_firmware_extract(FuUtil *self, gchar **values, GError **error)
{
	FuContext *ctx = fu_engine_get_context(self->engine);
	GType gtype;
	g_autofree gchar *firmware_type = NULL;
	g_autofree gchar *str = NULL;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GPtrArray) images = NULL;

	/* check args */
	if (g_strv_length(values) == 0 || g_strv_length(values) > 2) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: filename required");
		return FALSE;
	}
	if (g_strv_length(values) == 2)
		firmware_type = g_strdup(values[1]);

	/* load engine */
	if (!fu_engine_load(self->engine,
			    FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS,
			    self->progress,
			    error))
		return FALSE;

	/* find the GType to use */
	if (firmware_type == NULL) {
		g_autoptr(GPtrArray) firmware_types = fu_context_get_firmware_gtype_ids(ctx);
		firmware_type = fu_util_prompt_for_firmware_type(self, firmware_types, error);
	}
	if (firmware_type == NULL)
		return FALSE;
	gtype = fu_context_get_firmware_gtype_by_id(ctx, firmware_type);
	if (gtype == G_TYPE_INVALID) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "GType %s not supported",
			    firmware_type);
		return FALSE;
	}
	firmware = g_object_new(gtype, NULL);
	file = g_file_new_for_path(values[0]);
	if (!fu_firmware_parse_file(firmware, file, self->parse_flags, error))
		return FALSE;
	str = fu_firmware_to_string(firmware);
	fu_console_print_literal(self->console, str);
	images = fu_firmware_get_images(firmware);
	for (guint i = 0; i < images->len; i++) {
		FuFirmware *img = g_ptr_array_index(images, i);
		g_autofree gchar *fn = NULL;
		g_autoptr(GBytes) blob_img = NULL;

		/* get raw image without generated header, footer or crc */
		blob_img = fu_firmware_get_bytes(img, error);
		if (blob_img == NULL)
			return FALSE;
		if (g_bytes_get_size(blob_img) == 0)
			continue;

		/* use suitable filename */
		if (fu_firmware_get_filename(img) != NULL) {
			fn = g_strdup(fu_firmware_get_filename(img));
		} else if (fu_firmware_get_id(img) != NULL) {
			fn = g_strdup_printf("id-%s.fw", fu_firmware_get_id(img));
		} else if (fu_firmware_get_idx(img) != 0x0) {
			fn = g_strdup_printf("idx-0x%x.fw", (guint)fu_firmware_get_idx(img));
		} else {
			fn = g_strdup_printf("img-0x%x.fw", i);
		}
		/* TRANSLATORS: decompressing images from a container firmware */
		fu_console_print(self->console, "%s : %s", _("Writing file:"), fn);
		if (!fu_bytes_set_contents(fn, blob_img, error))
			return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_firmware_build(FuUtil *self, gchar **values, GError **error)
{
	GType gtype = FU_TYPE_FIRMWARE;
	const gchar *tmp;
	g_autofree gchar *str = NULL;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(FuFirmware) firmware_dst = NULL;
	g_autoptr(GBytes) blob_dst = NULL;
	g_autoptr(GBytes) blob_src = NULL;
	g_autoptr(XbBuilder) builder = xb_builder_new();
	g_autoptr(XbBuilderSource) source = xb_builder_source_new();
	g_autoptr(XbNode) n = NULL;
	g_autoptr(XbSilo) silo = NULL;

	/* check args */
	if (g_strv_length(values) != 2) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: filename required");
		return FALSE;
	}

	/* load file */
	blob_src = fu_bytes_get_contents(values[0], error);
	if (blob_src == NULL)
		return FALSE;

	/* load engine */
	if (!fu_engine_load(self->engine,
			    FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS,
			    self->progress,
			    error))
		return FALSE;

	/* parse XML */
	if (!xb_builder_source_load_bytes(source, blob_src, XB_BUILDER_SOURCE_FLAG_NONE, error)) {
		g_prefix_error(error, "could not parse XML: ");
		fwupd_error_convert(error);
		return FALSE;
	}
	xb_builder_import_source(builder, source);
	silo = xb_builder_compile(builder, XB_BUILDER_COMPILE_FLAG_NONE, NULL, error);
	if (silo == NULL) {
		fwupd_error_convert(error);
		return FALSE;
	}

	/* create FuFirmware of specific GType */
	n = xb_silo_query_first(silo, "firmware", error);
	if (n == NULL) {
		fwupd_error_convert(error);
		return FALSE;
	}
	tmp = xb_node_get_attr(n, "gtype");
	if (tmp != NULL) {
		gtype = g_type_from_name(tmp);
		if (gtype == G_TYPE_INVALID) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "GType %s not registered",
				    tmp);
			return FALSE;
		}
	}
	tmp = xb_node_get_attr(n, "id");
	if (tmp != NULL) {
		gtype =
		    fu_context_get_firmware_gtype_by_id(fu_engine_get_context(self->engine), tmp);
		if (gtype == G_TYPE_INVALID) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "GType %s not supported",
				    tmp);
			return FALSE;
		}
	}
	firmware = g_object_new(gtype, NULL);
	if (!fu_firmware_build(firmware, n, error))
		return FALSE;

	/* write new file */
	blob_dst = fu_firmware_write(firmware, error);
	if (blob_dst == NULL)
		return FALSE;
	if (!fu_bytes_set_contents(values[1], blob_dst, error))
		return FALSE;

	/* show what we wrote */
	firmware_dst = g_object_new(gtype, NULL);
	if (!fu_firmware_parse_bytes(firmware_dst, blob_dst, 0x0, self->parse_flags, error))
		return FALSE;
	str = fu_firmware_to_string(firmware_dst);
	fu_console_print_literal(self->console, str);

	/* success */
	return TRUE;
}

static gboolean
fu_util_firmware_convert(FuUtil *self, gchar **values, GError **error)
{
	FuContext *ctx = fu_engine_get_context(self->engine);
	GType gtype_dst;
	GType gtype_src;
	g_autofree gchar *firmware_type_dst = NULL;
	g_autofree gchar *firmware_type_src = NULL;
	g_autofree gchar *str_dst = NULL;
	g_autofree gchar *str_src = NULL;
	g_autoptr(FuFirmware) firmware_dst = NULL;
	g_autoptr(FuFirmware) firmware_src = NULL;
	g_autoptr(GBytes) blob_dst = NULL;
	g_autoptr(GFile) file_src = NULL;
	g_autoptr(GPtrArray) images = NULL;

	/* check args */
	if (g_strv_length(values) < 2 || g_strv_length(values) > 4) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments: filename required");
		return FALSE;
	}

	if (g_strv_length(values) > 2)
		firmware_type_src = g_strdup(values[2]);
	if (g_strv_length(values) > 3)
		firmware_type_dst = g_strdup(values[3]);

	/* load engine */
	if (!fu_engine_load(self->engine,
			    FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS,
			    self->progress,
			    error))
		return FALSE;

	/* find the GType to use */
	if (firmware_type_src == NULL) {
		g_autoptr(GPtrArray) firmware_types = fu_context_get_firmware_gtype_ids(ctx);
		firmware_type_src = fu_util_prompt_for_firmware_type(self, firmware_types, error);
	}
	if (firmware_type_src == NULL)
		return FALSE;
	if (firmware_type_dst == NULL) {
		g_autoptr(GPtrArray) firmware_types = fu_context_get_firmware_gtype_ids(ctx);
		firmware_type_dst = fu_util_prompt_for_firmware_type(self, firmware_types, error);
	}
	if (firmware_type_dst == NULL)
		return FALSE;
	gtype_src = fu_context_get_firmware_gtype_by_id(ctx, firmware_type_src);
	if (gtype_src == G_TYPE_INVALID) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "GType %s not supported",
			    firmware_type_src);
		return FALSE;
	}
	firmware_src = g_object_new(gtype_src, NULL);
	file_src = g_file_new_for_path(values[0]);
	if (!fu_firmware_parse_file(firmware_src, file_src, self->parse_flags, error))
		return FALSE;
	gtype_dst = fu_context_get_firmware_gtype_by_id(ctx, firmware_type_dst);
	if (gtype_dst == G_TYPE_INVALID) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "GType %s not supported",
			    firmware_type_dst);
		return FALSE;
	}
	str_src = fu_firmware_to_string(firmware_src);
	fu_console_print_literal(self->console, str_src);

	/* copy images */
	firmware_dst = g_object_new(gtype_dst, NULL);
	images = fu_firmware_get_images(firmware_src);
	for (guint i = 0; i < images->len; i++) {
		FuFirmware *img = g_ptr_array_index(images, i);
		fu_firmware_add_image(firmware_dst, img);
	}

	/* copy data as fallback, preferring a binary blob to the export */
	if (images->len == 0) {
		g_autoptr(GBytes) fw = NULL;
		g_autoptr(FuFirmware) img = NULL;
		fw = fu_firmware_get_bytes(firmware_src, NULL);
		if (fw == NULL) {
			fw = fu_firmware_write(firmware_src, error);
			if (fw == NULL)
				return FALSE;
		}
		img = fu_firmware_new_from_bytes(fw);
		fu_firmware_add_image(firmware_dst, img);
	}

	/* write new file */
	blob_dst = fu_firmware_write(firmware_dst, error);
	if (blob_dst == NULL)
		return FALSE;
	if (!fu_bytes_set_contents(values[1], blob_dst, error))
		return FALSE;
	str_dst = fu_firmware_to_string(firmware_dst);
	fu_console_print_literal(self->console, str_dst);

	/* success */
	return TRUE;
}

static GBytes *
fu_util_hex_string_to_bytes(const gchar *val, GError **error)
{
	gsize valsz;
	g_autoptr(GByteArray) buf = g_byte_array_new();

	/* sanity check */
	if (val == NULL) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "nothing to parse");
		return NULL;
	}

	/* parse each hex byte */
	valsz = strlen(val);
	for (guint i = 0; i < valsz; i += 2) {
		guint8 tmp = 0;
		if (!fu_firmware_strparse_uint8_safe(val, valsz, i, &tmp, error))
			return NULL;
		fu_byte_array_append_uint8(buf, tmp);
	}
	return g_bytes_new(buf->data, buf->len);
}

static gboolean
fu_util_firmware_patch(FuUtil *self, gchar **values, GError **error)
{
	FuContext *ctx = fu_engine_get_context(self->engine);
	GType gtype;
	g_autofree gchar *firmware_type = NULL;
	g_autofree gchar *str = NULL;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(GBytes) blob_dst = NULL;
	g_autoptr(GBytes) patch = NULL;
	g_autoptr(GFile) file_src = NULL;
	guint64 offset = 0;

	/* check args */
	if (g_strv_length(values) != 3 && g_strv_length(values) != 4) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_ARGS,
			    "Invalid arguments, expected %s",
			    "FILENAME OFFSET DATA [FIRMWARE-TYPE]");
		return FALSE;
	}

	/* hardcoded */
	if (g_strv_length(values) == 4)
		firmware_type = g_strdup(values[3]);

	/* parse offset */
	if (!fu_strtoull(values[1], &offset, 0x0, G_MAXUINT32, FU_INTEGER_BASE_AUTO, error)) {
		g_prefix_error(error, "failed to parse offset: ");
		return FALSE;
	}

	/* parse blob */
	patch = fu_util_hex_string_to_bytes(values[2], error);
	if (patch == NULL)
		return FALSE;
	if (g_bytes_get_size(patch) == 0) {
		g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_ARGS, "no data provided");
		return FALSE;
	}

	/* load engine */
	if (!fu_engine_load(self->engine,
			    FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS,
			    self->progress,
			    error))
		return FALSE;

	/* find the GType to use */
	if (firmware_type == NULL) {
		g_autoptr(GPtrArray) firmware_types = fu_context_get_firmware_gtype_ids(ctx);
		firmware_type = fu_util_prompt_for_firmware_type(self, firmware_types, error);
	}
	if (firmware_type == NULL)
		return FALSE;
	gtype = fu_context_get_firmware_gtype_by_id(ctx, firmware_type);
	if (gtype == G_TYPE_INVALID) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "GType %s not supported",
			    firmware_type);
		return FALSE;
	}
	firmware = g_object_new(gtype, NULL);
	file_src = g_file_new_for_path(values[0]);
	if (!fu_firmware_parse_file(firmware, file_src, self->parse_flags, error))
		return FALSE;

	/* add patch */
	fu_firmware_add_patch(firmware, offset, patch);

	/* write new file */
	blob_dst = fu_firmware_write(firmware, error);
	if (blob_dst == NULL)
		return FALSE;
	if (!fu_bytes_set_contents(values[0], blob_dst, error))
		return FALSE;
	str = fu_firmware_to_string(firmware);
	fu_console_print_literal(self->console, str);

	/* success */
	return TRUE;
}

static gboolean
fu_util_verify_update(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) dev = NULL;

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 50, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_VERIFY, 50, "verify-update");

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* get device */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_UPDATABLE;
	if (g_strv_length(values) == 1) {
		dev = fu_util_get_device(self, values[0], error);
		if (dev == NULL)
			return FALSE;
	} else {
		dev = fu_util_prompt_for_device(self, NULL, error);
		if (dev == NULL)
			return FALSE;
	}

	/* add checksums */
	if (!fu_engine_verify_update(self->engine,
				     fu_device_get_id(dev),
				     fu_progress_get_child(self->progress),
				     error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* show checksums */
	str = fu_device_to_string(dev);
	fu_console_print_literal(self->console, str);
	return TRUE;
}

static gboolean
fu_util_get_history(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(FuUtilNode) root = g_node_new(NULL);

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_REMOTES |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* get all devices from the history database */
	devices = fu_engine_get_history(self->engine, error);
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
		g_autoptr(GPtrArray) rels = NULL;
		FwupdDevice *dev = g_ptr_array_index(devices, i);
		FwupdRelease *rel;
		const gchar *remote;
		FuUtilNode *child;
		g_autoptr(GError) error_local = NULL;

		if (!fwupd_device_match_flags(dev,
					      self->filter_device_include,
					      self->filter_device_exclude))
			continue;
		child = g_node_append_data(root, g_object_ref(dev));

		rel = fwupd_device_get_release_default(dev);
		if (rel == NULL)
			continue;
		remote = fwupd_release_get_remote_id(rel);

		/* doesn't actually map to remote */
		if (remote == NULL) {
			g_node_append_data(child, g_object_ref(rel));
			continue;
		}

		/* try to lookup releases from client, falling back to the history release */
		rels = fu_engine_get_releases(self->engine,
					      self->request,
					      fwupd_device_get_id(dev),
					      &error_local);
		if (rels == NULL) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND)) {
				rels = g_ptr_array_new();
				g_ptr_array_add(rels, fwupd_device_get_release_default(dev));
			} else {
				g_propagate_error(error, g_steal_pointer(&error_local));
				return FALSE;
			}
		}

		/* map to a release in client */
		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel2 = g_ptr_array_index(rels, j);
			if (!fwupd_release_match_flags(rel2,
						       self->filter_release_include,
						       self->filter_release_exclude))
				continue;
			if (g_strcmp0(remote, fwupd_release_get_remote_id(rel2)) != 0)
				continue;
			if (g_strcmp0(fwupd_release_get_version(rel),
				      fwupd_release_get_version(rel2)) != 0)
				continue;
			g_node_append_data(child, g_object_ref(rel2));
			rel = NULL;
			break;
		}

		/* didn't match anything */
		if (rels->len == 0 || rel != NULL) {
			g_node_append_data(child, g_object_ref(rel));
			continue;
		}
	}
	fu_util_print_node(self->console, self->client, root);

	return TRUE;
}

static gboolean
fu_util_refresh_remote(FuUtil *self, FwupdRemote *remote, GError **error)
{
	g_autofree gchar *uri_raw = NULL;
	g_autofree gchar *uri_sig = NULL;
	g_autoptr(GBytes) bytes_raw = NULL;
	g_autoptr(GBytes) bytes_sig = NULL;

	/* signature */
	if (fwupd_remote_get_metadata_uri_sig(remote) == NULL) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOTHING_TO_DO,
			    "no metadata signature URI available for %s",
			    fwupd_remote_get_id(remote));
		return FALSE;
	}
	uri_sig = fwupd_remote_build_metadata_sig_uri(remote, error);
	if (uri_sig == NULL)
		return FALSE;
	bytes_sig = fwupd_client_download_bytes(self->client,
						uri_sig,
						FWUPD_CLIENT_DOWNLOAD_FLAG_NONE,
						self->cancellable,
						error);
	if (bytes_sig == NULL)
		return FALSE;
	if (!fwupd_remote_load_signature_bytes(remote, bytes_sig, error))
		return FALSE;

	/* payload */
	if (fwupd_remote_get_metadata_uri(remote) == NULL) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOTHING_TO_DO,
			    "no metadata URI available for %s",
			    fwupd_remote_get_id(remote));
		return FALSE;
	}
	uri_raw = fwupd_remote_build_metadata_uri(remote, error);
	if (uri_raw == NULL)
		return FALSE;
	bytes_raw = fwupd_client_download_bytes(self->client,
						uri_raw,
						FWUPD_CLIENT_DOWNLOAD_FLAG_NONE,
						self->cancellable,
						error);
	if (bytes_raw == NULL)
		return FALSE;

	/* send to daemon */
	g_info("updating %s", fwupd_remote_get_id(remote));
	return fu_engine_update_metadata_bytes(self->engine,
					       fwupd_remote_get_id(remote),
					       bytes_raw,
					       bytes_sig,
					       error);
}

static gboolean
fu_util_refresh(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) remotes = NULL;

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_REMOTES |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* download new metadata */
	remotes = fu_engine_get_remotes(self->engine, error);
	if (remotes == NULL)
		return FALSE;
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index(remotes, i);
		if (!fwupd_remote_has_flag(remote, FWUPD_REMOTE_FLAG_ENABLED))
			continue;
		if (fwupd_remote_get_kind(remote) != FWUPD_REMOTE_KIND_DOWNLOAD)
			continue;
		if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
		    !fwupd_remote_needs_refresh(remote)) {
			g_debug("skipping as remote %s age is %us",
				fwupd_remote_get_id(remote),
				(guint)fwupd_remote_get_age(remote));
			continue;
		}
		if (!fu_util_refresh_remote(self, remote, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
fu_util_get_remotes(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuUtilNode) root = g_node_new(NULL);
	g_autoptr(GPtrArray) remotes = NULL;

	/* load engine */
	if (!fu_util_start_engine(self, FU_ENGINE_LOAD_FLAG_REMOTES, self->progress, error))
		return FALSE;

	/* list remotes */
	remotes = fu_engine_get_remotes(self->engine, error);
	if (remotes == NULL)
		return FALSE;
	if (remotes->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "no remotes available");
		return FALSE;
	}
	if (self->as_json) {
		g_autoptr(JsonBuilder) builder = json_builder_new();
		json_builder_begin_object(builder);
		fwupd_codec_array_to_json(remotes, "Remotes", builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
		return fu_util_print_builder(self->console, builder, error);
	}
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote_tmp = g_ptr_array_index(remotes, i);
		g_node_append_data(root, g_object_ref(remote_tmp));
	}
	fu_util_print_node(self->console, self->client, root);

	return TRUE;
}

static gboolean
fu_util_security(FuUtil *self, gchar **values, GError **error)
{
	FuSecurityAttrToStringFlags flags = FU_SECURITY_ATTR_TO_STRING_FLAG_NONE;
	const gchar *fwupd_version = NULL;
	g_autoptr(FuSecurityAttrs) attrs = NULL;
	g_autoptr(FuSecurityAttrs) events = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GPtrArray) events_array = NULL;
	g_autofree gchar *str = NULL;
	g_autofree gchar *host_security_id = NULL;

#ifndef HAVE_HSI
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_SUPPORTED,
		    /* TRANSLATORS: error message for unsupported feature */
		    _("Host Security ID (HSI) is not supported"));
	return FALSE;
#endif /* HAVE_HSI */

	/* optionally restrict by version */
	if (g_strv_length(values) > 0)
		fwupd_version = values[0];

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_REMOTES |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* show or hide different elements */
	if (self->show_all) {
		flags |= FU_SECURITY_ATTR_TO_STRING_FLAG_SHOW_OBSOLETES;
		flags |= FU_SECURITY_ATTR_TO_STRING_FLAG_SHOW_URLS;
	}

	attrs = fu_engine_get_host_security_attrs(self->engine);
	items = fu_security_attrs_get_all(attrs, fwupd_version);

	/* print the "why" */
	if (self->as_json) {
		str = fwupd_codec_to_json_string(FWUPD_CODEC(attrs), FWUPD_CODEC_FLAG_NONE, error);
		if (str == NULL)
			return FALSE;
		fu_console_print_literal(self->console, str);
		return TRUE;
	}

	host_security_id = fu_engine_get_host_security_id(self->engine, fwupd_version);
	fu_console_print(self->console,
			 "%s \033[1m%s\033[0m",
			 /* TRANSLATORS: this is a string like 'HSI:2-U' */
			 _("Host Security ID:"),
			 host_security_id);

	str = fu_util_security_attrs_to_string(items, flags);
	fu_console_print_literal(self->console, str);

	/* print the "when" */
	events = fu_engine_get_host_security_events(self->engine, 10, error);
	if (events == NULL)
		return FALSE;
	events_array = fu_security_attrs_get_all(events, fwupd_version);
	if (events_array->len > 0) {
		g_autofree gchar *estr = fu_util_security_events_to_string(events_array, flags);
		if (estr != NULL)
			fu_console_print_literal(self->console, estr);
	}

	/* print the "also" */
	devices = fu_engine_get_devices(self->engine, error);
	if (devices == NULL)
		return FALSE;
	if (devices->len > 0) {
		g_autofree gchar *estr = fu_util_security_issues_to_string(devices);
		if (estr != NULL)
			fu_console_print_literal(self->console, estr);
	}

	/* success */
	return TRUE;
}

static FuVolume *
fu_util_prompt_for_volume(FuUtil *self, GError **error)
{
	FuContext *ctx = fu_engine_get_context(self->engine);
	FuVolume *volume;
	guint idx;
	g_autoptr(GPtrArray) volumes = NULL;

	/* exactly one */
	volumes = fu_context_get_esp_volumes(ctx, error);
	if (volumes == NULL)
		return NULL;
	if (volumes->len == 1) {
		volume = g_ptr_array_index(volumes, 0);
		if (fu_volume_get_id(volume) != NULL) {
			fu_console_print(self->console,
					 "%s: %s",
					 /* TRANSLATORS: Volume has been chosen by the user */
					 _("Selected volume"),
					 fu_volume_get_id(volume));
		}
		return g_object_ref(volume);
	}

	/* TRANSLATORS: this is to abort the interactive prompt */
	fu_console_print(self->console, "0.\t%s", _("Cancel"));
	for (guint i = 0; i < volumes->len; i++) {
		volume = g_ptr_array_index(volumes, i);
		fu_console_print(self->console, "%u.\t%s", i + 1, fu_volume_get_id(volume));
	}
	/* TRANSLATORS: get interactive prompt */
	idx = fu_console_input_uint(self->console, volumes->len, "%s", _("Choose volume"));
	if (idx == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "Request canceled");
		return NULL;
	}
	volume = g_ptr_array_index(volumes, idx - 1);
	return g_object_ref(volume);
}

static gboolean
fu_util_esp_mount(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuVolume) volume = NULL;
	volume = fu_util_prompt_for_volume(self, error);
	if (volume == NULL)
		return FALSE;
	return fu_volume_mount(volume, error);
}

static gboolean
fu_util_esp_unmount(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuVolume) volume = NULL;
	volume = fu_util_prompt_for_volume(self, error);
	if (volume == NULL)
		return FALSE;
	return fu_volume_unmount(volume, error);
}

static gboolean
fu_util_esp_list_as_json(FuUtil *self, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) volumes = NULL;

	volumes = fu_context_get_esp_volumes(fu_engine_get_context(self->engine), error);
	if (volumes == NULL)
		return FALSE;

	json_builder_begin_object(builder);
	fwupd_codec_array_to_json(volumes, "Volumes", builder, FWUPD_CODEC_FLAG_TRUSTED);
	json_builder_end_object(builder);
	return fu_util_print_builder(self->console, builder, error);
}

static gboolean
fu_util_esp_list(FuUtil *self, gchar **values, GError **error)
{
	g_autofree gchar *mount_point = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(FuVolume) volume = NULL;
	g_autoptr(GPtrArray) files = NULL;

	if (!fu_util_start_engine(self, FU_ENGINE_LOAD_FLAG_HWINFO, self->progress, error))
		return FALSE;
	if (self->as_json)
		return fu_util_esp_list_as_json(self, error);

	volume = fu_util_prompt_for_volume(self, error);
	if (volume == NULL)
		return FALSE;
	locker = fu_volume_locker(volume, error);
	if (locker == NULL)
		return FALSE;
	mount_point = fu_volume_get_mount_point(volume);
	if (mount_point == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "no mountpoint for ESP");
		return FALSE;
	}
	files = fu_path_get_files(mount_point, error);
	if (files == NULL)
		return FALSE;
	for (guint i = 0; i < files->len; i++) {
		const gchar *fn = g_ptr_array_index(files, i);
		fu_console_print_literal(self->console, fn);
	}
	return TRUE;
}

static gboolean
fu_util_modify_tag(FuUtil *self, gchar **values, gboolean enable, GError **error)
{
	g_autoptr(FuDevice) dev = NULL;
	const gchar *tag = enable ? "emulation-tag" : "~emulation-tag";

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* set the flag */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_CAN_EMULATION_TAG;
	if (g_strv_length(values) >= 1) {
		dev = fu_util_get_device(self, values[0], error);
		if (dev == NULL)
			return FALSE;
	} else {
		dev = fu_util_prompt_for_device(self, NULL, error);
		if (dev == NULL)
			return FALSE;
	}

	return fu_engine_modify_device(self->engine, fu_device_get_id(dev), "Flags", tag, error);
}

static gboolean
fu_util_emulation_tag(FuUtil *self, gchar **values, GError **error)
{
	return fu_util_modify_tag(self, values, TRUE, error);
}

static gboolean
fu_util_emulation_untag(FuUtil *self, gchar **values, GError **error)
{
	return fu_util_modify_tag(self, values, FALSE, error);
}

static gboolean
fu_util_emulation_load(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GInputStream) stream = NULL;

	/* check args */
	if (g_strv_length(values) < 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Invalid arguments, expected EMULATION-FILE [ARCHIVE-FILE]");
		return FALSE;
	}

	/* progress */
	fu_progress_set_id(self->progress, G_STRLOC);
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 95, "start-engine");
	fu_progress_add_step(self->progress, FWUPD_STATUS_LOADING, 5, "load-emulation");
	fu_progress_add_step(self->progress, FWUPD_STATUS_DEVICE_WRITE, 5, "write");

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_HWINFO,
				  fu_progress_get_child(self->progress),
				  error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* load emulation */
	stream = fu_input_stream_from_path(values[0], error);
	if (stream == NULL)
		return FALSE;
	if (!fu_engine_emulation_load(self->engine, stream, error))
		return FALSE;
	fu_progress_step_done(self->progress);

	/* "install" archive */
	if (values[1] != NULL) {
		g_autoptr(GInputStream) stream_cab = NULL;
		g_autoptr(GPtrArray) devices_possible = NULL;

		stream_cab = fu_input_stream_from_path(values[1], error);
		if (stream_cab == NULL)
			return FALSE;
		devices_possible = fu_engine_get_devices(self->engine, error);
		if (devices_possible == NULL)
			return FALSE;
		if (!fu_util_install_stream(self,
					    stream_cab,
					    devices_possible,
					    fu_progress_get_child(self->progress),
					    error))
			return FALSE;
	}
	fu_progress_step_done(self->progress);

	/* success */
	return TRUE;
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
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;
	g_autoptr(GPtrArray) branches = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(FuDevice) dev = NULL;

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_REMOTES | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* find the device and check it has multiple branches */
	self->filter_device_include |= FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES;
	self->filter_device_include |= FWUPD_DEVICE_FLAG_UPDATABLE;
	if (g_strv_length(values) == 1)
		dev = fu_util_get_device(self, values[1], error);
	else
		dev = fu_util_prompt_for_device(self, NULL, error);
	if (dev == NULL)
		return FALSE;
	if (!fu_device_has_flag(dev, FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "Multiple branches not available");
		return FALSE;
	}

	/* get all releases, including the alternate branch versions */
	rels = fu_engine_get_releases(self->engine, self->request, fu_device_get_id(dev), error);
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
	if (g_strcmp0(branch, fu_device_get_branch(dev)) == 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "Device %s is already on branch %s",
			    fu_device_get_name(dev),
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
	if (!fu_util_switch_branch_warning(self->console, FWUPD_DEVICE(dev), rel, FALSE, error))
		return FALSE;

	/* update the console if composite devices are also updated */
	self->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-changed",
			 G_CALLBACK(fu_util_update_device_changed_cb),
			 self);
	self->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	self->flags |= FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH;
	if (!fu_util_install_release(self, FWUPD_DEVICE(dev), rel, error))
		return FALSE;
	fu_util_display_current_message(self);

	/* we don't want to ask anything */
	if (self->no_reboot_check) {
		g_debug("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete(self->console, self->completion_flags, TRUE, error);
}

static gboolean
fu_util_set_bios_setting(FuUtil *self, gchar **input, GError **error)
{
	g_autoptr(GHashTable) settings = fu_util_bios_settings_parse_argv(input, error);

	if (settings == NULL)
		return FALSE;

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	if (!fu_engine_modify_bios_settings(self->engine, settings, FALSE, error)) {
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

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_REMOTES |
				      FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				      FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;
	if (!fu_engine_fix_host_security_attr(self->engine, values[0], error))
		return FALSE;
	/* TRANSLATORS: we've fixed a security problem on the machine */
	fu_console_print_literal(self->console, _("Fixed successfully"));
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

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_REMOTES |
				      FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
				      FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;
	if (!fu_engine_undo_host_security_attr(self->engine, values[0], error))
		return FALSE;
	/* TRANSLATORS: we've fixed a security problem on the machine */
	fu_console_print_literal(self->console, _("Fix reverted successfully"));
	return TRUE;
}

static gboolean
fu_util_get_bios_setting(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuBiosSettings) attrs = NULL;
	g_autoptr(GPtrArray) items = NULL;
	FuContext *ctx = fu_engine_get_context(self->engine);
	gboolean found = FALSE;

	/* load engine */
	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG | FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	attrs = fu_context_get_bios_settings(ctx);
	items = fu_bios_settings_get_all(attrs);
	if (self->as_json)
		return fu_util_bios_setting_console_print(self->console, values, items, error);

	for (guint i = 0; i < items->len; i++) {
		FwupdBiosSetting *attr = g_ptr_array_index(items, i);
		if (fu_util_bios_setting_matches_args(attr, values)) {
			g_autofree gchar *tmp = fu_util_bios_setting_to_string(attr, 0);
			fu_console_print_literal(self->console, tmp);
			found = TRUE;
		}
	}
	if (items->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: error message */
				    _("This system doesn't support firmware settings"));
		return FALSE;
	}
	if (!found) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_ARGS,
			    "%s: '%s'",
			    /* TRANSLATORS: error message */
			    _("Unable to find attribute"),
			    values[0]);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_util_reboot_cleanup(FuUtil *self, gchar **values, GError **error)
{
	FuPlugin *plugin;
	g_autoptr(FuDevice) device = NULL;

	if (!fu_util_start_engine(self,
				  FU_ENGINE_LOAD_FLAG_COLDPLUG |
				      FU_ENGINE_LOAD_FLAG_DEVICE_HOTPLUG |
				      FU_ENGINE_LOAD_FLAG_HWINFO,
				  self->progress,
				  error))
		return FALSE;

	/* both arguments are optional */
	if (g_strv_length(values) >= 1) {
		device = fu_engine_get_device(self->engine, values[1], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device(self, NULL, error);
		if (device == NULL)
			return FALSE;
	}
	plugin = fu_engine_get_plugin_by_name(self->engine, fu_device_get_plugin(device), error);
	if (plugin == NULL)
		return FALSE;
	return fu_plugin_runner_reboot_cleanup(plugin, device, error);
}

static gboolean
fu_util_efiboot_info_as_json(FuUtil *self, GPtrArray *entries, GError **error)
{
	FuEfivars *efivars = fu_context_get_efivars(self->ctx);
	guint16 idx = 0;
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	if (fu_efivars_get_boot_current(efivars, &idx, NULL))
		fwupd_codec_json_append_int(builder, "BootCurrent", idx);
	if (fu_efivars_get_boot_next(efivars, &idx, NULL))
		fwupd_codec_json_append_int(builder, "BootNext", idx);

	json_builder_set_member_name(builder, "Entries");
	json_builder_begin_object(builder);
	for (guint i = 0; i < entries->len; i++) {
		FuEfiLoadOption *entry = g_ptr_array_index(entries, i);
		g_autofree gchar *title =
		    g_strdup_printf("Boot%04X", (guint)fu_firmware_get_idx(FU_FIRMWARE(entry)));
		json_builder_set_member_name(builder, title);
		json_builder_begin_array(builder);
		json_builder_begin_object(builder);
		fwupd_codec_to_json(FWUPD_CODEC(entry), builder, FWUPD_CODEC_FLAG_TRUSTED);
		json_builder_end_object(builder);
		json_builder_end_array(builder);
	}
	json_builder_end_object(builder);

	json_builder_end_object(builder);
	return fu_util_print_builder(self->console, builder, error);
}

static gboolean
fu_util_efiboot_next(FuUtil *self, gchar **values, GError **error)
{
	FuEfivars *efivars = fu_context_get_efivars(self->ctx);
	guint64 value = 0;

	/* just show */
	if (values[0] == NULL) {
		guint16 idx = 0;
		if (!fu_efivars_get_boot_next(efivars, &idx, error))
			return FALSE;
		fu_console_print(self->console, "Boot%04X", idx);
		return TRUE;
	}

	/* modify */
	if (!fu_strtoull(values[0], &value, 0x0, G_MAXUINT16, FU_INTEGER_BASE_16, error))
		return FALSE;
	return fu_efivars_set_boot_next(efivars, (guint16)value, error);
}

static gboolean
fu_util_efiboot_order(FuUtil *self, gchar **values, GError **error)
{
	FuEfivars *efivars = fu_context_get_efivars(self->ctx);
	g_auto(GStrv) split = NULL;
	g_autoptr(GArray) order = NULL;

	/* just show */
	if (values[0] == NULL) {
		order = fu_efivars_get_boot_order(efivars, error);
		if (order == NULL)
			return FALSE;
		for (guint i = 0; i < order->len; i++) {
			guint16 idx = g_array_index(order, guint16, i);
			fu_console_print(self->console, "Boot%04X", idx);
		}
		return TRUE;
	}

	/* modify */
	order = g_array_new(FALSE, FALSE, sizeof(guint16));
	split = g_strsplit(values[0], ",", -1);
	for (guint i = 0; split[i] != NULL; i++) {
		guint64 value = 0;
		guint16 value_as_u16;
		if (!fu_strtoull(split[i], &value, 0x0, G_MAXUINT16, FU_INTEGER_BASE_16, error))
			return FALSE;
		value_as_u16 = (guint16)value;
		g_array_append_val(order, value_as_u16);
	}
	return fu_efivars_set_boot_order(efivars, order, error);
}

static gboolean
fu_util_efiboot_create(FuUtil *self, gchar **values, GError **error)
{
	FuEfivars *efivars = fu_context_get_efivars(self->ctx);
	g_autoptr(FuVolume) volume = NULL;
	guint64 idx = 0;

	/* check args */
	if (g_strv_length(values) < 3) {
		g_set_error_literal(
		    error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOTHING_TO_DO,
		    /* TRANSLATORS: error message */
		    _("Invalid arguments, expected INDEX NAME TARGET [MOUNTPOINT]"));
		return FALSE;
	}

	/* check the index does not already exist */
	if (!fu_strtoull(values[0], &idx, 0x0, G_MAXUINT16, FU_INTEGER_BASE_16, error))
		return FALSE;
	if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		g_autoptr(GBytes) blob = fu_efivars_get_boot_data(efivars, (guint16)idx, NULL);
		if (blob != NULL) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOTHING_TO_DO,
					    /* TRANSLATORS: error message */
					    _("Already exists, and no --force specified"));
			return FALSE;
		}
	}

	/* get volume */
	if (values[3] == NULL) {
		volume = fu_util_prompt_for_volume(self, error);
		if (volume == NULL)
			return FALSE;
	} else {
		g_autoptr(GPtrArray) volumes = NULL;
		volumes = fu_context_get_esp_volumes(self->ctx, error);
		if (volumes == NULL)
			return FALSE;
		for (guint i = 0; i < volumes->len; i++) {
			FuVolume *volume_tmp = g_ptr_array_index(volumes, i);
			g_autofree gchar *mount_point = fu_volume_get_mount_point(volume_tmp);
			if (g_strcmp0(mount_point, values[3]) == 0) {
				volume = g_object_ref(volume_tmp);
				break;
			}
		}
		if (volume == NULL) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    /* TRANSLATORS: error message */
				    _("No volume matched %s"),
				    values[3]);
			return FALSE;
		}
	}
	return fu_efivars_create_boot_entry_for_volume(efivars,
						       (guint16)idx,
						       volume,
						       values[1],
						       values[2],
						       error);
}

static gboolean
fu_util_efiboot_delete(FuUtil *self, gchar **values, GError **error)
{
	FuEfivars *efivars = fu_context_get_efivars(self->ctx);
	guint64 value = 0;

	if (values[0] == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: error message */
				    _("Invalid arguments, expected base-16 integer"));
		return FALSE;
	}
	if (!fu_strtoull(values[0], &value, 0x0, G_MAXUINT16, FU_INTEGER_BASE_16, error))
		return FALSE;

	/* success */
	return fu_efivars_set_boot_data(efivars, (guint16)value, NULL, error);
}

static gboolean
fu_util_efiboot_hive_check_loadopt_is_shim(FuEfiLoadOption *loadopt, GError **error)
{
	gboolean seen_shim = FALSE;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(GPtrArray) dps = NULL;

	/* get FuEfiDevicePathList */
	firmware = fu_firmware_get_image_by_idx(FU_FIRMWARE(loadopt), 0x0, error);
	if (firmware == NULL)
		return FALSE;
	dps = fu_firmware_get_images(firmware);
	for (guint i = 0; i < dps->len; i++) {
		FuFirmware *dp = g_ptr_array_index(dps, i);
		if (FU_IS_EFI_FILE_PATH_DEVICE_PATH(dp)) {
			g_autofree gchar *name =
			    fu_efi_file_path_device_path_get_name(FU_EFI_FILE_PATH_DEVICE_PATH(dp),
								  error);
			if (name == NULL)
				return FALSE;
			if (g_pattern_match_simple("*shim*.efi", name)) {
				seen_shim = TRUE;
				break;
			}
		}
	}
	if (!seen_shim) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "Only the shim bootloader supports the hive format");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_efiboot_hive(FuUtil *self, gchar **values, GError **error)
{
	FuEfivars *efivars = fu_context_get_efivars(self->ctx);
	g_autoptr(FuEfiLoadOption) loadopt = NULL;
	guint64 idx = 0;

	/* check args */
	if (g_strv_length(values) < 2) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: error message */
				    _("Invalid arguments, expected INDEX KEY [VALUE]"));
		return FALSE;
	}

	/* load the boot entry */
	if (!fu_strtoull(values[0], &idx, 0x0, G_MAXUINT16, FU_INTEGER_BASE_16, error))
		return FALSE;
	loadopt = fu_efivars_get_boot_entry(efivars, (guint16)idx, error);
	if (loadopt == NULL)
		return FALSE;

	/* get value */
	if (values[2] == NULL) {
		const gchar *value;
		fu_console_print_full(self->console,
				      FU_CONSOLE_PRINT_FLAG_WARNING,
				      "%s\n",
				      /* TRANSLATORS: try to treat the legacy format as a hive */
				      _("The EFI boot entry was not in hive format, falling back"));
		value = fu_efi_load_option_get_metadata(loadopt, values[1], error);
		if (value == NULL)
			return FALSE;
		fu_console_print_literal(self->console, value);
		return TRUE;
	}

	/* check this is actually shim */
	if (!fu_util_efiboot_hive_check_loadopt_is_shim(loadopt, error))
		return FALSE;

	/* change the format if required */
	if (fu_efi_load_option_get_kind(loadopt) != FU_EFI_LOAD_OPTION_KIND_HIVE) {
		fu_console_print_full(self->console,
				      FU_CONSOLE_PRINT_FLAG_WARNING,
				      "%s\n",
				      /* TRANSLATORS: the boot entry was in a legacy format */
				      _("The EFI boot entry is not in hive format, "
					"and shim may not be new enough to read it."));
		if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
		    !fu_console_input_bool(self->console,
					   FALSE,
					   "%s",
					   /* TRANSLATORS: ask the user if it's okay to convert,
					    * "it" being the data contained in the EFI boot entry */
					   _("Do you want to convert it now?"))) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_PERMISSION_DENIED,
					    "User declined action");
			return FALSE;
		}
		fu_efi_load_option_set_kind(loadopt, FU_EFI_LOAD_OPTION_KIND_HIVE);
	}

	/* set value */
	fu_efi_load_option_set_metadata(loadopt, values[1], values[2]);
	return fu_efivars_set_boot_entry(efivars, (guint16)idx, loadopt, error);
}

static gboolean
fu_util_efiboot_info(FuUtil *self, gchar **values, GError **error)
{
	FuEfivars *efivars = fu_context_get_efivars(self->ctx);
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GString) str = g_string_new(NULL);
	guint16 idx = 0;

	entries = fu_efivars_get_boot_entries(efivars, error);
	if (entries == NULL)
		return FALSE;

	/* dump to the screen in the most appropriate format */
	if (self->as_json)
		return fu_util_efiboot_info_as_json(self, entries, error);

	if (fu_efivars_get_boot_current(efivars, &idx, NULL))
		fwupd_codec_string_append_hex(str, 0, "BootCurrent", idx);
	if (fu_efivars_get_boot_next(efivars, &idx, NULL))
		fwupd_codec_string_append_hex(str, 0, "BootNext", idx);

	for (guint i = 0; i < entries->len; i++) {
		FuEfiLoadOption *entry = g_ptr_array_index(entries, i);
		g_autofree gchar *title =
		    g_strdup_printf("Boot%04X", (guint)fu_firmware_get_idx(FU_FIRMWARE(entry)));
		fwupd_codec_string_append(str, 0, title, "");
		fwupd_codec_add_string(FWUPD_CODEC(entry), 1, str);
	}

	/* success */
	fu_console_print_literal(self->console, str->str);
	return TRUE;
}

static gboolean
fu_util_efivar_files_as_json(FuUtil *self, GPtrArray *files, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GHashTable) hash = g_hash_table_new_full(g_str_hash,
							   g_str_equal,
							   g_free,
							   (GDestroyNotify)g_ptr_array_unref);
	GHashTableIter iter;
	gpointer key, value;

	/* convert an array of FuPeFirmware to a map with the BootXXXX ID as the hash key and the
	 * filename as an array */
	for (guint i = 0; i < files->len; i++) {
		FuFirmware *firmware = g_ptr_array_index(files, i);
		GPtrArray *array;
		g_autofree gchar *name = NULL;

		name = g_strdup_printf("Boot%04X", (guint)fu_firmware_get_idx(firmware));
		array = g_hash_table_lookup(hash, name);
		if (array == NULL) {
			array = g_ptr_array_new_with_free_func(g_free);
			g_hash_table_insert(hash, g_steal_pointer(&name), array);
		}
		g_ptr_array_add(array, g_strdup(fu_firmware_get_filename(firmware)));
	}

	/* export */
	json_builder_begin_object(builder);
	g_hash_table_iter_init(&iter, hash);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const gchar *bootvar = (const gchar *)key;
		GPtrArray *array = (GPtrArray *)value;

		json_builder_set_member_name(builder, bootvar);
		json_builder_begin_array(builder);
		for (guint i = 0; i < array->len; i++) {
			const gchar *filename = g_ptr_array_index(array, i);
			json_builder_add_string_value(builder, filename);
		}
		json_builder_end_array(builder);
	}
	json_builder_end_object(builder);
	return fu_util_print_builder(self->console, builder, error);
}

static gboolean
fu_util_efivar_files(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) files = NULL;

	files = fu_context_get_esp_files(self->ctx,
					 FU_CONTEXT_ESP_FILE_FLAG_INCLUDE_FIRST_STAGE |
					     FU_CONTEXT_ESP_FILE_FLAG_INCLUDE_SECOND_STAGE |
					     FU_CONTEXT_ESP_FILE_FLAG_INCLUDE_REVOCATIONS,
					 error);
	if (files == NULL)
		return FALSE;
	if (self->as_json)
		return fu_util_efivar_files_as_json(self, files, error);
	for (guint i = 0; i < files->len; i++) {
		FuFirmware *firmware = g_ptr_array_index(files, i);
		g_autofree gchar *name =
		    g_strdup_printf("Boot%04X", (guint)fu_firmware_get_idx(firmware));
		fu_console_print(self->console,
				 "%s → %s",
				 name,
				 fu_firmware_get_filename(firmware));
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_efivar_list(FuUtil *self, gchar **values, GError **error)
{
	FuEfivars *efivars = fu_context_get_efivars(self->ctx);
	g_autoptr(GPtrArray) names = NULL;

	/* sanity check */
	if (g_strv_length(values) < 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    /* TRANSLATORS: error message */
				    _("Invalid arguments, expected GUID"));
		return FALSE;
	}
	names = fu_efivars_get_names(efivars, values[0], error);
	if (names == NULL)
		return FALSE;
	for (guint i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		fu_console_print(self->console, "name: %s", name);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_build_cabinet(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(GBytes) cab_blob = NULL;
	g_autoptr(FuCabinet) cab_file = fu_cabinet_new();

	/* sanity check */
	if (g_strv_length(values) < 3) {
		g_set_error_literal(
		    error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOTHING_TO_DO,
		    /* TRANSLATORS: error message */
		    _("Invalid arguments, expected at least ARCHIVE FIRMWARE METAINFO"));
		return FALSE;
	}

	/* file already exists */
	if ((self->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    g_file_test(values[0], G_FILE_TEST_EXISTS)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "Filename already exists");
		return FALSE;
	}

	/* add each file */
	for (guint i = 1; values[i] != NULL; i++) {
		g_autoptr(GBytes) blob = NULL;
		g_autofree gchar *basename = g_path_get_basename(values[i]);
		blob = fu_bytes_get_contents(values[i], error);
		if (blob == NULL)
			return FALSE;
		if (g_bytes_get_size(blob) == 0) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_ARGS,
				    "%s has zero size",
				    values[i]);
			return FALSE;
		}
		fu_cabinet_add_file(cab_file, basename, blob);
	}

	/* export */
	cab_blob = fu_firmware_write(FU_FIRMWARE(cab_file), error);
	if (cab_blob == NULL)
		return FALSE;

	/* sanity check JCat and XML MetaInfo files */
	if (!fu_firmware_parse_bytes(FU_FIRMWARE(cab_file),
				     cab_blob,
				     0x0,
				     FU_FIRMWARE_PARSE_FLAG_CACHE_BLOB,
				     error))
		return FALSE;

	return fu_bytes_set_contents(values[0], cab_blob, error);
}

static gboolean
fu_util_version(FuUtil *self, GError **error)
{
	g_autoptr(GHashTable) metadata = NULL;
	g_autofree gchar *str = NULL;

	/* load engine */
	if (!fu_util_start_engine(
		self,
		FU_ENGINE_LOAD_FLAG_READONLY | FU_ENGINE_LOAD_FLAG_EXTERNAL_PLUGINS |
		    FU_ENGINE_LOAD_FLAG_BUILTIN_PLUGINS | FU_ENGINE_LOAD_FLAG_HWINFO,
		self->progress,
		error))
		return FALSE;

	/* get metadata */
	metadata = fu_engine_get_report_metadata(self->engine, error);
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
fu_util_clear_history(FuUtil *self, gchar **values, GError **error)
{
	g_autoptr(FuHistory) history = fu_history_new(self->ctx);
	return fu_history_remove_all(history, error);
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
	gboolean allow_branch_switch = FALSE;
	gboolean allow_older = FALSE;
	gboolean allow_reinstall = FALSE;
	gboolean force = FALSE;
	gboolean no_search = FALSE;
	gboolean ret;
	gboolean version = FALSE;
	gboolean ignore_checksum = FALSE;
	gboolean ignore_requirements = FALSE;
	gboolean ignore_vid_pid = FALSE;
	g_auto(GStrv) plugin_glob = NULL;
	g_autoptr(FuUtil) self = g_new0(FuUtil, 1);
	g_autoptr(GError) error_console = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) cmd_array = fu_util_cmd_array_new();
	g_autofree gchar *cmd_descriptions = NULL;
	g_autofree gchar *filter_device = NULL;
	g_autofree gchar *filter_release = NULL;
	const GOptionEntry options[] = {
	    {"version",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &version,
	     /* TRANSLATORS: command line option */
	     N_("Show client and daemon versions"),
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
	    {"force",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &force,
	     /* TRANSLATORS: command line option */
	     N_("Force the action by relaxing some runtime checks"),
	     NULL},
	    {"ignore-checksum",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &ignore_checksum,
	     /* TRANSLATORS: command line option */
	     N_("Ignore firmware checksum failures"),
	     NULL},
	    {"ignore-vid-pid",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &ignore_vid_pid,
	     /* TRANSLATORS: command line option */
	     N_("Ignore firmware hardware mismatch failures"),
	     NULL},
	    {"ignore-requirements",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &ignore_requirements,
	     /* TRANSLATORS: command line option */
	     N_("Ignore non-critical firmware requirements"),
	     NULL},
	    {"no-reboot-check",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->no_reboot_check,
	     /* TRANSLATORS: command line option */
	     N_("Do not check or prompt for reboot after update"),
	     NULL},
	    {"no-search",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &no_search,
	     /* TRANSLATORS: command line option */
	     N_("Do not search the firmware when parsing"),
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
	    {"plugins",
	     '\0',
	     0,
	     G_OPTION_ARG_STRING_ARRAY,
	     &plugin_glob,
	     /* TRANSLATORS: command line option */
	     N_("Manually enable specific plugins"),
	     NULL},
	    {"plugin-whitelist",
	     '\0',
	     G_OPTION_FLAG_HIDDEN,
	     G_OPTION_ARG_STRING_ARRAY,
	     &plugin_glob,
	     /* TRANSLATORS: command line option */
	     N_("Manually enable specific plugins"),
	     NULL},
	    {"prepare",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->prepare_blob,
	     /* TRANSLATORS: command line option */
	     N_("Run the plugin composite prepare routine when using install-blob"),
	     NULL},
	    {"cleanup",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->cleanup_blob,
	     /* TRANSLATORS: command line option */
	     N_("Run the plugin composite cleanup routine when using install-blob"),
	     NULL},
	    {"disable-ssl-strict",
	     '\0',
	     0,
	     G_OPTION_ARG_NONE,
	     &self->disable_ssl_strict,
	     /* TRANSLATORS: command line option */
	     N_("Ignore SSL strict checks when downloading files"),
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
	    {NULL}};

#ifdef _WIN32
	/* workaround Windows setting the codepage to 1252 */
	(void)g_setenv("LANG", "C.UTF-8", FALSE);
#endif

	setlocale(LC_ALL, "");

	bindtextdomain(GETTEXT_PACKAGE, FWUPD_LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
	textdomain(GETTEXT_PACKAGE);
	g_set_prgname(fu_util_get_prgname(argv[0]));

	/* create helper object */
	self->lock_fd = -1;
	self->main_ctx = g_main_context_new();
	self->loop = g_main_loop_new(self->main_ctx, FALSE);
	self->console = fu_console_new();
	self->post_requests = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	fu_console_set_main_context(self->console, self->main_ctx);
	self->request = fu_engine_request_new(NULL);

	/* used for monitoring and downloading */
	self->client = fwupd_client_new();
	fwupd_client_set_main_context(self->client, self->main_ctx);
	fwupd_client_set_daemon_version(self->client, PACKAGE_VERSION);
	fwupd_client_set_user_agent_for_package(self->client, "fwupdtool", PACKAGE_VERSION);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "notify::percentage",
			 G_CALLBACK(fu_util_client_notify_cb),
			 self);
	g_signal_connect(FWUPD_CLIENT(self->client),
			 "notify::status",
			 G_CALLBACK(fu_util_client_notify_cb),
			 self);

	/* when not using the engine */
	self->progress = fu_progress_new(G_STRLOC);
	g_signal_connect(self->progress,
			 "percentage-changed",
			 G_CALLBACK(fu_util_progress_percentage_changed_cb),
			 self);
	g_signal_connect(self->progress,
			 "status-changed",
			 G_CALLBACK(fu_util_progress_status_changed_cb),
			 self);

	/* add commands */
	fu_util_cmd_array_add(cmd_array,
			      "smbios-dump",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILE"),
			      /* TRANSLATORS: command description */
			      _("Dump SMBIOS data from a file"),
			      fu_util_smbios_dump);
	fu_util_cmd_array_add(cmd_array,
			      "get-plugins",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Get all enabled plugins registered with the system"),
			      fu_util_get_plugins);
	fu_util_cmd_array_add(cmd_array,
			      "get-details",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILE"),
			      /* TRANSLATORS: command description */
			      _("Gets details about a firmware file"),
			      fu_util_get_details);
	fu_util_cmd_array_add(cmd_array,
			      "get-history",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Show history of firmware updates"),
			      fu_util_get_history);
	fu_util_cmd_array_add(cmd_array,
			      "get-updates,get-upgrades",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Gets the list of updates for all specified devices, or all "
				"devices if unspecified"),
			      fu_util_get_updates);
	fu_util_cmd_array_add(cmd_array,
			      "get-devices,get-topology",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Get all devices that support firmware updates"),
			      fu_util_get_devices);
	fu_util_cmd_array_add(cmd_array,
			      "get-device-flags",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Get all device flags supported by fwupd"),
			      fu_util_get_device_flags);
	fu_util_cmd_array_add(cmd_array,
			      "watch",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Watch for hardware changes"),
			      fu_util_watch);
	fu_util_cmd_array_add(cmd_array,
			      "install-blob",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME DEVICE-ID [VERSION]"),
			      /* TRANSLATORS: command description */
			      _("Install a raw firmware blob on a device"),
			      fu_util_install_blob);
	fu_util_cmd_array_add(cmd_array,
			      "install",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILE [DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Install a specific firmware on a device, all possible devices"
				" will also be installed once the CAB matches"),
			      fu_util_install);
	fu_util_cmd_array_add(cmd_array,
			      "reinstall",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("DEVICE-ID|GUID"),
			      /* TRANSLATORS: command description */
			      _("Reinstall firmware on a device"),
			      fu_util_reinstall);
	fu_util_cmd_array_add(cmd_array,
			      "attach",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("DEVICE-ID|GUID"),
			      /* TRANSLATORS: command description */
			      _("Attach to firmware mode"),
			      fu_util_attach);
	fu_util_cmd_array_add(cmd_array,
			      "get-report-metadata",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Get device report metadata"),
			      fu_util_get_report_metadata);
	fu_util_cmd_array_add(cmd_array,
			      "detach",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("DEVICE-ID|GUID"),
			      /* TRANSLATORS: command description */
			      _("Detach to bootloader mode"),
			      fu_util_detach);
	fu_util_cmd_array_add(cmd_array,
			      "unbind-driver",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Unbind current driver"),
			      fu_util_unbind_driver);
	fu_util_cmd_array_add(cmd_array,
			      "bind-driver",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("SUBSYSTEM DRIVER [DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Bind new kernel driver"),
			      fu_util_bind_driver);
	fu_util_cmd_array_add(cmd_array,
			      "activate",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Activate pending devices"),
			      fu_util_activate);
	fu_util_cmd_array_add(cmd_array,
			      "hwids",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[SMBIOS-FILE|HWIDS-FILE]"),
			      /* TRANSLATORS: command description */
			      _("Return all the hardware IDs for the machine"),
			      fu_util_hwids);
	fu_util_cmd_array_add(cmd_array,
			      "export-hwids",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("HWIDS-FILE"),
			      /* TRANSLATORS: command description */
			      _("Save a file that allows generation of hardware IDs"),
			      fu_util_export_hwids);
	fu_util_cmd_array_add(cmd_array,
			      "monitor",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Monitor the daemon for events"),
			      fu_util_monitor);
	fu_util_cmd_array_add(cmd_array,
			      "update,upgrade",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Updates all specified devices to latest firmware version, or all "
				"devices if unspecified"),
			      fu_util_update);
	fu_util_cmd_array_add(cmd_array,
			      "self-sign",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("TEXT"),
			      /* TRANSLATORS: command description */
			      C_("command-description", "Sign data using the client certificate"),
			      fu_util_self_sign);
	fu_util_cmd_array_add(cmd_array,
			      "verify-update",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Update the stored metadata with current contents"),
			      fu_util_verify_update);
	fu_util_cmd_array_add(cmd_array,
			      "firmware-sign",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME CERTIFICATE PRIVATE-KEY"),
			      /* TRANSLATORS: command description */
			      _("Sign a firmware with a new key"),
			      fu_util_firmware_sign);
	fu_util_cmd_array_add(cmd_array,
			      "firmware-dump",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME [DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Read a firmware blob from a device"),
			      fu_util_firmware_dump);
	fu_util_cmd_array_add(cmd_array,
			      "firmware-read",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME [DEVICE-ID|GUID]"),
			      /* TRANSLATORS: command description */
			      _("Read a firmware from a device"),
			      fu_util_firmware_read);
	fu_util_cmd_array_add(cmd_array,
			      "firmware-patch",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME OFFSET DATA [FIRMWARE-TYPE]"),
			      /* TRANSLATORS: command description */
			      _("Patch a firmware blob at a known offset"),
			      fu_util_firmware_patch);
	fu_util_cmd_array_add(
	    cmd_array,
	    "firmware-convert",
	    /* TRANSLATORS: command argument: uppercase, spaces->dashes */
	    _("FILENAME-SRC FILENAME-DST [FIRMWARE-TYPE-SRC] [FIRMWARE-TYPE-DST]"),
	    /* TRANSLATORS: command description */
	    _("Convert a firmware file"),
	    fu_util_firmware_convert);
	fu_util_cmd_array_add(cmd_array,
			      "firmware-build",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("BUILDER-XML FILENAME-DST"),
			      /* TRANSLATORS: command description */
			      _("Build a firmware file"),
			      fu_util_firmware_build);
	fu_util_cmd_array_add(cmd_array,
			      "firmware-parse",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME [FIRMWARE-TYPE]"),
			      /* TRANSLATORS: command description */
			      _("Parse and show details about a firmware file"),
			      fu_util_firmware_parse);
	fu_util_cmd_array_add(cmd_array,
			      "firmware-export",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME [FIRMWARE-TYPE]"),
			      /* TRANSLATORS: command description */
			      _("Export a firmware file structure to XML"),
			      fu_util_firmware_export);
	fu_util_cmd_array_add(cmd_array,
			      "firmware-extract",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("FILENAME [FIRMWARE-TYPE]"),
			      /* TRANSLATORS: command description */
			      _("Extract a firmware blob to images"),
			      fu_util_firmware_extract);
	fu_util_cmd_array_add(cmd_array,
			      "get-firmware-types",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("List the available firmware types"),
			      fu_util_get_firmware_types);
	fu_util_cmd_array_add(cmd_array,
			      "get-firmware-gtypes",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("List the available firmware GTypes"),
			      fu_util_get_firmware_gtypes);
	fu_util_cmd_array_add(cmd_array,
			      "get-remotes",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Gets the configured remotes"),
			      fu_util_get_remotes);
	fu_util_cmd_array_add(cmd_array,
			      "refresh",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Refresh metadata from remote server"),
			      fu_util_refresh);
	fu_util_cmd_array_add(cmd_array,
			      "security",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[FWUPD-VERSION]"),
			      /* TRANSLATORS: command description */
			      _("Gets the host security attributes"),
			      fu_util_security);
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
			      "emulation-load",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("EMULATION-FILE [ARCHIVE-FILE]"),
			      /* TRANSLATORS: command description */
			      _("Load device emulation data"),
			      fu_util_emulation_load);
	fu_util_cmd_array_add(cmd_array,
			      "esp-mount",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Mounts the ESP"),
			      fu_util_esp_mount);
	fu_util_cmd_array_add(cmd_array,
			      "esp-unmount",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Unmounts the ESP"),
			      fu_util_esp_unmount);
	fu_util_cmd_array_add(cmd_array,
			      "esp-list",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Lists files on the ESP"),
			      fu_util_esp_list);
	fu_util_cmd_array_add(cmd_array,
			      "switch-branch",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE-ID|GUID] [BRANCH]"),
			      /* TRANSLATORS: command description */
			      _("Switch the firmware branch on the device"),
			      fu_util_switch_branch);
	fu_util_cmd_array_add(cmd_array,
			      "clear-history",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Erase all firmware update history"),
			      fu_util_clear_history);
	fu_util_cmd_array_add(
	    cmd_array,
	    "get-bios-settings,get-bios-setting",
	    /* TRANSLATORS: command argument: uppercase, spaces->dashes */
	    _("[SETTING1] [SETTING2]..."),
	    /* TRANSLATORS: command description */
	    _("Retrieve BIOS settings.  If no arguments are passed all settings are returned"),
	    fu_util_get_bios_setting);
	fu_util_cmd_array_add(cmd_array,
			      "set-bios-setting",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("SETTING VALUE"),
			      /* TRANSLATORS: command description */
			      _("Set a BIOS setting"),
			      fu_util_set_bios_setting);
	fu_util_cmd_array_add(cmd_array,
			      "build-cabinet",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("ARCHIVE FIRMWARE METAINFO [FIRMWARE] [METAINFO] [JCATFILE]"),
			      /* TRANSLATORS: command description */
			      _("Build a cabinet archive from a firmware blob and XML metadata"),
			      fu_util_build_cabinet);
	fu_util_cmd_array_add(cmd_array,
			      "efivar-list",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      C_("command-argument", "GUID"),
			      /* TRANSLATORS: command description */
			      _("List EFI variables with a specific GUID"),
			      fu_util_efivar_list);
	fu_util_cmd_array_add(cmd_array,
			      "efiboot-info,efivar-boot",
			      /* TRANSLATORS: lowercase sub-command (do not translate): then
			       * uppercase, spaces->dashes */
			      NULL,
			      /* TRANSLATORS: command description */
			      _("List EFI boot parameters"),
			      fu_util_efiboot_info);
	fu_util_cmd_array_add(cmd_array,
			      "efiboot-next",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("INDEX"),
			      /* TRANSLATORS: command description */
			      _("Set the EFI boot next"),
			      fu_util_efiboot_next);
	fu_util_cmd_array_add(cmd_array,
			      "efiboot-order",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("INDEX1,INDEX2"),
			      /* TRANSLATORS: command description */
			      _("Set the EFI boot order"),
			      fu_util_efiboot_order);
	fu_util_cmd_array_add(cmd_array,
			      "efiboot-delete",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("INDEX"),
			      /* TRANSLATORS: command description */
			      _("Delete an EFI boot entry"),
			      fu_util_efiboot_delete);
	fu_util_cmd_array_add(cmd_array,
			      "efiboot-create",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("INDEX NAME TARGET [MOUNTPOINT]"),
			      /* TRANSLATORS: command description */
			      _("Create an EFI boot entry"),
			      fu_util_efiboot_create);
	fu_util_cmd_array_add(cmd_array,
			      "efiboot-hive",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("INDEX KEY [VALUE]"),
			      /* TRANSLATORS: command description */
			      _("Set or remove an EFI boot hive entry"),
			      fu_util_efiboot_hive);
	fu_util_cmd_array_add(cmd_array,
			      "efiboot-files,efivar-files",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      NULL,
			      /* TRANSLATORS: command description */
			      _("List EFI boot files"),
			      fu_util_efivar_files);
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
			      "reboot-cleanup",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("[DEVICE]"),
			      /* TRANSLATORS: command description */
			      _("Run the post-reboot cleanup action"),
			      fu_util_reboot_cleanup);
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
			      "enable-test-devices",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Enables virtual testing devices"),
			      fu_util_enable_test_devices);
	fu_util_cmd_array_add(cmd_array,
			      "disable-test-devices",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Disables virtual testing devices"),
			      fu_util_disable_test_devices);
	fu_util_cmd_array_add(cmd_array,
			      "get-version-formats",
			      NULL,
			      /* TRANSLATORS: command description */
			      _("Get all known version formats"),
			      fu_util_get_verfmts);
	fu_util_cmd_array_add(cmd_array,
			      "vercmp",
			      /* TRANSLATORS: command argument: uppercase, spaces->dashes */
			      _("VERSION1 VERSION2 [FORMAT]"),
			      /* TRANSLATORS: command description */
			      _("Compares two versions for equality"),
			      fu_util_vercmp);

	/* do stuff on ctrl+c */
	self->cancellable = g_cancellable_new();
	fu_util_setup_signal_handlers(self);
	g_signal_connect(G_CANCELLABLE(self->cancellable),
			 "cancelled",
			 G_CALLBACK(fu_util_cancelled_cb),
			 self);

	/* sort by command name */
	fu_util_cmd_array_sort(cmd_array);

	/* non-TTY consoles cannot answer questions */
	if (!fu_util_setup_interactive(self, &error_console)) {
		g_info("failed to initialize interactive console: %s", error_console->message);
		self->no_reboot_check = TRUE;
		self->no_safety_check = TRUE;
		self->no_device_prompt = TRUE;
	} else {
		self->interactive = TRUE;
		/* set our implemented feature set */
		fu_engine_request_set_feature_flags(
		    self->request,
		    FWUPD_FEATURE_FLAG_DETACH_ACTION | FWUPD_FEATURE_FLAG_SWITCH_BRANCH |
			FWUPD_FEATURE_FLAG_FDE_WARNING | FWUPD_FEATURE_FLAG_UPDATE_ACTION |
			FWUPD_FEATURE_FLAG_COMMUNITY_TEXT | FWUPD_FEATURE_FLAG_SHOW_PROBLEMS |
			FWUPD_FEATURE_FLAG_REQUESTS | FWUPD_FEATURE_FLAG_REQUESTS_NON_GENERIC);
	}
	fu_console_set_interactive(self->console, self->interactive);

	/* get a list of the commands */
	self->context = g_option_context_new(NULL);
	cmd_descriptions = fu_util_cmd_array_to_string(cmd_array);
	g_option_context_set_summary(self->context, cmd_descriptions);
	g_option_context_set_description(
	    self->context,
	    /* TRANSLATORS: CLI description */
	    _("This tool allows an administrator to use the fwupd plugins "
	      "without being installed on the host system."));

	/* TRANSLATORS: program name */
	g_set_application_name(_("Firmware Utility"));
	g_option_context_add_main_entries(self->context, options, NULL);
	g_option_context_add_group(self->context, fu_debug_get_option_group());
	ret = g_option_context_parse(self->context, &argc, &argv, &error);
	if (!ret) {
		fu_console_print(self->console,
				 "%s: %s",
				 /* TRANSLATORS: the user didn't read the man page */
				 _("Failed to parse arguments"),
				 error->message);
		return EXIT_FAILURE;
	}
	fu_progress_set_profile(self->progress, g_getenv("FWUPD_VERBOSE") != NULL);

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

	/* set flags */
	if (allow_reinstall)
		self->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	if (allow_older)
		self->flags |= FWUPD_INSTALL_FLAG_ALLOW_OLDER;
	if (allow_branch_switch)
		self->flags |= FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH;
	if (force)
		self->flags |= FWUPD_INSTALL_FLAG_FORCE;
	if (no_search)
		self->parse_flags |= FU_FIRMWARE_PARSE_FLAG_NO_SEARCH;
	if (ignore_checksum)
		self->parse_flags |= FU_FIRMWARE_PARSE_FLAG_IGNORE_CHECKSUM;
	if (ignore_vid_pid)
		self->parse_flags |= FU_FIRMWARE_PARSE_FLAG_IGNORE_VID_PID;
	if (ignore_requirements)
		self->flags |= FWUPD_INSTALL_FLAG_IGNORE_REQUIREMENTS;

	/* load engine */
	self->ctx = fu_context_new();
	self->engine = fu_engine_new(self->ctx);
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-request",
			 G_CALLBACK(fu_util_update_device_request_cb),
			 self);
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-added",
			 G_CALLBACK(fu_util_engine_device_added_cb),
			 self);
	g_signal_connect(FU_ENGINE(self->engine),
			 "device-removed",
			 G_CALLBACK(fu_util_engine_device_removed_cb),
			 self);
	g_signal_connect(FU_ENGINE(self->engine),
			 "status-changed",
			 G_CALLBACK(fu_util_engine_status_changed_cb),
			 self);

	/* just show versions and exit */
	if (version) {
		if (!fu_util_version(self, &error)) {
			fu_util_print_error(self, error);
			return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
	}

	/* any plugin allowlist specified */
	for (guint i = 0; plugin_glob != NULL && plugin_glob[i] != NULL; i++)
		fu_engine_add_plugin_filter(self->engine, plugin_glob[i]);

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
			fu_console_print(self->console,
					 /* TRANSLATORS: explain how to get help, %1 is
					  * 'fwupdtool --help' */
					 _("Use %s for help"),
					 "fwupdtool --help");
		} else if (g_error_matches(error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
			g_info("%s\n", error->message);
			return EXIT_NOTHING_TO_DO;
		} else if (g_error_matches(error, FWUPD_ERROR, FWUPD_ERROR_NOT_REACHABLE)) {
			g_info("%s\n", error->message);
			return EXIT_NOT_REACHABLE;
		} else if (g_error_matches(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND)) {
			g_info("%s\n", error->message);
			return EXIT_NOT_FOUND;
		}
#ifdef HAVE_GETUID
		/* if not root, then notify users on the error path */
		if (self->interactive && (getuid() != 0 || geteuid() != 0)) {
			fu_console_print_full(self->console,
					      FU_CONSOLE_PRINT_FLAG_STDERR |
						  FU_CONSOLE_PRINT_FLAG_WARNING,
					      "%s\n",
					      /* TRANSLATORS: we're poking around as a power user */
					      _("This program may only work correctly as root"));
		}
#endif
		return EXIT_FAILURE;
	}

	/* a good place to do the traceback */
	if (fu_progress_get_profile(self->progress)) {
		g_autofree gchar *str = fu_progress_traceback(self->progress);
		if (str != NULL)
			fu_console_print_literal(self->console, str);
	}

	/* success */
	return EXIT_SUCCESS;
}
