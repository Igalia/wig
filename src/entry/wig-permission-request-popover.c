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

#include "wig-permission-request-popover.h"

struct _WigPermissionRequestPopover {
  GtkPopover parent;

  GtkWidget *icon;
  GtkWidget *description;

  WigPermissions *permissions;
  WebKitPermissionRequest *request;
  WigPermissionKind kinds;
};

G_DEFINE_FINAL_TYPE(WigPermissionRequestPopover, wig_permission_request_popover, GTK_TYPE_POPOVER)

typedef enum {
  PROP_PROMPTING = 1,
} WigPermissionRequestPopoverProps;

static GParamSpec *props[PROP_PROMPTING + 1];

static char *describe_request(const char *origin, WigPermissionKind kinds)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();

  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    if (kinds & kind)
      g_strv_builder_add(builder, wig_permission_kind_get_request_phrase(kind));
  }

  g_auto(GStrv) phrases = g_strv_builder_end(builder);
  g_autofree char *joined = g_strjoinv(" and ", phrases);
  return g_strdup_printf("%s wants to %s.", origin, joined);
}

static WigPermissionKind first_kind(WigPermissionKind kinds)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    if (kinds & kind)
      return kind;
  }
  return 0;
}

static void clear_request(WigPermissionRequestPopover *self)
{
  g_clear_object(&self->request);
  g_clear_object(&self->permissions);
  self->kinds = 0;

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PROMPTING]);
}

static void answer_request(WigPermissionRequestPopover *self, WebKitPermissionState state, gboolean session_only)
{
  g_assert(self->request != NULL);

  g_debug("answering %s: %s%s", G_OBJECT_TYPE_NAME(self->request),
          state == WEBKIT_PERMISSION_STATE_GRANTED ? "allow" : "deny", session_only ? " for this session" : "");

  /* A request naming several devices is answered as a whole, so the choice
   * applies to every kind it asked about. */
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    if (!(self->kinds & kind))
      continue;

    if (session_only)
      wig_permissions_set_session_state(self->permissions, kind, state);
    else
      wig_permissions_set_state(self->permissions, kind, state);
  }

  if (state == WEBKIT_PERMISSION_STATE_GRANTED)
    webkit_permission_request_allow(self->request);
  else
    webkit_permission_request_deny(self->request);

  clear_request(self);
  gtk_popover_popdown(GTK_POPOVER(self));
}

static void on_allow_always_clicked(GtkButton *button, WigPermissionRequestPopover *self)
{
  answer_request(self, WEBKIT_PERMISSION_STATE_GRANTED, FALSE);
}

static void on_allow_once_clicked(GtkButton *button, WigPermissionRequestPopover *self)
{
  answer_request(self, WEBKIT_PERMISSION_STATE_GRANTED, TRUE);
}

static void on_never_allow_clicked(GtkButton *button, WigPermissionRequestPopover *self)
{
  answer_request(self, WEBKIT_PERMISSION_STATE_DENIED, FALSE);
}

/* Clicking away is an answer of "ask later". */
static void on_closed(WigPermissionRequestPopover *self)
{
  if (!self->request)
    return;

  g_debug("permission request %s dismissed without an answer", G_OBJECT_TYPE_NAME(self->request));
  webkit_permission_request_deny(self->request);
  clear_request(self);
}

static GtkWidget *build_choice_button(const char *label, GCallback callback, WigPermissionRequestPopover *self)
{
  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_set_hexpand(button, TRUE);
  g_signal_connect(button, "clicked", callback, self);

  return button;
}

static void wig_permission_request_popover_dispose(GObject *object)
{
  WigPermissionRequestPopover *self = WIG_PERMISSION_REQUEST_POPOVER(object);

  if (self->request)
    webkit_permission_request_deny(self->request);
  g_clear_object(&self->request);
  g_clear_object(&self->permissions);

  G_OBJECT_CLASS(wig_permission_request_popover_parent_class)->dispose(object);
}

static void wig_permission_request_popover_get_property(GObject *object, guint prop_id, GValue *value,
                                                        GParamSpec *pspec)
{
  WigPermissionRequestPopover *self = WIG_PERMISSION_REQUEST_POPOVER(object);

  switch ((WigPermissionRequestPopoverProps)prop_id) {
  case PROP_PROMPTING:
    g_value_set_boolean(value, self->request != NULL);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_permission_request_popover_init(WigPermissionRequestPopover *self)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_size_request(box, 320, -1);

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  self->icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(self->icon), 32);
  gtk_widget_set_valign(self->icon, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header), self->icon);

  self->description = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(self->description), TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->description), 0.0);
  gtk_widget_set_hexpand(self->description, TRUE);
  gtk_box_append(GTK_BOX(header), self->description);
  gtk_box_append(GTK_BOX(box), header);

  GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *allow_always = build_choice_button("Allow while visiting this site", G_CALLBACK(on_allow_always_clicked),
                                                self);
  gtk_widget_add_css_class(allow_always, "suggested-action");
  gtk_box_append(GTK_BOX(choices), allow_always);
  gtk_box_append(GTK_BOX(choices), build_choice_button("Allow this time", G_CALLBACK(on_allow_once_clicked), self));
  gtk_box_append(GTK_BOX(choices), build_choice_button("Never Allow", G_CALLBACK(on_never_allow_clicked), self));
  gtk_box_append(GTK_BOX(box), choices);

  gtk_popover_set_child(GTK_POPOVER(self), box);

  g_signal_connect(self, "closed", G_CALLBACK(on_closed), NULL);
}

static void wig_permission_request_popover_class_init(WigPermissionRequestPopoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_permission_request_popover_dispose;
  object_class->get_property = wig_permission_request_popover_get_property;

  props[PROP_PROMPTING] = g_param_spec_boolean("prompting", NULL, NULL, FALSE,
                                               G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, G_N_ELEMENTS(props), props);
}

GtkWidget *wig_permission_request_popover_new(void)
{
  return g_object_new(WIG_TYPE_PERMISSION_REQUEST_POPOVER, NULL);
}

void wig_permission_request_popover_prompt(WigPermissionRequestPopover *self, WigPermissions *permissions,
                                           const char *origin, WigPermissionKind kinds,
                                           WebKitPermissionRequest *request)
{
  g_assert(kinds > 0 && kinds <= WIG_PERMISSION_ALL_KINDS);

  /* Only one question fits on screen, so an unanswered predecessor is treated
   * as ignored rather than queued behind this one. */
  if (self->request) {
    g_debug("replacing unanswered %s with %s", G_OBJECT_TYPE_NAME(self->request), G_OBJECT_TYPE_NAME(request));
    webkit_permission_request_deny(self->request);
    g_clear_object(&self->request);
    g_clear_object(&self->permissions);
  }

  self->request = g_object_ref(request);
  self->permissions = g_object_ref(permissions);
  self->kinds = kinds;

  gtk_image_set_from_icon_name(GTK_IMAGE(self->icon), wig_permission_kind_get_icon_name(first_kind(kinds)));
  g_autofree char *description = describe_request(origin, kinds);
  gtk_label_set_text(GTK_LABEL(self->description), description);

  /* Presenting is the window's job: it owns the button this is shown from. */
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PROMPTING]);
}
