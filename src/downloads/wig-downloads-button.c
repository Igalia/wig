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

#include "wig-downloads-button.h"

#include "wig-application.h"
#include "wig-downloads-list.h"
#include "wig-downloads-paintable.h"

#define ATTENTION_MS 2000
#define AUTO_CLOSE_MS 6000
#define LIST_WIDTH 460
#define LIST_HEIGHT 420

struct _WigDownloadsButton {
  GtkWidget parent;

  GtkWidget *revealer;
  GtkWidget *menu_button;
  GtkWidget *image;
  GtkWidget *popover;
  GtkWidget *clear_button;
  GtkWidget *list;
  WigDownloadsPaintable *paintable;

  WigDownloadsManager *manager;
  GtkEventController *window_clicks;
  GtkEventController *window_keys;
  gboolean popover_open;
  gboolean showed_itself;
  guint attention_id;
  guint auto_close_id;
};

G_DEFINE_FINAL_TYPE(WigDownloadsButton, wig_downloads_button, GTK_TYPE_WIDGET)

static void wig_downloads_button_sync(WigDownloadsButton *self)
{
  wig_downloads_list_sync(WIG_DOWNLOADS_LIST(self->list));
  gtk_widget_set_sensitive(self->clear_button, wig_downloads_manager_has_finished(self->manager));
}

static void wig_downloads_button_update(WigDownloadsButton *self)
{
  /* Once the popover is up the button has to stay put underneath it, even for an
   * empty list the user asked to see. */
  gboolean any = !wig_downloads_manager_is_empty(self->manager);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->revealer), any || self->popover_open);

  double progress = wig_downloads_manager_get_progress(self->manager);
  wig_downloads_paintable_set_active(self->paintable, progress > 0);
  wig_downloads_paintable_set_progress(self->paintable, progress);

  /* A closed popover is caught up when it is shown, so progress ticks only cost
   * a list rebuild while it is actually on screen. */
  if (self->popover_open)
    wig_downloads_button_sync(self);
}

static void wig_downloads_button_clear_clicked(WigDownloadsButton *self)
{
  wig_downloads_list_clear_error(WIG_DOWNLOADS_LIST(self->list));
  wig_downloads_manager_clear_finished(self->manager);
}

static void wig_downloads_button_attention_over(WigDownloadsButton *self)
{
  self->attention_id = 0;
  gtk_widget_remove_css_class(self->image, "accent");
}

static void wig_downloads_button_close_unless_used(gpointer user_data)
{
  WigDownloadsButton *self = user_data;

  self->auto_close_id = 0;

  g_debug("downloads: the popover showed itself and went unused, closing it");
  gtk_menu_button_popdown(GTK_MENU_BUTTON(self->menu_button));
}

static void wig_downloads_button_arm_auto_close(WigDownloadsButton *self)
{
  g_clear_handle_id(&self->auto_close_id, g_source_remove);
  self->auto_close_id = g_timeout_add_once(AUTO_CLOSE_MS, wig_downloads_button_close_unless_used, self);
}

static void wig_downloads_button_popover_used(WigDownloadsButton *self)
{
  if (!self->showed_itself)
    return;

  g_debug("downloads: the popover is being used, leaving it up");

  self->showed_itself = FALSE;
  g_clear_handle_id(&self->auto_close_id, g_source_remove);
}

static void wig_downloads_button_download_added(WigDownloadsButton *self)
{
  g_clear_handle_id(&self->attention_id, g_source_remove);
  gtk_widget_add_css_class(self->image, "accent");
  self->attention_id = g_timeout_add_once(ATTENTION_MS, (GSourceOnceFunc)wig_downloads_button_attention_over, self);

  wig_downloads_button_update(self);

  if (!self->popover_open) {
    g_debug("downloads: showing the popover for a download that just started");
    self->showed_itself = TRUE;
    gtk_menu_button_popup(GTK_MENU_BUTTON(self->menu_button));
  }

  /* Another download arriving while the list is still showing itself is worth
   * another moment of it, but a popover the user opened is left alone. */
  if (self->showed_itself)
    wig_downloads_button_arm_auto_close(self);
}

static void wig_downloads_button_download_completed(WigDownloadsButton *self)
{
  wig_downloads_paintable_flash_done(self->paintable);
}

/* Presses inside the popover go to its own surface, so one arriving here was
 * aimed at something else in the window and the popover is in the way of it. */
static void wig_downloads_button_window_pressed(WigDownloadsButton *self, int n_press, double x, double y,
                                                GtkGestureClick *click)
{
  GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
  GtkWidget *pressed = root ? gtk_widget_pick(root, x, y, GTK_PICK_DEFAULT) : NULL;

  /* The button itself already answers a press by closing what it opened. */
  if (pressed && (pressed == self->menu_button || gtk_widget_is_ancestor(pressed, self->menu_button)))
    return;

  g_debug("downloads: something else in the window was pressed, closing the popover");
  gtk_menu_button_popdown(GTK_MENU_BUTTON(self->menu_button));
}

static gboolean wig_downloads_button_window_key_pressed(WigDownloadsButton *self, guint keyval, guint keycode,
                                                        GdkModifierType state, GtkEventControllerKey *keys)
{
  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;

  g_debug("downloads: escape pressed, closing the popover");
  gtk_menu_button_popdown(GTK_MENU_BUTTON(self->menu_button));

  return GDK_EVENT_PROPAGATE;
}

/* Watched only while the popover is up, so the rest of the window is left to
 * itself the rest of the time. Both run in the capture phase: what closes the
 * popover is still free to do whatever it was for. */
static void wig_downloads_button_watch_window(WigDownloadsButton *self)
{
  GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
  if (!root || self->window_clicks)
    return;

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect_swapped(click, "pressed", G_CALLBACK(wig_downloads_button_window_pressed), self);
  self->window_clicks = GTK_EVENT_CONTROLLER(click);
  gtk_widget_add_controller(root, self->window_clicks);

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect_swapped(keys, "key-pressed", G_CALLBACK(wig_downloads_button_window_key_pressed), self);
  self->window_keys = keys;
  gtk_widget_add_controller(root, self->window_keys);
}

static void wig_downloads_button_unwatch_window(WigDownloadsButton *self)
{
  GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));

  if (!root) {
    self->window_clicks = NULL;
    self->window_keys = NULL;
    return;
  }

  if (self->window_clicks)
    gtk_widget_remove_controller(root, g_steal_pointer(&self->window_clicks));
  if (self->window_keys)
    gtk_widget_remove_controller(root, g_steal_pointer(&self->window_keys));
}

static void wig_downloads_button_popover_shown(WigDownloadsButton *self)
{
  self->popover_open = TRUE;

  wig_downloads_list_clear_error(WIG_DOWNLOADS_LIST(self->list));
  wig_downloads_button_sync(self);
  wig_downloads_button_watch_window(self);
}

static void wig_downloads_button_popover_closed(WigDownloadsButton *self)
{
  self->popover_open = FALSE;
  self->showed_itself = FALSE;
  g_clear_handle_id(&self->auto_close_id, g_source_remove);

  wig_downloads_button_unwatch_window(self);
  wig_downloads_button_update(self);
}

static void wig_downloads_button_dispose(GObject *object)
{
  WigDownloadsButton *self = WIG_DOWNLOADS_BUTTON(object);

  g_clear_handle_id(&self->attention_id, g_source_remove);
  g_clear_handle_id(&self->auto_close_id, g_source_remove);
  wig_downloads_button_unwatch_window(self);
  g_clear_pointer(&self->revealer, gtk_widget_unparent);
  g_clear_object(&self->paintable);

  G_OBJECT_CLASS(wig_downloads_button_parent_class)->dispose(object);
}

static void wig_downloads_button_class_init(WigDownloadsButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_downloads_button_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-downloads-button");
}

static GtkWidget *wig_downloads_button_build_list(WigDownloadsButton *self)
{
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(content, LIST_WIDTH, -1);

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(header, "downloads-header");

  GtkWidget *title = gtk_label_new("Downloads");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_add_css_class(title, "heading");
  gtk_box_append(GTK_BOX(header), title);

  self->clear_button = gtk_button_new_with_label("Clear completed");
  gtk_widget_add_css_class(self->clear_button, "flat");
  g_signal_connect_swapped(self->clear_button, "clicked", G_CALLBACK(wig_downloads_button_clear_clicked), self);
  gtk_box_append(GTK_BOX(header), self->clear_button);

  gtk_box_append(GTK_BOX(content), header);

  /* The popover is only as tall as what it holds, up to a point; the page
   * showing the same list takes whatever room it is given. */
  self->list = wig_downloads_list_new();
  wig_downloads_list_set_max_height(WIG_DOWNLOADS_LIST(self->list), LIST_HEIGHT);
  gtk_box_append(GTK_BOX(content), self->list);

  return content;
}

static void wig_downloads_button_init(WigDownloadsButton *self)
{
  self->popover = gtk_popover_new();
  gtk_widget_add_css_class(self->popover, "downloads-popover");
  gtk_popover_set_child(GTK_POPOVER(self->popover), wig_downloads_button_build_list(self));

  /* Without a grab the window keeps its input: a press meant for a tab switches
   * that tab instead of being swallowed to dismiss the popover, and a popover
   * that shows itself for a new download never takes the focus from whatever is
   * being typed. What a grab would have done for it is done by hand, in
   * wig_downloads_button_watch_window(). */
  gtk_popover_set_autohide(GTK_POPOVER(self->popover), FALSE);

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect_swapped(motion, "motion", G_CALLBACK(wig_downloads_button_popover_used), self);
  gtk_widget_add_controller(self->popover, motion);

  GtkGesture *inside_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(inside_click), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(inside_click), GTK_PHASE_CAPTURE);
  g_signal_connect_swapped(inside_click, "pressed", G_CALLBACK(wig_downloads_button_popover_used), self);
  gtk_widget_add_controller(self->popover, GTK_EVENT_CONTROLLER(inside_click));

  self->image = gtk_image_new();
  gtk_widget_set_valign(self->image, GTK_ALIGN_CENTER);

  self->menu_button = gtk_menu_button_new();
  gtk_menu_button_set_child(GTK_MENU_BUTTON(self->menu_button), self->image);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->menu_button), self->popover);
  gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(self->menu_button), FALSE);
  gtk_widget_set_tooltip_text(self->menu_button, "Downloads");
  gtk_widget_add_css_class(self->menu_button, "toolbar-button");
  gtk_widget_set_focusable(self->menu_button, FALSE);

  self->revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->revealer), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
  gtk_revealer_set_child(GTK_REVEALER(self->revealer), self->menu_button);
  gtk_widget_set_parent(self->revealer, GTK_WIDGET(self));

  /* The paintable needs a widget to take its scale factor and animation clock
   * from, so it can only be built once the image exists. */
  self->paintable = WIG_DOWNLOADS_PAINTABLE(wig_downloads_paintable_new(self->image));
  gtk_image_set_from_paintable(GTK_IMAGE(self->image), GDK_PAINTABLE(self->paintable));

  g_signal_connect_swapped(self->popover, "show", G_CALLBACK(wig_downloads_button_popover_shown), self);
  g_signal_connect_swapped(self->popover, "closed", G_CALLBACK(wig_downloads_button_popover_closed), self);

  self->manager = wig_application_get_downloads_manager(wig_application_get());
  g_signal_connect_object(self->manager, "added", G_CALLBACK(wig_downloads_button_download_added), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->manager, "completed", G_CALLBACK(wig_downloads_button_download_completed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->manager, "changed", G_CALLBACK(wig_downloads_button_update), self, G_CONNECT_SWAPPED);

  wig_downloads_button_update(self);
}

GtkWidget *wig_downloads_button_new(void)
{
  return GTK_WIDGET(g_object_new(WIG_TYPE_DOWNLOADS_BUTTON, NULL));
}
