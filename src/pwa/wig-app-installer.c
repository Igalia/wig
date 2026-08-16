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

#include "wig-app-installer.h"

G_DEFINE_INTERFACE(WigAppInstaller, wig_app_installer, G_TYPE_OBJECT)

static void wig_app_installer_default_init(WigAppInstallerInterface *interface)
{
}

void wig_app_installer_artifacts_free(WigAppInstallerArtifacts *artifacts)
{
  g_clear_pointer(&artifacts->desktop_file, g_free);
  g_clear_pointer(&artifacts->icon_file, g_free);
  g_free(artifacts);
}

void wig_app_installer_install_async(WigAppInstaller *self, const char *id, const char *target, GVariant *manifest,
                                     const char *executable, GBytes *icon, const char *icon_content_type, int icon_size,
                                     const char *const *protocols, gboolean replace_icon,
                                     const char *current_desktop_file, const char *current_icon_file,
                                     GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(WIG_IS_APP_INSTALLER(self));
  g_return_if_fail(id != NULL);
  g_return_if_fail(target != NULL);
  g_return_if_fail(manifest && g_variant_is_of_type(manifest, G_VARIANT_TYPE_VARDICT));
  g_return_if_fail(executable != NULL);
  g_return_if_fail(icon_size >= 0);
  g_return_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable));

  WIG_APP_INSTALLER_GET_IFACE(self)->install_async(self, id, target, manifest, executable, icon, icon_content_type,
                                                   icon_size, protocols, replace_icon, current_desktop_file,
                                                   current_icon_file, cancellable, callback, user_data);
}

WigAppInstallerArtifacts *wig_app_installer_install_finish(WigAppInstaller *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(WIG_IS_APP_INSTALLER(self), NULL);
  g_return_val_if_fail(G_IS_ASYNC_RESULT(result), NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  return WIG_APP_INSTALLER_GET_IFACE(self)->install_finish(self, result, error);
}

gboolean wig_app_installer_uninstall(WigAppInstaller *self, const char *id, const char *desktop_file,
                                     const char *icon_file, GError **error)
{
  g_return_val_if_fail(WIG_IS_APP_INSTALLER(self), FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(desktop_file != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  return WIG_APP_INSTALLER_GET_IFACE(self)->uninstall(self, id, desktop_file, icon_file, error);
}

gboolean wig_app_installer_remove_icon(WigAppInstaller *self, const char *id, const char *icon_file, GError **error)
{
  g_return_val_if_fail(WIG_IS_APP_INSTALLER(self), FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  return WIG_APP_INSTALLER_GET_IFACE(self)->remove_icon(self, id, icon_file, error);
}

GBytes *wig_app_installer_load_icon(WigAppInstaller *self, const char *id, const char *desktop_file,
                                    const char *icon_file, char **content_type, GError **error)
{
  g_return_val_if_fail(WIG_IS_APP_INSTALLER(self), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(desktop_file != NULL, NULL);
  g_return_val_if_fail(content_type != NULL && *content_type == NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  return WIG_APP_INSTALLER_GET_IFACE(self)->load_icon(self, id, desktop_file, icon_file, content_type, error);
}
