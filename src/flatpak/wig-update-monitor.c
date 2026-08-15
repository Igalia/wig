/*
 * Copyright (c) 2026 Igalia S.L.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wig-update-monitor.h"

#include "wig-flatpak-portal.h"
#include "wig-flatpak.h"

/* What the portal reports when the flatpak updates permission for this application is "no". */
#define WIG_DBUS_ERROR_ACCESS_DENIED "org.freedesktop.DBus.Error.AccessDenied"

#define FLATPAK_UPDATED_FILE "/app/.updated"

/* Progress::status values of org.freedesktop.portal.Flatpak.UpdateMonitor. */
typedef enum {
  UPDATE_STATUS_RUNNING,
  UPDATE_STATUS_EMPTY,
  UPDATE_STATUS_DONE,
  UPDATE_STATUS_FAILED,
} UpdateStatus;

struct _WigUpdateMonitor {
  GObject parent;

  GCancellable *cancellable;
  GDBusConnection *connection;
  GFileMonitor *updated_file_monitor;
  char *object_path;
  char *local_commit;
  char *remote_commit;
  guint update_available_id;
  guint progress_id;
  gboolean has_updated_file;
  gboolean download_blocked;
  gboolean downloading;
  WigUpdateState state;
};

G_DEFINE_FINAL_TYPE(WigUpdateMonitor, wig_update_monitor, G_TYPE_OBJECT)

G_DEFINE_ENUM_TYPE(WigUpdateState, wig_update_state, G_DEFINE_ENUM_VALUE(WIG_UPDATE_STATE_NONE, "none"),
                   G_DEFINE_ENUM_VALUE(WIG_UPDATE_STATE_AVAILABLE, "available"),
                   G_DEFINE_ENUM_VALUE(WIG_UPDATE_STATE_DOWNLOADING, "downloading"),
                   G_DEFINE_ENUM_VALUE(WIG_UPDATE_STATE_READY, "ready"),
                   G_DEFINE_ENUM_VALUE(WIG_UPDATE_STATE_BLOCKED, "blocked"))

enum {
  PROP_STATE = 1,
  N_PROPS,
};

static GParamSpec *props[N_PROPS];

static WigUpdateState wig_update_monitor_derive_state(WigUpdateMonitor *self)
{
  if (self->downloading)
    return WIG_UPDATE_STATE_DOWNLOADING;

  gboolean download_pending = self->remote_commit && g_strcmp0(self->remote_commit, self->local_commit) != 0;

  if (download_pending && !self->download_blocked)
    return WIG_UPDATE_STATE_AVAILABLE;

  if (self->has_updated_file)
    return WIG_UPDATE_STATE_READY;

  return download_pending ? WIG_UPDATE_STATE_BLOCKED : WIG_UPDATE_STATE_NONE;
}

static void wig_update_monitor_refresh_state(WigUpdateMonitor *self)
{
  WigUpdateState state = wig_update_monitor_derive_state(self);
  if (self->state == state)
    return;

  self->state = state;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE]);
}

static void wig_update_monitor_updated_file_changed(GFileMonitor *monitor, GFile *file, GFile *other_file,
                                                    GFileMonitorEvent event_type, gpointer user_data)
{
  WigUpdateMonitor *self = user_data;

  if (event_type != G_FILE_MONITOR_EVENT_CREATED)
    return;

  g_debug("update: %s appeared, a newer commit is deployed", FLATPAK_UPDATED_FILE);

  self->has_updated_file = TRUE;
  wig_update_monitor_refresh_state(self);
}

static void wig_update_monitor_watch_updated_file(WigUpdateMonitor *self)
{
  g_autoptr(GFile) updated_file = g_file_new_for_path(FLATPAK_UPDATED_FILE);
  g_autoptr(GError) error = NULL;

  self->has_updated_file = g_file_query_exists(updated_file, NULL);

  self->updated_file_monitor = g_file_monitor_file(updated_file, G_FILE_MONITOR_NONE, self->cancellable, &error);
  if (!self->updated_file_monitor) {
    g_warning("update: cannot watch %s, an update installed by anything else will not be noticed until the portal "
              "polls: %s",
              FLATPAK_UPDATED_FILE, error->message);
    return;
  }

  g_signal_connect(self->updated_file_monitor, "changed", G_CALLBACK(wig_update_monitor_updated_file_changed), self);
}

static void wig_update_monitor_progress(GDBusConnection *connection, const char *sender_name, const char *object_path,
                                        const char *interface_name, const char *signal_name, GVariant *parameters,
                                        gpointer user_data)
{
  WigUpdateMonitor *self = user_data;
  g_autoptr(GVariant) info = NULL;
  g_variant_get(parameters, "(@a{sv})", &info);

  guint32 status = UPDATE_STATUS_RUNNING;
  guint32 operation = 0;
  guint32 operation_count = 0;
  guint32 progress = 0;
  g_variant_lookup(info, "status", "u", &status);
  g_variant_lookup(info, "op", "u", &operation);
  g_variant_lookup(info, "n_ops", "u", &operation_count);
  g_variant_lookup(info, "progress", "u", &progress);

  switch ((UpdateStatus)status) {
  case UPDATE_STATUS_RUNNING:
    g_debug("update: downloading, operation %u of %u at %u%%", operation, operation_count, progress);
    return;
  case UPDATE_STATUS_EMPTY:

    g_debug("update: the portal had nothing to install");
    self->downloading = FALSE;
    g_set_str(&self->remote_commit, self->local_commit);
    break;
  case UPDATE_STATUS_DONE:
    g_debug("update: downloaded, a restart will pick it up");
    self->downloading = FALSE;
    g_set_str(&self->local_commit, self->remote_commit);
    break;
  case UPDATE_STATUS_FAILED: {
    const char *error_name = NULL;
    const char *error_message = NULL;
    g_variant_lookup(info, "error", "&s", &error_name);
    g_variant_lookup(info, "error_message", "&s", &error_message);
    g_warning("update: download failed: %s: %s", error_name ? error_name : "(unknown error)",
              error_message ? error_message : "");

    self->downloading = FALSE;
    /* Refusal is a stored answer, not a transient failure, so offering the download again
     * would fail the same way until it is changed with
     * flatpak permission-set flatpak updates com.igalia.wig ask */
    if (g_strcmp0(error_name, WIG_DBUS_ERROR_ACCESS_DENIED) == 0)
      self->download_blocked = TRUE;
    break;
  }
  default:
    g_debug("update: unhandled install status %u", status);
    return;
  }

  wig_update_monitor_refresh_state(self);
}

static void wig_update_monitor_update_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigUpdateMonitor) self = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (reply)
    return;

  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  g_warning("update: the portal refused to download the update: %s", error->message);
  self->downloading = FALSE;
  wig_update_monitor_refresh_state(self);
}

static void wig_update_monitor_update_available(GDBusConnection *connection, const char *sender_name,
                                                const char *object_path, const char *interface_name,
                                                const char *signal_name, GVariant *parameters, gpointer user_data)
{
  WigUpdateMonitor *self = user_data;
  g_autoptr(GVariant) info = NULL;
  g_variant_get(parameters, "(@a{sv})", &info);

  const char *running_commit = NULL;
  const char *local_commit = NULL;
  const char *remote_commit = NULL;
  g_variant_lookup(info, "running-commit", "&s", &running_commit);
  g_variant_lookup(info, "local-commit", "&s", &local_commit);
  g_variant_lookup(info, "remote-commit", "&s", &remote_commit);

  g_debug("update: available, running %s, local %s, remote %s", running_commit, local_commit, remote_commit);

  g_set_str(&self->local_commit, local_commit);
  g_set_str(&self->remote_commit, remote_commit);

  /* Nothing left to fetch is what withdraws a refusal: the answer it was stored against
   * no longer has anything to apply to. */
  if (g_strcmp0(remote_commit, local_commit) == 0)
    self->download_blocked = FALSE;

  wig_update_monitor_refresh_state(self);
}

static void wig_update_monitor_created(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigUpdateMonitor) self = user_data;
  GDBusConnection *connection = G_DBUS_CONNECTION(source);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(connection, result, &error);
  if (!reply) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("update: the portal would not create an update monitor: %s", error->message);
    return;
  }

  g_variant_get(reply, "(o)", &self->object_path);
  g_debug("update: monitoring %s", self->object_path);

  self->update_available_id = g_dbus_connection_signal_subscribe(
      connection, WIG_FLATPAK_PORTAL_BUS_NAME, WIG_FLATPAK_PORTAL_UPDATE_MONITOR_INTERFACE, "UpdateAvailable",
      self->object_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, wig_update_monitor_update_available, self, NULL);
  self->progress_id = g_dbus_connection_signal_subscribe(
      connection, WIG_FLATPAK_PORTAL_BUS_NAME, WIG_FLATPAK_PORTAL_UPDATE_MONITOR_INTERFACE, "Progress",
      self->object_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, wig_update_monitor_progress, self, NULL);
}

static void wig_update_monitor_bus_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigUpdateMonitor) self = user_data;
  g_autoptr(GError) error = NULL;
  GDBusConnection *connection = g_bus_get_finish(result, &error);
  if (!connection) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("update: no session bus, updates are not monitored: %s", error->message);
    return;
  }

  self->connection = connection;

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call(connection, WIG_FLATPAK_PORTAL_BUS_NAME, WIG_FLATPAK_PORTAL_OBJECT_PATH,
                         WIG_FLATPAK_PORTAL_INTERFACE, "CreateUpdateMonitor",
                         g_variant_new("(@a{sv})", g_variant_builder_end(&options)), G_VARIANT_TYPE("(o)"),
                         G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable, wig_update_monitor_created, g_object_ref(self));
}

static void wig_update_monitor_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigUpdateMonitor *self = WIG_UPDATE_MONITOR(object);

  switch (prop_id) {
  case PROP_STATE:
    g_value_set_enum(value, (int)self->state);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_update_monitor_dispose(GObject *object)
{
  WigUpdateMonitor *self = WIG_UPDATE_MONITOR(object);

  g_cancellable_cancel(self->cancellable);

  if (self->connection) {
    if (self->update_available_id) {
      g_dbus_connection_signal_unsubscribe(self->connection, self->update_available_id);
      self->update_available_id = 0;
    }
    if (self->progress_id) {
      g_dbus_connection_signal_unsubscribe(self->connection, self->progress_id);
      self->progress_id = 0;
    }
    if (self->object_path) {
      g_dbus_connection_call(self->connection, WIG_FLATPAK_PORTAL_BUS_NAME, self->object_path,
                             WIG_FLATPAK_PORTAL_UPDATE_MONITOR_INTERFACE, "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                             -1, NULL, NULL, NULL);
    }
  }

  if (self->updated_file_monitor)
    g_file_monitor_cancel(self->updated_file_monitor);

  g_clear_pointer(&self->object_path, g_free);
  g_clear_pointer(&self->local_commit, g_free);
  g_clear_pointer(&self->remote_commit, g_free);
  g_clear_object(&self->updated_file_monitor);
  g_clear_object(&self->connection);
  g_clear_object(&self->cancellable);

  G_OBJECT_CLASS(wig_update_monitor_parent_class)->dispose(object);
}

static void wig_update_monitor_class_init(WigUpdateMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->get_property = wig_update_monitor_get_property;
  gobject_class->dispose = wig_update_monitor_dispose;

  props[PROP_STATE] = g_param_spec_enum("state", NULL, NULL, WIG_TYPE_UPDATE_STATE, WIG_UPDATE_STATE_NONE,
                                        G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(gobject_class, N_PROPS, props);
}

static void wig_update_monitor_init(WigUpdateMonitor *self)
{
  self->cancellable = g_cancellable_new();

  wig_update_monitor_watch_updated_file(self);
  wig_update_monitor_refresh_state(self);
}

WigUpdateMonitor *wig_update_monitor_new(void)
{
  g_assert(wig_in_flatpak());

  WigUpdateMonitor *self = g_object_new(WIG_TYPE_UPDATE_MONITOR, NULL);

  g_bus_get(G_BUS_TYPE_SESSION, self->cancellable, wig_update_monitor_bus_ready, g_object_ref(self));
  return self;
}

WigUpdateState wig_update_monitor_get_state(WigUpdateMonitor *self)
{
  return self->state;
}

void wig_update_monitor_download(WigUpdateMonitor *self)
{
  if (self->state != WIG_UPDATE_STATE_AVAILABLE)
    return;

  self->downloading = TRUE;
  wig_update_monitor_refresh_state(self);
  g_debug("update: asking the portal to download the update");

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call(self->connection, WIG_FLATPAK_PORTAL_BUS_NAME, self->object_path,
                         WIG_FLATPAK_PORTAL_UPDATE_MONITOR_INTERFACE, "Update",
                         g_variant_new("(s@a{sv})", "", g_variant_builder_end(&options)), NULL, G_DBUS_CALL_FLAGS_NONE,
                         -1, self->cancellable, wig_update_monitor_update_finished, g_object_ref(self));
}

/* Installed it sits in libexec; in a build that has not been installed it is next
 * to the browser itself. */
static char *wig_restart_helper_path(void)
{
  g_autofree char *installed = g_build_filename(WIG_LIBEXECDIR, "wig-restart-helper", NULL);
  if (g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE))
    return g_steal_pointer(&installed);

  g_autofree char *self = g_file_read_link("/proc/self/exe", NULL);
  if (!self)
    return g_steal_pointer(&installed);

  g_autofree char *directory = g_path_get_dirname(self);
  return g_build_filename(directory, "wig-restart-helper", NULL);
}

/* The replacement instance cannot be started from here: it would find this process still
 * owning the application's bus name and hand its startup back to the instance that is on its
 * way out. The helper waits for the name to go away and asks the portal instead. */
gboolean wig_update_monitor_spawn_restart_helper(GError **error)
{
  g_autofree char *path = wig_restart_helper_path();
  g_autoptr(GSubprocess) helper = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, error, path, NULL);
  if (!helper)
    return FALSE;

  g_debug("restart: launched %s to start the browser again", path);
  return TRUE;
}
