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

#include "wig-tab-list-view.h"

typedef struct {
  WigTabList *list;
  GtkBox *tab_box;
  GSList *tab_widgets;
} WigTabListViewPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(WigTabListView, wig_tab_list_view, GTK_TYPE_WIDGET)

static void wig_tab_list_view_update_active(WigTabListView *self, GParamSpec *pspec, WigTabList *list)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  WigTab *active = wig_tab_list_get_active(priv->list);
  for (GSList *l = priv->tab_widgets; l; l = g_slist_next(l)) {
    WigTabWidget *widget = WIG_TAB_WIDGET(l->data);
    if (wig_tab_widget_get_tab(widget) == active)
      gtk_widget_add_css_class(GTK_WIDGET(widget), "active");
    else
      gtk_widget_remove_css_class(GTK_WIDGET(widget), "active");
  }
}

static void wig_tab_list_view_tab_selected_changed(WigTab *tab, GParamSpec *pspec, WigTabWidget *tab_widget)
{
  if (wig_tab_get_selected(tab))
    gtk_widget_add_css_class(GTK_WIDGET(tab_widget), "selected");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(tab_widget), "selected");
}

static void wig_tab_list_view_close_requested(WigTabWidget *tab_widget, WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  wig_tab_list_close(priv->list, wig_tab_widget_get_tab(tab_widget));
}

static void wig_tab_list_view_tab_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                          WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  WigTab *tab = wig_tab_widget_get_tab(WIG_TAB_WIDGET(widget));
  if (tab)
    wig_tab_list_set_active(priv->list, tab);
}

static void wig_tab_list_view_tab_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                                WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  wig_tab_widget_show_context_menu(WIG_TAB_WIDGET(widget), priv->list);
}

static void wig_tab_list_view_tab_added(WigTabList *list, WigTab *tab, guint position, WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);

  GtkWidget *widget = wig_tab_widget_new(tab);
  WigTabWidget *tab_widget = WIG_TAB_WIDGET(widget);

  g_signal_connect_object(tab_widget, "close-requested", G_CALLBACK(wig_tab_list_view_close_requested), self,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(tab, "notify::selected", G_CALLBACK(wig_tab_list_view_tab_selected_changed), tab_widget,
                          G_CONNECT_DEFAULT);

  GtkGestureClick *gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  g_signal_connect_object(gesture, "pressed", G_CALLBACK(wig_tab_list_view_tab_pressed), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));

  GtkGestureClick *right_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_gesture), GDK_BUTTON_SECONDARY);
  g_signal_connect_object(right_gesture, "pressed", G_CALLBACK(wig_tab_list_view_tab_right_pressed), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(right_gesture));

  GtkWidget *prev_sibling = position > 0 ? GTK_WIDGET(g_slist_nth_data(priv->tab_widgets, position - 1)) : NULL;
  priv->tab_widgets = g_slist_insert(priv->tab_widgets, tab_widget, (gint)position);
  gtk_box_insert_child_after(priv->tab_box, widget, prev_sibling);

  if (wig_tab_widget_get_tab(tab_widget) == wig_tab_list_get_active(priv->list))
    gtk_widget_add_css_class(widget, "active");

  WigTabListViewClass *klass = WIG_TAB_LIST_VIEW_GET_CLASS(self);
  if (klass->tab_widget_added)
    klass->tab_widget_added(self, tab_widget, position);
}

static void wig_tab_list_view_tab_removed(WigTabList *list, WigTab *tab, guint position, WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  GSList *link = g_slist_nth(priv->tab_widgets, position);
  WigTabWidget *tab_widget = WIG_TAB_WIDGET(link->data);
  priv->tab_widgets = g_slist_delete_link(priv->tab_widgets, link);
  gtk_box_remove(priv->tab_box, GTK_WIDGET(tab_widget));
}

static void wig_tab_list_view_dispose(GObject *object)
{
  WigTabListView *self = WIG_TAB_LIST_VIEW(object);
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  g_clear_pointer(&priv->tab_widgets, g_slist_free);
  g_clear_object(&priv->list);
  G_OBJECT_CLASS(wig_tab_list_view_parent_class)->dispose(object);
}

static void wig_tab_list_view_init(WigTabListView *self)
{
}

static void wig_tab_list_view_class_init(WigTabListViewClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_list_view_dispose;
}

void wig_tab_list_view_setup(WigTabListView *self, WigTabList *list, GtkBox *tab_box)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);

  priv->list = g_object_ref(list);
  priv->tab_box = tab_box;

  g_signal_connect_object(list, "tab-added", G_CALLBACK(wig_tab_list_view_tab_added), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(list, "tab-removed", G_CALLBACK(wig_tab_list_view_tab_removed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(list, "notify::active-tab", G_CALLBACK(wig_tab_list_view_update_active), self,
                          G_CONNECT_SWAPPED);

  guint n = wig_tab_list_get_n_tabs(list);
  for (guint i = 0; i < n; i++)
    wig_tab_list_view_tab_added(list, wig_tab_list_get_nth(list, i), i, self);
}

WigTabList *wig_tab_list_view_get_list(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  return priv->list;
}

GtkBox *wig_tab_list_view_get_tab_box(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  return priv->tab_box;
}

GSList *wig_tab_list_view_get_tab_widgets(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  return priv->tab_widgets;
}
