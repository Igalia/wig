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

#include "wig-tab-sidebar.h"

#include "wig-tab-wrap-layout.h"

struct _WigTabSidebar {
  WigTabListView parent;

  GtkWidget *pinned_box;
  GtkWidget *separator;
  GtkWidget *scrolled_window;
  GtkWidget *new_tab_button;
};

G_DEFINE_FINAL_TYPE(WigTabSidebar, wig_tab_sidebar, WIG_TYPE_TAB_LIST_VIEW)

static void wig_tab_sidebar_new_tab_clicked(GtkButton *button, WigTabSidebar *self)
{
  WigTab *tab = NULL;
  g_signal_emit_by_name(wig_tab_list_view_get_list(WIG_TAB_LIST_VIEW(self)), "create-tab", &tab);
}

static void wig_tab_sidebar_dispose(GObject *object)
{
  WigTabSidebar *self = WIG_TAB_SIDEBAR(object);
  // FIXME: Why do we need to manually unparent?
  g_clear_pointer(&self->new_tab_button, gtk_widget_unparent);
  g_clear_pointer(&self->scrolled_window, gtk_widget_unparent);
  g_clear_pointer(&self->separator, gtk_widget_unparent);
  g_clear_pointer(&self->pinned_box, gtk_widget_unparent);
  G_OBJECT_CLASS(wig_tab_sidebar_parent_class)->dispose(object);
}

static void wig_tab_sidebar_init(WigTabSidebar *self)
{
}

static void wig_tab_sidebar_class_init(WigTabSidebarClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_sidebar_dispose;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-tab-sidebar");
}

GtkWidget *wig_tab_sidebar_new(WigTabList *list)
{
  g_return_val_if_fail(WIG_IS_TAB_LIST(list), NULL);

  WigTabSidebar *self = WIG_TAB_SIDEBAR(g_object_new(WIG_TYPE_TAB_SIDEBAR, NULL));

  gtk_orientable_set_orientation(GTK_ORIENTABLE(gtk_widget_get_layout_manager(GTK_WIDGET(self))),
                                 GTK_ORIENTATION_VERTICAL);

  GtkBox *tab_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  /* Pinned tabs are favicon-sized, so they sit in rows above the tab list
   * rather than taking a full-width row each. */
  self->pinned_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_layout_manager(self->pinned_box, wig_tab_wrap_layout_new());
  gtk_widget_add_css_class(self->pinned_box, "pinned-tabs");
  gtk_widget_set_parent(self->pinned_box, GTK_WIDGET(self));

  self->separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->separator, "pinned-separator");
  gtk_widget_set_parent(self->separator, GTK_WIDGET(self));

  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(self->scrolled_window), TRUE);
  gtk_widget_set_vexpand(self->scrolled_window, FALSE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window), GTK_WIDGET(tab_box));
  gtk_widget_set_parent(self->scrolled_window, GTK_WIDGET(self));

  self->new_tab_button = gtk_button_new();
  gtk_widget_add_css_class(self->new_tab_button, "flat");
  GtkWidget *new_tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(new_tab_box), gtk_image_new_from_icon_name("tab-new-symbolic"));
  GtkWidget *new_tab_label = gtk_label_new("New Tab");
  gtk_widget_add_css_class(new_tab_label, "dim-label");
  gtk_box_append(GTK_BOX(new_tab_box), new_tab_label);
  gtk_button_set_child(GTK_BUTTON(self->new_tab_button), new_tab_box);
  gtk_widget_set_hexpand(self->new_tab_button, TRUE);
  gtk_widget_set_halign(self->new_tab_button, GTK_ALIGN_FILL);
  g_signal_connect_object(self->new_tab_button, "clicked", G_CALLBACK(wig_tab_sidebar_new_tab_clicked), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_set_parent(self->new_tab_button, GTK_WIDGET(self));

  wig_tab_list_view_setup(WIG_TAB_LIST_VIEW(self), list, GTK_BOX(self->pinned_box), self->separator, tab_box,
                          GTK_ORIENTATION_VERTICAL);

  return GTK_WIDGET(self);
}
