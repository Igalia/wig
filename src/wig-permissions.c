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
  const char *request_phrase;
} WigPermissionInfo;

/* Indexed by wig_permission_kind_index(), so ordered as WigPermissionKind is. */
static const WigPermissionInfo permission_infos[WIG_PERMISSION_N_KINDS] = {
  { "camera", "camera-web-symbolic", "Camera", "use your camera" },
  { "microphone", "audio-input-microphone-symbolic", "Microphone", "use your microphone" },
  { "device-info", "camera-small-symbolic", "Device Info", "see the names of your cameras and microphones" },
  { "notification", "preferences-system-notifications-symbolic", "Notifications", "send you notifications" },
  { "clipboard", "edit-paste-symbolic", "Clipboard", "read your clipboard" },
  { "geolocation", "find-location-symbolic", "Location", "know your location" },
  { "media-key-system", "video-x-generic-symbolic", "Protected Media", "play protected media" },
  { "xr", "video-display-symbolic", "Virtual Reality", "start an immersive session" },
};

guint wig_permission_kind_index(WigPermissionKind kind)
{
  return (guint)g_bit_nth_lsf(kind, -1);
}

static WigPermissionKind user_media_kinds(WebKitUserMediaPermissionRequest *request)
{
  /* Screen sharing is not a per-origin permission of ours; the portal gates it
   * instead, so it never reaches the permissions popover. */
  if (webkit_user_media_permission_is_for_display_device(request))
    return 0;

  WigPermissionKind kinds = 0;
  if (webkit_user_media_permission_is_for_video_device(request))
    kinds |= WIG_PERMISSION_CAMERA;
  if (webkit_user_media_permission_is_for_audio_device(request))
    kinds |= WIG_PERMISSION_MICROPHONE;

  return kinds;
}

gboolean wig_permission_request_is_display_capture(WebKitPermissionRequest *request)
{
  return WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)
      && webkit_user_media_permission_is_for_display_device(WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request));
}

WigPermissionKind wig_permission_kinds_for_request(WebKitPermissionRequest *request)
{
  if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request))
    return user_media_kinds(WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request));
  if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(request))
    return WIG_PERMISSION_DEVICE_INFO;
  if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(request))
    return WIG_PERMISSION_NOTIFICATION;
  if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(request))
    return WIG_PERMISSION_GEOLOCATION;
  if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(request))
    return WIG_PERMISSION_MEDIA_KEY_SYSTEM;
#if HAVE_CLIPBOARD_PERMISSION_SUPPORT
  if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(request))
    return WIG_PERMISSION_CLIPBOARD;
#endif
#if HAVE_XR_PERMISSION_SUPPORT
  if (WEBKIT_IS_XR_PERMISSION_REQUEST(request))
    return WIG_PERMISSION_XR;
#endif
  return 0;
}

const char *wig_permission_kind_get_icon_name(WigPermissionKind kind)
{
  return permission_infos[wig_permission_kind_index(kind)].icon_name;
}

const char *wig_permission_kind_get_label(WigPermissionKind kind)
{
  return permission_infos[wig_permission_kind_index(kind)].label;
}

const char *wig_permission_kind_get_property_name(WigPermissionKind kind)
{
  return permission_infos[wig_permission_kind_index(kind)].property_name;
}

const char *wig_permission_kind_get_request_phrase(WigPermissionKind kind)
{
  return permission_infos[wig_permission_kind_index(kind)].request_phrase;
}

struct _WigPermissions {
  GObject parent;

  WebKitPermissionState states[WIG_PERMISSION_N_KINDS];
  gboolean session_only[WIG_PERMISSION_N_KINDS];
};

G_DEFINE_FINAL_TYPE(WigPermissions, wig_permissions, G_TYPE_OBJECT)

/* Property ids map to kinds as PROP_<kind> = wig_permission_kind_index() + 1
 * (PROP_0 is reserved). */
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
    wig_permissions_set_state(self, (WigPermissionKind)(1 << (prop_id - 1)),
                              (WebKitPermissionState)g_value_get_enum(value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static void wig_permissions_class_init(WigPermissionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = wig_permissions_get_property;
  object_class->set_property = wig_permissions_set_property;

  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++) {
    properties[i + 1] = g_param_spec_enum(permission_infos[i].property_name, NULL, NULL, WEBKIT_TYPE_PERMISSION_STATE,
                                          WEBKIT_PERMISSION_STATE_PROMPT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  }

  g_object_class_install_properties(object_class, WIG_PERMISSION_N_KINDS + 1, properties);
}

static void wig_permissions_init(WigPermissions *self)
{
  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++)
    self->states[i] = WEBKIT_PERMISSION_STATE_PROMPT;
}

WigPermissions *wig_permissions_new(void)
{
  return g_object_new(WIG_TYPE_PERMISSIONS, NULL);
}

WebKitPermissionState wig_permissions_get_state(WigPermissions *self, WigPermissionKind kind)
{
  g_return_val_if_fail(WIG_IS_PERMISSIONS(self), WEBKIT_PERMISSION_STATE_PROMPT);

  return self->states[wig_permission_kind_index(kind)];
}

static void set_state(WigPermissions *self, WigPermissionKind kind, WebKitPermissionState state, gboolean session_only)
{
  guint index = wig_permission_kind_index(kind);

  session_only = session_only && state != WEBKIT_PERMISSION_STATE_PROMPT;

  if (self->states[index] == state && self->session_only[index] == session_only)
    return;

  self->states[index] = state;
  self->session_only[index] = session_only;
  g_object_notify_by_pspec(G_OBJECT(self), properties[index + 1]);
}

void wig_permissions_set_state(WigPermissions *self, WigPermissionKind kind, WebKitPermissionState state)
{
  set_state(self, kind, state, FALSE);
}

void wig_permissions_set_session_state(WigPermissions *self, WigPermissionKind kind, WebKitPermissionState state)
{
  set_state(self, kind, state, TRUE);
}

gboolean wig_permissions_is_session_only(WigPermissions *self, WigPermissionKind kind)
{
  return self->session_only[wig_permission_kind_index(kind)];
}
