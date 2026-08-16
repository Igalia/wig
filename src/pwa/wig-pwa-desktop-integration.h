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
} WigPwaDesktopArtifacts;

WigPwaDesktopArtifacts *wig_pwa_desktop_install(const char *data_home, const char *id, GVariant *manifest,
                                                const char *executable, GBytes *icon, const char *icon_content_type,
                                                const char *const *protocols, GError **error);
gboolean wig_pwa_desktop_update(const char *data_home, const char *id, GVariant *manifest, const char *executable,
                                const char *const *protocols, GError **error);
gboolean wig_pwa_desktop_uninstall(const char *data_home, const char *id, const char *icon_file, GError **error);
gboolean wig_pwa_desktop_remove_icon(const char *data_home, const char *id, const char *icon_file, GError **error);
GBytes *wig_pwa_desktop_load_icon(const char *data_home, const char *id, const char *icon_file, char **content_type,
                                  GError **error);
GKeyFile *wig_pwa_desktop_entry_new(const char *id, GVariant *manifest, const char *executable,
                                    const char *const *protocols);
GBytes *wig_pwa_desktop_fallback_icon(void);
void wig_pwa_desktop_artifacts_free(WigPwaDesktopArtifacts *artifacts);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WigPwaDesktopArtifacts, wig_pwa_desktop_artifacts_free)

G_END_DECLS
