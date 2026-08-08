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

#include "wig-permissions-popover.h"

/* Dropdown indices map to WebKitPermissionState as follows:
 *   0 → Prompt  (WEBKIT_PERMISSION_STATE_PROMPT  = 2)
 *   1 → Granted (WEBKIT_PERMISSION_STATE_GRANTED = 0)
 *   2 → Denied  (WEBKIT_PERMISSION_STATE_DENIED  = 1)
 */
static const char *state_labels[] = { "Prompt", "Granted", "Denied", NULL };

#define DROPDOWN_INDEX_PROMPT 0
#define DROPDOWN_INDEX_GRANTED 1
#define DROPDOWN_INDEX_DENIED 2

/* Per-kind arrays are indexed by wig_permission_kind_index(). */
struct _WigPermissionsPopover {
  GtkPopover parent;

  GtkWidget *rows[WIG_PERMISSION_N_KINDS];
  GtkWidget *dropdowns[WIG_PERMISSION_N_KINDS];

  WigPermissions *permissions;
  gulong permissions_notify_id;
};

G_DEFINE_FINAL_TYPE(WigPermissionsPopover, wig_permissions_popover, GTK_TYPE_POPOVER)

typedef enum {
  PROP_HAS_PERMISSIONS = 1,
} WigPermissionsPopoverProps;

static GParamSpec *props[PROP_HAS_PERMISSIONS + 1];

static guint dropdown_index_for_state(WebKitPermissionState state)
{
  switch (state) {
  case WEBKIT_PERMISSION_STATE_GRANTED:
    return DROPDOWN_INDEX_GRANTED;
  case WEBKIT_PERMISSION_STATE_DENIED:
    return DROPDOWN_INDEX_DENIED;
  case WEBKIT_PERMISSION_STATE_PROMPT:
  default:
    return DROPDOWN_INDEX_PROMPT;
  }
}

static WebKitPermissionState state_for_dropdown_index(guint index)
{
  switch (index) {
  case DROPDOWN_INDEX_GRANTED:
    return WEBKIT_PERMISSION_STATE_GRANTED;
  case DROPDOWN_INDEX_DENIED:
    return WEBKIT_PERMISSION_STATE_DENIED;
  case DROPDOWN_INDEX_PROMPT:
  default:
    return WEBKIT_PERMISSION_STATE_PROMPT;
  }
}

static void on_dropdown_selected(GtkDropDown *dropdown, GParamSpec *pspec, WigPermissionsPopover *self);

static void set_dropdown_blocked(WigPermissionsPopover *self, WigPermissionKind kind, guint selected)
{
  GtkDropDown *dropdown = GTK_DROP_DOWN(self->dropdowns[wig_permission_kind_index(kind)]);
  g_signal_handlers_block_by_func(dropdown, on_dropdown_selected, self);
  gtk_drop_down_set_selected(dropdown, selected);
  g_signal_handlers_unblock_by_func(dropdown, on_dropdown_selected, self);
}

static void update_row_visibility(WigPermissionsPopover *self, WigPermissionKind kind)
{
  WebKitPermissionState state = self->permissions ? wig_permissions_get_state(self->permissions, kind)
                                                  : WEBKIT_PERMISSION_STATE_PROMPT;

  /* Only answered permissions are listed; the rest are nothing to manage. */
  gtk_widget_set_visible(self->rows[wig_permission_kind_index(kind)], state != WEBKIT_PERMISSION_STATE_PROMPT);
}

static void sync_from_permissions(WigPermissionsPopover *self)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    WebKitPermissionState state = self->permissions ? wig_permissions_get_state(self->permissions, kind)
                                                    : WEBKIT_PERMISSION_STATE_PROMPT;
    set_dropdown_blocked(self, kind, dropdown_index_for_state(state));
    update_row_visibility(self, kind);
  }
}

static void on_permissions_notify(WigPermissions *permissions, GParamSpec *pspec, WigPermissionsPopover *self)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    if (g_strcmp0(g_param_spec_get_name(pspec), wig_permission_kind_get_property_name(kind)) != 0)
      continue;

    set_dropdown_blocked(self, kind, dropdown_index_for_state(wig_permissions_get_state(permissions, kind)));
    update_row_visibility(self, kind);
    break;
  }
}

static void on_dropdown_selected(GtkDropDown *dropdown, GParamSpec *pspec, WigPermissionsPopover *self)
{
  WigPermissionKind kind = 0;
  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++) {
    if (GTK_WIDGET(dropdown) == self->dropdowns[i]) {
      kind = (WigPermissionKind)(1 << i);
      break;
    }
  }
  if (kind == 0 || !self->permissions)
    return;

  WebKitPermissionState state = state_for_dropdown_index(gtk_drop_down_get_selected(dropdown));

  /* Choosing here is a lasting answer, so a grant made for one session earlier
   * becomes permanent. */
  wig_permissions_set_state(self->permissions, kind, state);
  update_row_visibility(self, kind);
}

static GtkWidget *build_permission_row(WigPermissionsPopover *self, WigPermissionKind kind)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(row, 6);
  gtk_widget_set_margin_end(row, 6);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  GtkWidget *icon = gtk_image_new_from_icon_name(wig_permission_kind_get_icon_name(kind));
  gtk_box_append(GTK_BOX(row), icon);

  GtkWidget *label = gtk_label_new(wig_permission_kind_get_label(kind));
  gtk_widget_set_hexpand(label, TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_box_append(GTK_BOX(row), label);

  GtkWidget *dropdown = gtk_drop_down_new_from_strings(state_labels);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), DROPDOWN_INDEX_PROMPT);
  self->dropdowns[wig_permission_kind_index(kind)] = dropdown;
  g_signal_connect_object(dropdown, "notify::selected", G_CALLBACK(on_dropdown_selected), self, G_CONNECT_DEFAULT);
  gtk_box_append(GTK_BOX(row), dropdown);

  return row;
}

static void wig_permissions_popover_dispose(GObject *object)
{
  WigPermissionsPopover *self = WIG_PERMISSIONS_POPOVER(object);

  if (self->permissions && self->permissions_notify_id) {
    g_clear_signal_handler(&self->permissions_notify_id, self->permissions);
  }
  g_clear_object(&self->permissions);

  G_OBJECT_CLASS(wig_permissions_popover_parent_class)->dispose(object);
}

static void wig_permissions_popover_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigPermissionsPopover *self = WIG_PERMISSIONS_POPOVER(object);
  switch ((WigPermissionsPopoverProps)prop_id) {
  case PROP_HAS_PERMISSIONS:
    g_value_set_boolean(value, self->permissions != NULL);
    break;
  }
}

static void wig_permissions_popover_init(WigPermissionsPopover *self)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++) {
    self->rows[i] = build_permission_row(self, (WigPermissionKind)(1 << i));
    gtk_box_append(GTK_BOX(box), self->rows[i]);
  }

  gtk_popover_set_child(GTK_POPOVER(self), box);
}

static void wig_permissions_popover_class_init(WigPermissionsPopoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_permissions_popover_dispose;
  object_class->get_property = wig_permissions_popover_get_property;

  /* The button owning this popover has nothing to offer until an origin with
   * stored permissions is bound, so it follows this rather than hiding itself. */
  props[PROP_HAS_PERMISSIONS] = g_param_spec_boolean("has-permissions", NULL, NULL, FALSE,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, G_N_ELEMENTS(props), props);
}

GtkWidget *wig_permissions_popover_new(void)
{
  return g_object_new(WIG_TYPE_PERMISSIONS_POPOVER, NULL);
}

void wig_permissions_popover_set_permissions(WigPermissionsPopover *self, WigPermissions *permissions)
{
  g_return_if_fail(WIG_IS_PERMISSIONS_POPOVER(self));
  g_return_if_fail(permissions == NULL || WIG_IS_PERMISSIONS(permissions));

  if (self->permissions == permissions)
    return;

  if (self->permissions && self->permissions_notify_id)
    g_clear_signal_handler(&self->permissions_notify_id, self->permissions);

  g_set_object(&self->permissions, permissions);

  if (self->permissions)
    self->permissions_notify_id = g_signal_connect(self->permissions, "notify", G_CALLBACK(on_permissions_notify),
                                                   self);

  sync_from_permissions(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_HAS_PERMISSIONS]);
}
