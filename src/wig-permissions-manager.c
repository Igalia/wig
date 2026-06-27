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

#include "wig-permissions-manager.h"

struct _WigPermissionsManager {
  GObject parent;

  GHashTable *origins; /* char* origin -> WigPermissions* (owned) */
};

G_DEFINE_FINAL_TYPE(WigPermissionsManager, wig_permissions_manager, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void wig_permissions_manager_dispose(GObject *object)
{
  WigPermissionsManager *self = WIG_PERMISSIONS_MANAGER(object);

  g_clear_pointer(&self->origins, g_hash_table_unref);

  G_OBJECT_CLASS(wig_permissions_manager_parent_class)->dispose(object);
}

static void wig_permissions_manager_class_init(WigPermissionsManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_permissions_manager_dispose;

  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void wig_permissions_manager_init(WigPermissionsManager *self)
{
  self->origins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}

WigPermissionsManager *wig_permissions_manager_new(void)
{
  return g_object_new(WIG_TYPE_PERMISSIONS_MANAGER, NULL);
}

WigPermissions *wig_permissions_manager_lookup(WigPermissionsManager *self, const char *origin)
{
  g_return_val_if_fail(WIG_IS_PERMISSIONS_MANAGER(self), NULL);
  g_return_val_if_fail(origin != NULL, NULL);

  return g_hash_table_lookup(self->origins, origin);
}

WigPermissions *wig_permissions_manager_ensure(WigPermissionsManager *self, const char *origin)
{
  g_return_val_if_fail(WIG_IS_PERMISSIONS_MANAGER(self), NULL);
  g_return_val_if_fail(origin != NULL, NULL);

  WigPermissions *permissions = g_hash_table_lookup(self->origins, origin);
  if (permissions)
    return permissions;

  permissions = wig_permissions_new();
  g_hash_table_insert(self->origins, g_strdup(origin), permissions);

  g_signal_emit(self, signals[SIGNAL_CHANGED], 0, origin);

  return permissions;
}

void wig_permissions_manager_handle_request(WigPermissionsManager *self, const char *origin,
                                            WebKitPermissionRequest *request, WigPermissionsButton *button)
{
  g_return_if_fail(WIG_IS_PERMISSIONS_MANAGER(self));
  g_return_if_fail(origin != NULL);
  g_return_if_fail(WEBKIT_IS_PERMISSION_REQUEST(request));
  g_return_if_fail(WIG_IS_PERMISSIONS_BUTTON(button));

  WigPermissionKind kind = wig_permission_kind_for_request(request);
  WigPermissions *permissions = wig_permissions_manager_lookup(self, origin);

  if (permissions) {
    switch (wig_permissions_get_state(permissions, kind)) {
    case WEBKIT_PERMISSION_STATE_GRANTED:
      webkit_permission_request_allow(request);
      return;
    case WEBKIT_PERMISSION_STATE_DENIED:
      webkit_permission_request_deny(request);
      return;
    case WEBKIT_PERMISSION_STATE_PROMPT:
    default:
      break;
    }
  }

  /* No stored decision: remember the origin (making the button visible) and
   * prompt the user for this specific permission. */
  permissions = wig_permissions_manager_ensure(self, origin);
  wig_permissions_button_prompt(button, permissions, kind, request);
}
