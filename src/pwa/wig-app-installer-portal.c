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

#include "wig-app-installer-portal.h"

#include "wig-pwa-desktop-integration.h"
#include "wig-pwa-util.h"

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define DYNAMIC_LAUNCHER_INTERFACE "org.freedesktop.portal.DynamicLauncher"
#define REQUEST_INTERFACE "org.freedesktop.portal.Request"
#define WEB_APP_LAUNCHER_TYPE 2

typedef struct {
  GDBusConnection *connection;
  char *desktop_file_id;
  char *desktop_entry;
  char *name;
  char *target;
  char *request_path;
  GVariant *icon;
  GCancellable *cancellable;
  gulong cancelled_id;
  guint response_subscription;
  gboolean install_started;
} DynamicLauncherInstallData;

struct _WigAppInstallerPortal {
  GObject parent;
};

static void app_installer_interface_init(WigAppInstallerInterface *interface);

G_DEFINE_FINAL_TYPE_WITH_CODE(WigAppInstallerPortal, wig_app_installer_portal, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(WIG_TYPE_APP_INSTALLER, app_installer_interface_init))

static void dynamic_launcher_install_data_free(DynamicLauncherInstallData *data)
{
  if (data->response_subscription)
    g_dbus_connection_signal_unsubscribe(data->connection, data->response_subscription);
  if (data->cancelled_id)
    g_cancellable_disconnect(data->cancellable, data->cancelled_id);
  g_clear_object(&data->connection);
  g_clear_object(&data->cancellable);
  g_clear_pointer(&data->desktop_file_id, g_free);
  g_clear_pointer(&data->desktop_entry, g_free);
  g_clear_pointer(&data->name, g_free);
  g_clear_pointer(&data->target, g_free);
  g_clear_pointer(&data->request_path, g_free);
  g_clear_pointer(&data->icon, g_variant_unref);
  g_free(data);
}

static void disconnect_cancellable(DynamicLauncherInstallData *data)
{
  if (!data->cancelled_id)
    return;
  gulong cancelled_id = data->cancelled_id;
  data->cancelled_id = 0;
  g_cancellable_disconnect(data->cancellable, cancelled_id);
}

static gboolean cancel_install(gpointer user_data)
{
  GTask *task = G_TASK(user_data);
  if (g_task_get_completed(task))
    return G_SOURCE_REMOVE;

  DynamicLauncherInstallData *data = g_task_get_task_data(task);
  if (data->install_started)
    return G_SOURCE_REMOVE;
  disconnect_cancellable(data);
  if (data->request_path) {
    g_dbus_connection_call(data->connection, PORTAL_BUS_NAME, data->request_path, REQUEST_INTERFACE, "Close", NULL,
                           G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  }
  if (data->response_subscription) {
    guint subscription = data->response_subscription;
    data->response_subscription = 0;
    g_dbus_connection_signal_unsubscribe(data->connection, subscription);
  }
  g_task_return_error_if_cancelled(task);
  return G_SOURCE_REMOVE;
}

static void install_cancelled(GCancellable *cancellable, GTask *task)
{
  g_main_context_invoke_full(g_task_get_context(task), G_PRIORITY_DEFAULT, cancel_install, g_object_ref(task),
                             g_object_unref);
}

static char *dynamic_launcher_desktop_file_id(const char *id)
{
  g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
  return g_strconcat("com.igalia.wig.WebApp_", hash, ".desktop", NULL);
}

static GVariant *serialize_icon(GBytes *icon, const char *content_type)
{
  g_autoptr(GBytes) bytes = NULL;
  if (icon) {
    g_assert(g_strcmp0(content_type, "image/png") == 0);
    bytes = g_bytes_ref(icon);
  }
  if (!bytes)
    bytes = wig_pwa_desktop_fallback_icon();

  g_autoptr(GIcon) bytes_icon = g_bytes_icon_new(bytes);
  return g_icon_serialize(bytes_icon);
}

static void dynamic_launcher_installed(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!reply) {
    g_prefix_error_literal(&error, "DynamicLauncher.Install failed: ");
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  DynamicLauncherInstallData *data = g_task_get_task_data(task);
  g_debug("pwa: installed portal launcher %s", data->desktop_file_id);
  WigAppInstallerArtifacts *artifacts = g_new0(WigAppInstallerArtifacts, 1);
  artifacts->desktop_file = g_strdup(data->desktop_file_id);
  artifacts->can_rollback = TRUE;
  g_task_return_pointer(task, artifacts, (GDestroyNotify)wig_app_installer_artifacts_free);
}

static void dynamic_launcher_response(GDBusConnection *connection, const char *sender_name, const char *object_path,
                                      const char *interface_name, const char *signal_name, GVariant *parameters,
                                      gpointer user_data)
{
  g_autoptr(GTask) task = g_object_ref(G_TASK(user_data));
  DynamicLauncherInstallData *data = g_task_get_task_data(task);
  data->install_started = TRUE;
  disconnect_cancellable(data);
  guint subscription = data->response_subscription;
  data->response_subscription = 0;
  g_dbus_connection_signal_unsubscribe(connection, subscription);

  guint32 response;
  g_autoptr(GVariant) results = NULL;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  if (response != 0) {
    if (response == 1)
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Web app installation was cancelled");
    else
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "DynamicLauncher.PrepareInstall failed");
    return;
  }

  const char *token = NULL;
  if (!g_variant_lookup(results, "token", "&s", &token) || !token || !*token) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "DynamicLauncher.PrepareInstall returned no install token");
    return;
  }

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_debug("pwa: installing portal desktop entry:\n%s", data->desktop_entry);
  g_dbus_connection_call(
      connection, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, DYNAMIC_LAUNCHER_INTERFACE, "Install",
      g_variant_new("(sss@a{sv})", token, data->desktop_file_id, data->desktop_entry, g_variant_builder_end(&options)),
      G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE, -1, g_task_get_cancellable(task), dynamic_launcher_installed,
      g_object_ref(task));
}

static void subscribe_to_response(GTask *task)
{
  DynamicLauncherInstallData *data = g_task_get_task_data(task);
  data->response_subscription = g_dbus_connection_signal_subscribe(
      data->connection, PORTAL_BUS_NAME, REQUEST_INTERFACE, "Response", data->request_path, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, dynamic_launcher_response, g_object_ref(task), g_object_unref);
}

static void dynamic_launcher_prepared(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  DynamicLauncherInstallData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (g_task_get_completed(task))
    return;
  if (!reply) {
    disconnect_cancellable(data);
    if (data->response_subscription) {
      guint subscription = data->response_subscription;
      data->response_subscription = 0;
      g_dbus_connection_signal_unsubscribe(data->connection, subscription);
    }
    g_prefix_error_literal(&error, "DynamicLauncher.PrepareInstall failed: ");
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  const char *returned_path;
  g_variant_get(reply, "(&o)", &returned_path);
  if (g_str_equal(returned_path, data->request_path))
    return;

  guint subscription = data->response_subscription;
  data->response_subscription = 0;
  g_dbus_connection_signal_unsubscribe(data->connection, subscription);
  g_free(data->request_path);
  data->request_path = g_strdup(returned_path);
  subscribe_to_response(task);
}

static void dynamic_launcher_bus_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  DynamicLauncherInstallData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  data->connection = g_bus_get_finish(result, &error);
  if (!data->connection) {
    g_prefix_error_literal(&error, "Failed to connect to the session bus: ");
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  const char *unique_name = g_dbus_connection_get_unique_name(data->connection);
  if (!unique_name) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Session bus connection has no unique name");
    return;
  }
  g_autofree char *sender = g_strdup(unique_name + 1);
  g_strdelimit(sender, ".", '_');
  g_autofree char *handle_token = g_strdup_printf("wig_pwa_%08x%08x", g_random_int(), g_random_int());
  data->request_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, handle_token);
  subscribe_to_response(task);
  GCancellable *cancellable = g_task_get_cancellable(task);
  if (cancellable) {
    g_set_object(&data->cancellable, cancellable);
    data->cancelled_id = g_cancellable_connect(cancellable, G_CALLBACK(install_cancelled), g_object_ref(task),
                                               g_object_unref);
  }

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(handle_token));
  g_variant_builder_add(&options, "{sv}", "launcher_type", g_variant_new_uint32(WEB_APP_LAUNCHER_TYPE));
  g_variant_builder_add(&options, "{sv}", "target", g_variant_new_string(data->target));
  g_variant_builder_add(&options, "{sv}", "editable_name", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&options, "{sv}", "editable_icon", g_variant_new_boolean(FALSE));

  g_debug("pwa: preparing portal launcher %s", data->desktop_file_id);
  g_dbus_connection_call(
      data->connection, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, DYNAMIC_LAUNCHER_INTERFACE, "PrepareInstall",
      g_variant_new("(ssv@a{sv})", "", data->name, data->icon, g_variant_builder_end(&options)), G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NONE, -1, g_task_get_cancellable(task), dynamic_launcher_prepared, g_object_ref(task));
}

static void install_async(WigAppInstaller *installer, const char *id, const char *target, GVariant *manifest,
                          const char *executable, GBytes *icon, const char *icon_content_type, int icon_size,
                          const char *const *protocols, gboolean replace_icon, const char *current_desktop_file,
                          const char *current_icon_file, GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  if (!replace_icon)
    g_debug("pwa: portal installer cannot preserve the existing launcher icon; reinstalling it");
  if (icon_size > 0)
    g_debug("pwa: portal installer ignores the requested icon size of %d pixels", icon_size);
  if (current_desktop_file || current_icon_file)
    g_debug("pwa: portal installer ignores existing launcher artifact paths");

  g_autoptr(GTask) task = g_task_new(installer, cancellable, callback, user_data);
  g_task_set_source_tag(task, install_async);
  DynamicLauncherInstallData *data = g_new0(DynamicLauncherInstallData, 1);
  data->desktop_file_id = dynamic_launcher_desktop_file_id(id);
  g_autofree char *command = g_path_get_basename(executable);
  g_autoptr(GKeyFile) desktop_entry = wig_pwa_desktop_entry_new(id, manifest, command, protocols);
  data->desktop_entry = g_key_file_to_data(desktop_entry, NULL, NULL);
  const char *name = wig_pwa_variant_lookup_string(manifest, "name");
  if (!name || !*name)
    name = wig_pwa_variant_lookup_string(manifest, "short_name");
  data->name = g_strdup(name && *name ? name : id);
  data->target = g_strdup(target);
  data->icon = serialize_icon(icon, icon_content_type);
  g_task_set_task_data(task, data, (GDestroyNotify)dynamic_launcher_install_data_free);

  g_bus_get(G_BUS_TYPE_SESSION, cancellable, dynamic_launcher_bus_ready, g_object_ref(task));
}

static WigAppInstallerArtifacts *install_finish(WigAppInstaller *installer, GAsyncResult *result, GError **error)
{
  g_assert(g_task_is_valid(result, installer));
  g_assert(g_async_result_is_tagged(result, install_async));
  return g_task_propagate_pointer(G_TASK(result), error);
}

static GDBusConnection *get_session_bus(GError **error)
{
  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
  if (!connection)
    g_prefix_error_literal(error, "Failed to connect to the session bus: ");
  return connection;
}

static gboolean uninstall(WigAppInstaller *installer, const char *id, const char *desktop_file_id,
                          const char *icon_file, GError **error)
{
  g_autoptr(GDBusConnection) connection = get_session_bus(error);
  if (!connection)
    return FALSE;
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      connection, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, DYNAMIC_LAUNCHER_INTERFACE, "Uninstall",
      g_variant_new("(s@a{sv})", desktop_file_id, g_variant_builder_end(&options)), G_VARIANT_TYPE_UNIT,
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!reply) {
    g_prefix_error_literal(error, "DynamicLauncher.Uninstall failed: ");
    return FALSE;
  }
  return TRUE;
}

static gboolean remove_icon(WigAppInstaller *installer, const char *id, const char *icon_file, GError **error)
{
  return TRUE;
}

static GBytes *load_icon(WigAppInstaller *installer, const char *id, const char *desktop_file_id, const char *icon_file,
                         char **content_type, GError **error)
{
  g_autoptr(GDBusConnection) connection = get_session_bus(error);
  if (!connection)
    return NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      connection, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, DYNAMIC_LAUNCHER_INTERFACE, "GetIcon",
      g_variant_new("(s)", desktop_file_id), G_VARIANT_TYPE("(vsu)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (!reply) {
    g_prefix_error_literal(error, "DynamicLauncher.GetIcon failed: ");
    return NULL;
  }

  g_autoptr(GVariant) wrapped_icon = NULL;
  const char *format;
  guint32 size;
  g_variant_get(reply, "(@v&su)", &wrapped_icon, &format, &size);
  g_autoptr(GVariant) serialized_icon = g_variant_get_variant(wrapped_icon);
  g_autoptr(GIcon) icon = g_icon_deserialize(serialized_icon);
  if (!icon || !G_IS_BYTES_ICON(icon)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "DynamicLauncher returned an invalid icon");
    return NULL;
  }

  if (g_str_equal(format, "png"))
    *content_type = g_strdup("image/png");
  else if (g_str_equal(format, "jpeg"))
    *content_type = g_strdup("image/jpeg");
  else if (g_str_equal(format, "svg"))
    *content_type = g_strdup("image/svg+xml");
  else {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "DynamicLauncher returned unknown icon format '%s'",
                format);
    return NULL;
  }
  return g_bytes_ref(g_bytes_icon_get_bytes(G_BYTES_ICON(icon)));
}

static void app_installer_interface_init(WigAppInstallerInterface *interface)
{
  interface->install_async = install_async;
  interface->install_finish = install_finish;
  interface->uninstall = uninstall;
  interface->remove_icon = remove_icon;
  interface->load_icon = load_icon;
}

static void wig_app_installer_portal_class_init(WigAppInstallerPortalClass *klass)
{
}

static void wig_app_installer_portal_init(WigAppInstallerPortal *self)
{
}

WigAppInstaller *wig_app_installer_portal_new(void)
{
  return g_object_new(WIG_TYPE_APP_INSTALLER_PORTAL, NULL);
}
