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

#include <wpe/webkit.h>

#include "wig-pwa-launch-handler.h"
#include "wig-pwa-navigation-policy.h"

G_BEGIN_DECLS

#define WIG_TYPE_PWA_MANAGER (wig_pwa_manager_get_type())
G_DECLARE_FINAL_TYPE(WigPwaManager, wig_pwa_manager, WIG, PWA_MANAGER, GObject)

WigPwaManager *wig_pwa_manager_new(const char *config_dir, const char *data_dir, WebKitNetworkSession *network_session);
#if HAVE_APPLICATION_MANIFEST_SUPPORT
void wig_pwa_manager_install_async(WigPwaManager *self, WebKitApplicationManifest *manifest, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer user_data);
void wig_pwa_manager_refresh_async(WigPwaManager *self, WebKitApplicationManifest *manifest, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer user_data);
#endif
void wig_pwa_manager_install_manifest_async(WigPwaManager *self, const char *manifest_url, GVariant *manifest,
                                            GLoadableIcon *icon, GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data);
gboolean wig_pwa_manager_install_finish(WigPwaManager *self, GAsyncResult *result, GError **error);
gboolean wig_pwa_manager_is_installed(WigPwaManager *self, const char *id, GError **error);
GVariant *wig_pwa_manager_list(WigPwaManager *self, GError **error);
GVariant *wig_pwa_manager_get_details(WigPwaManager *self, const char *id, GError **error);
GBytes *wig_pwa_manager_get_cached_icon(WigPwaManager *self, const char *id, char **content_type, GError **error);
char *wig_pwa_manager_get_start_url(WigPwaManager *self, const char *id, GError **error);
char *wig_pwa_manager_resolve_protocol_url(WigPwaManager *self, const char *id, const char *url, GError **error);
WigPwaLaunchClientMode wig_pwa_manager_get_launch_client_mode(WigPwaManager *self, const char *id, GError **error);
gboolean wig_pwa_manager_set_launch_client_mode_override(WigPwaManager *self, const char *id, const char *client_mode,
                                                         GError **error);
gboolean wig_pwa_manager_get_open_primary_scope_in_browser(WigPwaManager *self, const char *id, GError **error);
gboolean wig_pwa_manager_set_open_primary_scope_in_browser(WigPwaManager *self, const char *id, gboolean enabled,
                                                           GError **error);
char *wig_pwa_manager_find_for_primary_scope(WigPwaManager *self, const char *url, GError **error);
char *wig_pwa_manager_get_background_color(WigPwaManager *self, const char *id, GError **error);
WigPwaNavigationPolicy *wig_pwa_manager_get_navigation_policy(WigPwaManager *self, const char *id, GError **error);
GVariant *wig_pwa_manager_get_shortcuts(WigPwaManager *self, const char *id, GError **error);
gboolean wig_pwa_manager_uninstall(WigPwaManager *self, const char *id, GError **error);

G_END_DECLS
