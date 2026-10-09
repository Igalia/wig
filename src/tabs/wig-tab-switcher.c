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

#include "wig-tab-switcher.h"

struct _WigTabSwitcher {
  GObject parent;

  GtkWindow *window;
  WigTabList *list;
  WigTabListView *view;

  GPtrArray *tabs;
  guint position;
  GtkWidget *return_focus;
  GtkWidget *highlighted;
};

G_DEFINE_FINAL_TYPE(WigTabSwitcher, wig_tab_switcher, G_TYPE_OBJECT)

static void wig_tab_switcher_unhighlight(WigTabSwitcher *self)
{
  if (!self->highlighted)
    return;

  wig_tab_widget_hide_preview(WIG_TAB_WIDGET(self->highlighted));
  gtk_widget_remove_css_class(self->highlighted, "highlighted");
  gtk_widget_set_focusable(self->highlighted, FALSE);
  g_clear_object(&self->highlighted);
}

static void wig_tab_switcher_highlight(WigTabSwitcher *self, WigTab *tab)
{
  GtkWidget *widget = NULL;
  if (self->view)
    widget = g_slist_nth_data(wig_tab_list_view_get_tab_widgets(self->view), wig_tab_list_index_of(self->list, tab));

  if (widget && gtk_widget_get_mapped(widget)) {
    gtk_widget_set_focusable(widget, TRUE);
    gtk_widget_grab_focus(widget);
    gtk_widget_add_css_class(widget, "highlighted");
    wig_tab_widget_show_preview(WIG_TAB_WIDGET(widget));
  } else {
    widget = NULL;
  }

  wig_tab_switcher_unhighlight(self);
  self->highlighted = widget ? g_object_ref(widget) : NULL;
}

static void wig_tab_switcher_return_focus(WigTabSwitcher *self)
{
  g_autoptr(GtkWidget) focus = g_steal_pointer(&self->return_focus);
  if (focus && gtk_widget_get_mapped(focus) && gtk_widget_grab_focus(focus))
    return;

  WigTab *active = wig_tab_list_get_active(self->list);
  if (active)
    gtk_widget_child_focus(wig_tab_get_widget(active), GTK_DIR_TAB_FORWARD);
}

static void wig_tab_switcher_finish(WigTabSwitcher *self, gboolean switch_tab, const char *reason)
{
  if (!self->tabs)
    return;

  g_autoptr(GPtrArray) tabs = g_steal_pointer(&self->tabs);
  WigTab *tab = g_ptr_array_index(tabs, self->position);

  wig_tab_switcher_return_focus(self);
  wig_tab_switcher_unhighlight(self);

  if (switch_tab) {
    g_debug("tab-switcher: %s, switching to tab %u", reason, wig_tab_get_id(tab));
    wig_tab_list_set_active(self->list, tab);
  } else {
    g_debug("tab-switcher: %s, staying on the active tab", reason);
  }
}

static void wig_tab_switcher_step(WigTabSwitcher *self, int offset)
{
  if (!self->tabs) {
    g_autoptr(GPtrArray) tabs = wig_tab_list_get_by_last_active(self->list);
    if (tabs->len < 2)
      return;

    self->tabs = g_steal_pointer(&tabs);
    self->position = 0;
    g_set_object(&self->return_focus, gtk_window_get_focus(self->window));
  }

  guint n_tabs = self->tabs->len;
  self->position = (self->position + n_tabs + (guint)offset) % n_tabs;

  WigTab *tab = g_ptr_array_index(self->tabs, self->position);
  g_debug("tab-switcher: highlighting tab %u (%u of %u)", wig_tab_get_id(tab), self->position, n_tabs);
  wig_tab_switcher_highlight(self, tab);
}

static gboolean wig_tab_switcher_key_pressed(WigTabSwitcher *self, guint keyval, guint keycode, GdkModifierType state,
                                             GtkEventControllerKey *keys)
{
  GdkModifierType modifiers = state & gtk_accelerator_get_default_mod_mask();
  gboolean tab_key = keyval == GDK_KEY_Tab || keyval == GDK_KEY_KP_Tab || keyval == GDK_KEY_ISO_Left_Tab;

  if (tab_key && (modifiers == GDK_CONTROL_MASK || modifiers == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
    wig_tab_switcher_step(self, (modifiers & GDK_SHIFT_MASK) ? -1 : 1);
    return GDK_EVENT_STOP;
  }

  if (!self->tabs)
    return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Escape) {
    wig_tab_switcher_finish(self, FALSE, "escape pressed");
    return GDK_EVENT_STOP;
  }

  if (!gdk_key_event_is_modifier(gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(keys))))
    wig_tab_switcher_finish(self, TRUE, "another key pressed");

  return GDK_EVENT_PROPAGATE;
}

static void wig_tab_switcher_key_released(WigTabSwitcher *self, guint keyval, guint keycode, GdkModifierType state,
                                          GtkEventControllerKey *keys)
{
  if (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R)
    wig_tab_switcher_finish(self, TRUE, "ctrl released");
}

static void wig_tab_switcher_window_active_changed(WigTabSwitcher *self, GParamSpec *pspec, GtkWindow *window)
{
  if (!gtk_window_is_active(window))
    wig_tab_switcher_finish(self, TRUE, "window lost focus");
}

static void wig_tab_switcher_active_tab_changed(WigTabSwitcher *self, GParamSpec *pspec, WigTabList *list)
{
  wig_tab_switcher_finish(self, FALSE, "active tab changed elsewhere");
}

static void wig_tab_switcher_tab_removed(WigTabSwitcher *self, WigTab *tab, guint position, WigTabList *list)
{
  wig_tab_switcher_finish(self, FALSE, "a tab was removed");
}

static void wig_tab_switcher_dispose(GObject *object)
{
  WigTabSwitcher *self = WIG_TAB_SWITCHER(object);

  g_clear_pointer(&self->tabs, g_ptr_array_unref);
  g_clear_object(&self->return_focus);
  g_clear_object(&self->highlighted);
  g_clear_object(&self->view);
  g_clear_object(&self->list);
  G_OBJECT_CLASS(wig_tab_switcher_parent_class)->dispose(object);
}

static void wig_tab_switcher_init(WigTabSwitcher *self)
{
}

static void wig_tab_switcher_class_init(WigTabSwitcherClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_switcher_dispose;
}

WigTabSwitcher *wig_tab_switcher_new(GtkWindow *window, WigTabList *list)
{
  WigTabSwitcher *self = g_object_new(WIG_TYPE_TAB_SWITCHER, NULL);
  self->window = window;
  self->list = g_object_ref(list);

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect_object(keys, "key-pressed", G_CALLBACK(wig_tab_switcher_key_pressed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(keys, "key-released", G_CALLBACK(wig_tab_switcher_key_released), self, G_CONNECT_SWAPPED);
  gtk_widget_add_controller(GTK_WIDGET(window), keys);

  g_signal_connect_object(window, "notify::is-active", G_CALLBACK(wig_tab_switcher_window_active_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(list, "notify::active-tab", G_CALLBACK(wig_tab_switcher_active_tab_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(list, "tab-removed", G_CALLBACK(wig_tab_switcher_tab_removed), self, G_CONNECT_SWAPPED);

  return self;
}

void wig_tab_switcher_set_view(WigTabSwitcher *self, WigTabListView *view)
{
  g_set_object(&self->view, view);
}
