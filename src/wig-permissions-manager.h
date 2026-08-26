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

#include "wig-permission-request-popover.h"
#include "wig-permissions.h"

G_BEGIN_DECLS

#define WIG_TYPE_PERMISSIONS_MANAGER (wig_permissions_manager_get_type())
G_DECLARE_FINAL_TYPE(WigPermissionsManager, wig_permissions_manager, WIG, PERMISSIONS_MANAGER, GObject)

WigPermissionsManager *wig_permissions_manager_new(const char *state_dir);
gboolean wig_permissions_manager_load(WigPermissionsManager *self, GError **error);
WigPermissions *wig_permissions_manager_lookup(WigPermissionsManager *self, const char *origin);
WigPermissions *wig_permissions_manager_ensure(WigPermissionsManager *self, const char *origin);
void wig_permissions_manager_visit(WigPermissionsManager *self, const char *origin);
GList *wig_permissions_manager_list_sites(WigPermissionsManager *self, WigPermissionKind kind,
                                          WebKitPermissionState state);

gboolean wig_permissions_manager_get_autoplay(WigPermissionsManager *self, const char *origin,
                                              WebKitAutoplayPolicy *autoplay);
void wig_permissions_manager_set_autoplay(WigPermissionsManager *self, const char *origin,
                                          WebKitAutoplayPolicy autoplay);
void wig_permissions_manager_clear_autoplay(WigPermissionsManager *self, const char *origin);
GList *wig_permissions_manager_list_autoplay_sites(WigPermissionsManager *self, WebKitAutoplayPolicy autoplay);

#if HAVE_UPGRADE_TO_HTTPS_POLICY_SUPPORT
gboolean wig_permissions_manager_get_https_navigation(WigPermissionsManager *self, const char *origin,
                                                      WebKitUpgradeToHTTPSPolicy *https_navigation);
void wig_permissions_manager_set_https_navigation(WigPermissionsManager *self, const char *origin,
                                                  WebKitUpgradeToHTTPSPolicy https_navigation);
void wig_permissions_manager_clear_https_navigation(WigPermissionsManager *self, const char *origin);
GList *wig_permissions_manager_list_https_navigation_sites(WigPermissionsManager *self,
                                                           WebKitUpgradeToHTTPSPolicy https_navigation);
#endif

const char *wig_permissions_manager_get_user_agent(WigPermissionsManager *self, const char *origin);
void wig_permissions_manager_set_user_agent(WigPermissionsManager *self, const char *origin, const char *user_agent);
GList *wig_permissions_manager_list_user_agent_sites(WigPermissionsManager *self);
GList *wig_permissions_manager_list_origins(WigPermissionsManager *self, WigPermissionKind kind,
                                            WebKitPermissionState state);
void wig_permissions_manager_save(WigPermissionsManager *self);
void wig_permissions_manager_handle_request(WigPermissionsManager *self, const char *origin,
                                            WebKitPermissionRequest *request, WigPermissionRequestPopover *popover);

G_END_DECLS
