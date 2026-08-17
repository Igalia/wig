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

#include <adwaita.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef enum {
  WIG_PERMISSION_CAMERA = 1 << 0,
  WIG_PERMISSION_MICROPHONE = 1 << 1,
  WIG_PERMISSION_DEVICE_INFO = 1 << 2,
  WIG_PERMISSION_NOTIFICATION = 1 << 3,
  WIG_PERMISSION_CLIPBOARD = 1 << 4,
  WIG_PERMISSION_GEOLOCATION = 1 << 5,
  WIG_PERMISSION_MEDIA_KEY_SYSTEM = 1 << 6,
  WIG_PERMISSION_XR = 1 << 7,
} WigPermissionKind;

#define WIG_PERMISSION_N_KINDS 8
#define WIG_PERMISSION_ALL_KINDS ((WigPermissionKind)((1 << WIG_PERMISSION_N_KINDS) - 1))

WigPermissionKind wig_permission_kinds_for_request(WebKitPermissionRequest *request);
gboolean wig_permission_request_is_display_capture(WebKitPermissionRequest *request);
guint wig_permission_kind_index(WigPermissionKind kind);
const char *wig_permission_kind_get_icon_name(WigPermissionKind kind);
const char *wig_permission_kind_get_label(WigPermissionKind kind);
const char *wig_permission_kind_get_property_name(WigPermissionKind kind);
const char *wig_permission_kind_get_request_phrase(WigPermissionKind kind);

#define WIG_TYPE_PERMISSIONS (wig_permissions_get_type())
G_DECLARE_FINAL_TYPE(WigPermissions, wig_permissions, WIG, PERMISSIONS, GObject)

WigPermissions *wig_permissions_new(void);
WebKitPermissionState wig_permissions_get_state(WigPermissions *self, WigPermissionKind kind);
void wig_permissions_set_state(WigPermissions *self, WigPermissionKind kind, WebKitPermissionState state);
void wig_permissions_set_session_state(WigPermissions *self, WigPermissionKind kind, WebKitPermissionState state);
gboolean wig_permissions_is_session_only(WigPermissions *self, WigPermissionKind kind);

G_END_DECLS
