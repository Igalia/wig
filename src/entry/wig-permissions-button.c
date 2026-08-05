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

#include "wig-permissions-button.h"

/* Dropdown indices map to WebKitPermissionState as follows:
 *   0 → Prompt  (WEBKIT_PERMISSION_STATE_PROMPT  = 2)
 *   1 → Granted (WEBKIT_PERMISSION_STATE_GRANTED = 0)
 *   2 → Denied  (WEBKIT_PERMISSION_STATE_DENIED  = 1)
 */
static const char *state_labels[] = { "Prompt", "Granted", "Denied", NULL };

#define DROPDOWN_INDEX_PROMPT 0
#define DROPDOWN_INDEX_GRANTED 1
#define DROPDOWN_INDEX_DENIED 2

/* One in-flight WebKitPermissionRequest, shared by every row it prompts for. */
typedef struct {
  WebKitPermissionRequest *request;
  WigPermissionKind undecided;
  gboolean denied;
} PendingRequest;

/* Per-kind arrays are indexed by wig_permission_kind_index(). */
struct _WigPermissionsButton {
  GtkWidget parent;

  GtkWidget *menu_button;
  GtkWidget *rows[WIG_PERMISSION_N_KINDS];
  GtkWidget *dropdowns[WIG_PERMISSION_N_KINDS];

  WigPermissions *permissions;
  gulong permissions_notify_id;

  PendingRequest *pending[WIG_PERMISSION_N_KINDS]; /* entries may be shared between kinds */
};

G_DEFINE_FINAL_TYPE(WigPermissionsButton, wig_permissions_button, GTK_TYPE_WIDGET)

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

static void on_dropdown_selected(GtkDropDown *dropdown, GParamSpec *pspec, WigPermissionsButton *self);

static void set_dropdown_blocked(WigPermissionsButton *self, WigPermissionKind kind, guint selected)
{
  GtkDropDown *dropdown = GTK_DROP_DOWN(self->dropdowns[wig_permission_kind_index(kind)]);
  g_signal_handlers_block_by_func(dropdown, on_dropdown_selected, self);
  gtk_drop_down_set_selected(dropdown, selected);
  g_signal_handlers_unblock_by_func(dropdown, on_dropdown_selected, self);
}

static void update_row_visibility(WigPermissionsButton *self, WigPermissionKind kind)
{
  WebKitPermissionState state = self->permissions ? wig_permissions_get_state(self->permissions, kind)
                                                  : WEBKIT_PERMISSION_STATE_PROMPT;
  guint index = wig_permission_kind_index(kind);

  /* PROMPT rows are hidden unless we are actively prompting for them. */
  gboolean visible = (state != WEBKIT_PERMISSION_STATE_PROMPT) || (self->pending[index] != NULL);
  gtk_widget_set_visible(self->rows[index], visible);
}

static void update_all_row_visibility(WigPermissionsButton *self)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1)
    update_row_visibility(self, kind);
}

static void sync_from_permissions(WigPermissionsButton *self)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    WebKitPermissionState state = self->permissions ? wig_permissions_get_state(self->permissions, kind)
                                                    : WEBKIT_PERMISSION_STATE_PROMPT;
    set_dropdown_blocked(self, kind, dropdown_index_for_state(state));
    update_row_visibility(self, kind);
  }
}

static void on_permissions_notify(WigPermissions *permissions, GParamSpec *pspec, WigPermissionsButton *self)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    if (g_strcmp0(g_param_spec_get_name(pspec), wig_permission_kind_get_property_name(kind)) != 0)
      continue;

    set_dropdown_blocked(self, kind, dropdown_index_for_state(wig_permissions_get_state(permissions, kind)));
    update_row_visibility(self, kind);
    break;
  }
}

/* Detaches @pending from every kind it was prompting for and frees it. */
static void drop_pending_request(WigPermissionsButton *self, PendingRequest *pending)
{
  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++) {
    if (self->pending[i] == pending)
      self->pending[i] = NULL;
  }

  g_clear_object(&pending->request);
  g_free(pending);
}

static void clear_pending_requests(WigPermissionsButton *self)
{
  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++) {
    if (self->pending[i])
      drop_pending_request(self, self->pending[i]);
  }
}

static gboolean has_pending_request(WigPermissionsButton *self)
{
  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++) {
    if (self->pending[i])
      return TRUE;
  }
  return FALSE;
}

/* A getUserMedia request naming both camera and microphone can only be answered
 * as a whole, so it waits until every row it prompts for has been decided. A
 * single denial is enough to answer it immediately. */
static void apply_decision_to_pending(WigPermissionsButton *self, WigPermissionKind kind, WebKitPermissionState state)
{
  PendingRequest *pending = self->pending[wig_permission_kind_index(kind)];
  if (!pending || state == WEBKIT_PERMISSION_STATE_PROMPT)
    return;

  pending->undecided &= ~kind;
  if (state == WEBKIT_PERMISSION_STATE_DENIED)
    pending->denied = TRUE;

  if (!pending->denied && pending->undecided != 0) {
    g_debug("%s decided, still awaiting kinds 0x%x", wig_permission_kind_get_label(kind), pending->undecided);
    return;
  }

  g_debug("answering %s: %s", G_OBJECT_TYPE_NAME(pending->request), pending->denied ? "deny" : "allow");

  if (pending->denied)
    webkit_permission_request_deny(pending->request);
  else
    webkit_permission_request_allow(pending->request);

  drop_pending_request(self, pending);
}

static void on_dropdown_selected(GtkDropDown *dropdown, GParamSpec *pspec, WigPermissionsButton *self)
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

  wig_permissions_set_state(self->permissions, kind, state);
  apply_decision_to_pending(self, kind, state);

  /* Answering a request can retire rows other than this one. */
  update_all_row_visibility(self);

  if (state != WEBKIT_PERMISSION_STATE_PROMPT && !has_pending_request(self))
    gtk_menu_button_popdown(GTK_MENU_BUTTON(self->menu_button));
}

static GtkWidget *build_permission_row(WigPermissionsButton *self, WigPermissionKind kind)
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

static void wig_permissions_button_dispose(GObject *object)
{
  WigPermissionsButton *self = WIG_PERMISSIONS_BUTTON(object);

  clear_pending_requests(self);

  if (self->permissions && self->permissions_notify_id) {
    g_clear_signal_handler(&self->permissions_notify_id, self->permissions);
  }
  g_clear_object(&self->permissions);

  g_clear_pointer(&self->menu_button, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_permissions_button_parent_class)->dispose(object);
}

static void wig_permissions_button_init(WigPermissionsButton *self)
{
  gtk_widget_set_layout_manager(GTK_WIDGET(self), gtk_bin_layout_new());

  /* Hidden until an origin with stored permissions is bound. */
  gtk_widget_set_visible(GTK_WIDGET(self), FALSE);

  self->menu_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(self->menu_button), "sliders-horizontal-symbolic");
  gtk_widget_add_css_class(self->menu_button, "flat");
  gtk_widget_set_parent(self->menu_button, GTK_WIDGET(self));

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++) {
    self->rows[i] = build_permission_row(self, (WigPermissionKind)(1 << i));
    gtk_box_append(GTK_BOX(box), self->rows[i]);
  }

  GtkWidget *popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(popover), box);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->menu_button), popover);
}

static void wig_permissions_button_class_init(WigPermissionsButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_permissions_button_dispose;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

GtkWidget *wig_permissions_button_new(void)
{
  return g_object_new(WIG_TYPE_PERMISSIONS_BUTTON, NULL);
}

void wig_permissions_button_set_permissions(WigPermissionsButton *self, WigPermissions *permissions)
{
  g_return_if_fail(WIG_IS_PERMISSIONS_BUTTON(self));
  g_return_if_fail(permissions == NULL || WIG_IS_PERMISSIONS(permissions));

  if (self->permissions == permissions)
    return;

  clear_pending_requests(self);

  if (self->permissions && self->permissions_notify_id)
    g_clear_signal_handler(&self->permissions_notify_id, self->permissions);

  g_set_object(&self->permissions, permissions);

  if (self->permissions)
    self->permissions_notify_id = g_signal_connect(self->permissions, "notify", G_CALLBACK(on_permissions_notify),
                                                   self);

  sync_from_permissions(self);
  gtk_widget_set_visible(GTK_WIDGET(self), self->permissions != NULL);
}

void wig_permissions_button_prompt(WigPermissionsButton *self, WigPermissions *permissions, WigPermissionKind kinds,
                                   WebKitPermissionRequest *request)
{
  g_return_if_fail(WIG_IS_PERMISSIONS_BUTTON(self));
  g_return_if_fail(WIG_IS_PERMISSIONS(permissions));
  g_return_if_fail(kinds > 0 && kinds <= WIG_PERMISSION_ALL_KINDS);
  g_return_if_fail(WEBKIT_IS_PERMISSION_REQUEST(request));

  /* Binding a different origin drops anything still pending for the old one. */
  wig_permissions_button_set_permissions(self, permissions);

  PendingRequest *pending = g_new0(PendingRequest, 1);
  pending->request = g_object_ref(request);
  pending->undecided = kinds;

  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    if (!(kinds & kind))
      continue;

    guint index = wig_permission_kind_index(kind);
    if (self->pending[index])
      drop_pending_request(self, self->pending[index]);

    self->pending[index] = pending;
    set_dropdown_blocked(self, kind, DROPDOWN_INDEX_PROMPT);
  }

  update_all_row_visibility(self);

  gtk_menu_button_popup(GTK_MENU_BUTTON(self->menu_button));
}
