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

#include "wig-app-installer-native.h"

#include "wig-pwa-desktop-integration.h"

struct _WigAppInstallerNative {
  GObject parent;
  char *data_home;
};

static void app_installer_interface_init(WigAppInstallerInterface *interface);

G_DEFINE_FINAL_TYPE_WITH_CODE(WigAppInstallerNative, wig_app_installer_native, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(WIG_TYPE_APP_INSTALLER, app_installer_interface_init))

static void install_async(WigAppInstaller *installer, const char *id, const char *target, GVariant *manifest,
                          const char *executable, GBytes *icon, const char *const *protocols, gboolean replace_icon,
                          const char *current_desktop_file, const char *current_icon_file, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  WigAppInstallerNative *self = WIG_APP_INSTALLER_NATIVE(installer);
  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, install_async);
  if (g_task_return_error_if_cancelled(task))
    return;

  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaDesktopArtifacts) desktop_artifacts = NULL;
  if (replace_icon) {
    desktop_artifacts = wig_pwa_desktop_install(self->data_home, id, manifest, executable, icon, protocols, &error);
    if (!desktop_artifacts) {
      g_prefix_error_literal(&error, "Failed to install desktop launcher: ");
      g_task_return_error(task, g_steal_pointer(&error));
      return;
    }
  } else {
    if (!current_desktop_file || !current_icon_file) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app has invalid desktop artifacts");
      return;
    }
    if (!wig_pwa_desktop_update(self->data_home, id, manifest, executable, protocols, &error)) {
      g_prefix_error_literal(&error, "Failed to update desktop launcher: ");
      g_task_return_error(task, g_steal_pointer(&error));
      return;
    }
  }

  WigAppInstallerArtifacts *artifacts = g_new0(WigAppInstallerArtifacts, 1);
  artifacts->desktop_file = g_strdup(desktop_artifacts ? desktop_artifacts->desktop_file : current_desktop_file);
  artifacts->icon_file = g_strdup(desktop_artifacts ? desktop_artifacts->icon_file : current_icon_file);
  g_task_return_pointer(task, artifacts, (GDestroyNotify)wig_app_installer_artifacts_free);
}

static WigAppInstallerArtifacts *install_finish(WigAppInstaller *installer, GAsyncResult *result, GError **error)
{
  g_assert(g_task_is_valid(result, installer));
  g_assert(g_async_result_is_tagged(result, install_async));
  return g_task_propagate_pointer(G_TASK(result), error);
}

static gboolean uninstall(WigAppInstaller *installer, const char *id, const char *desktop_file, const char *icon_file,
                          GError **error)
{
  WigAppInstallerNative *self = WIG_APP_INSTALLER_NATIVE(installer);
  return wig_pwa_desktop_uninstall(self->data_home, id, icon_file, error);
}

static gboolean remove_icon(WigAppInstaller *installer, const char *id, const char *icon_file, GError **error)
{
  WigAppInstallerNative *self = WIG_APP_INSTALLER_NATIVE(installer);
  return wig_pwa_desktop_remove_icon(self->data_home, id, icon_file, error);
}

static GBytes *load_icon(WigAppInstaller *installer, const char *id, const char *desktop_file, const char *icon_file,
                         char **content_type, GError **error)
{
  WigAppInstallerNative *self = WIG_APP_INSTALLER_NATIVE(installer);
  return wig_pwa_desktop_load_icon(self->data_home, id, icon_file, content_type, error);
}

static void app_installer_interface_init(WigAppInstallerInterface *interface)
{
  interface->install_async = install_async;
  interface->install_finish = install_finish;
  interface->uninstall = uninstall;
  interface->remove_icon = remove_icon;
  interface->load_icon = load_icon;
}

static void wig_app_installer_native_finalize(GObject *object)
{
  WigAppInstallerNative *self = WIG_APP_INSTALLER_NATIVE(object);
  g_clear_pointer(&self->data_home, g_free);
  G_OBJECT_CLASS(wig_app_installer_native_parent_class)->finalize(object);
}

static void wig_app_installer_native_class_init(WigAppInstallerNativeClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = wig_app_installer_native_finalize;
}

static void wig_app_installer_native_init(WigAppInstallerNative *self)
{
}

WigAppInstaller *wig_app_installer_native_new(const char *data_home)
{
  g_return_val_if_fail(data_home != NULL, NULL);
  WigAppInstallerNative *self = g_object_new(WIG_TYPE_APP_INSTALLER_NATIVE, NULL);
  self->data_home = g_strdup(data_home);
  return WIG_APP_INSTALLER(self);
}
