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

#include "wig-permissions.h"

typedef struct {
  const char *property_name;
  const char *icon_name;
  const char *label;
} WigPermissionInfo;

static const WigPermissionInfo permission_infos[WIG_PERMISSION_N_KINDS] = {
  [WIG_PERMISSION_DEVICE_INFO] = { "device-info", "camera-small-symbolic", "Device Info" },
  [WIG_PERMISSION_NOTIFICATION] = { "notification", "preferences-system-notifications-symbolic", "Notifications" },
};

WigPermissionKind wig_permission_kind_for_request(WebKitPermissionRequest *request)
{
  if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(request))
    return WIG_PERMISSION_DEVICE_INFO;
  return WIG_PERMISSION_NOTIFICATION;
}

const char *wig_permission_kind_get_icon_name(WigPermissionKind kind)
{
  g_return_val_if_fail(kind < WIG_PERMISSION_N_KINDS, NULL);
  return permission_infos[kind].icon_name;
}

const char *wig_permission_kind_get_label(WigPermissionKind kind)
{
  g_return_val_if_fail(kind < WIG_PERMISSION_N_KINDS, NULL);
  return permission_infos[kind].label;
}

const char *wig_permission_kind_get_property_name(WigPermissionKind kind)
{
  g_return_val_if_fail(kind < WIG_PERMISSION_N_KINDS, NULL);
  return permission_infos[kind].property_name;
}

struct _WigPermissions {
  GObject parent;

  WebKitPermissionState states[WIG_PERMISSION_N_KINDS];
};

G_DEFINE_FINAL_TYPE(WigPermissions, wig_permissions, G_TYPE_OBJECT)

/* Property ids map to kinds as PROP_<kind> = kind + 1 (PROP_0 is reserved). */
static GParamSpec *properties[WIG_PERMISSION_N_KINDS + 1];

static void wig_permissions_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigPermissions *self = WIG_PERMISSIONS(object);

  if (prop_id >= 1 && prop_id <= WIG_PERMISSION_N_KINDS)
    g_value_set_enum(value, (int)self->states[prop_id - 1]);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static void wig_permissions_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigPermissions *self = WIG_PERMISSIONS(object);

  if (prop_id >= 1 && prop_id <= WIG_PERMISSION_N_KINDS)
    wig_permissions_set_state(self, prop_id - 1, (WebKitPermissionState)g_value_get_enum(value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static void wig_permissions_class_init(WigPermissionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = wig_permissions_get_property;
  object_class->set_property = wig_permissions_set_property;

  for (WigPermissionKind kind = 0; kind < WIG_PERMISSION_N_KINDS; kind++) {
    properties[kind + 1] = g_param_spec_enum(permission_infos[kind].property_name, NULL, NULL,
                                             WEBKIT_TYPE_PERMISSION_STATE, WEBKIT_PERMISSION_STATE_PROMPT,
                                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  }

  g_object_class_install_properties(object_class, WIG_PERMISSION_N_KINDS + 1, properties);
}

static void wig_permissions_init(WigPermissions *self)
{
  for (WigPermissionKind kind = 0; kind < WIG_PERMISSION_N_KINDS; kind++)
    self->states[kind] = WEBKIT_PERMISSION_STATE_PROMPT;
}

WigPermissions *wig_permissions_new(void)
{
  return g_object_new(WIG_TYPE_PERMISSIONS, NULL);
}

WebKitPermissionState wig_permissions_get_state(WigPermissions *self, WigPermissionKind kind)
{
  g_return_val_if_fail(WIG_IS_PERMISSIONS(self), WEBKIT_PERMISSION_STATE_PROMPT);
  g_return_val_if_fail(kind < WIG_PERMISSION_N_KINDS, WEBKIT_PERMISSION_STATE_PROMPT);

  return self->states[kind];
}

void wig_permissions_set_state(WigPermissions *self, WigPermissionKind kind, WebKitPermissionState state)
{
  g_return_if_fail(WIG_IS_PERMISSIONS(self));
  g_return_if_fail(kind < WIG_PERMISSION_N_KINDS);

  if (self->states[kind] == state)
    return;

  self->states[kind] = state;
  g_object_notify_by_pspec(G_OBJECT(self), properties[kind + 1]);
}
