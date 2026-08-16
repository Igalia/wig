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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  char *desktop_file;
  char *icon_file;
} WigAppInstallerArtifacts;

void wig_app_installer_artifacts_free(WigAppInstallerArtifacts *artifacts);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WigAppInstallerArtifacts, wig_app_installer_artifacts_free)

#define WIG_TYPE_APP_INSTALLER (wig_app_installer_get_type())
G_DECLARE_INTERFACE(WigAppInstaller, wig_app_installer, WIG, APP_INSTALLER, GObject)

struct _WigAppInstallerInterface {
  GTypeInterface parent;

  void (*install_async)(WigAppInstaller *self, const char *id, const char *target, GVariant *manifest,
                        const char *executable, GBytes *icon, const char *const *protocols, gboolean replace_icon,
                        const char *current_desktop_file, const char *current_icon_file, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data);
  WigAppInstallerArtifacts *(*install_finish)(WigAppInstaller *self, GAsyncResult *result, GError **error);
  gboolean (*uninstall)(WigAppInstaller *self, const char *id, const char *desktop_file, const char *icon_file,
                        GError **error);
  gboolean (*remove_icon)(WigAppInstaller *self, const char *id, const char *icon_file, GError **error);
  GBytes *(*load_icon)(WigAppInstaller *self, const char *id, const char *desktop_file, const char *icon_file,
                       char **content_type, GError **error);
  gboolean (*needs_confirmation)(WigAppInstaller *self);
};

void wig_app_installer_install_async(WigAppInstaller *self, const char *id, const char *target, GVariant *manifest,
                                     const char *executable, GBytes *icon, const char *const *protocols,
                                     gboolean replace_icon, const char *current_desktop_file,
                                     const char *current_icon_file, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
WigAppInstallerArtifacts *wig_app_installer_install_finish(WigAppInstaller *self, GAsyncResult *result, GError **error);
gboolean wig_app_installer_uninstall(WigAppInstaller *self, const char *id, const char *desktop_file,
                                     const char *icon_file, GError **error);
gboolean wig_app_installer_remove_icon(WigAppInstaller *self, const char *id, const char *icon_file, GError **error);
GBytes *wig_app_installer_load_icon(WigAppInstaller *self, const char *id, const char *desktop_file,
                                    const char *icon_file, char **content_type, GError **error);
gboolean wig_app_installer_needs_confirmation(WigAppInstaller *self);

G_END_DECLS
