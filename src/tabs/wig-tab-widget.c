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

#include "wig-tab-widget.h"

#include "wig-tab-context-menu.h"

/* Below this width there is only room for the favicon, so the title and close
 * button are hidden and the icon is centred. */
// FIXME: Scale aware.
#define WIG_TAB_COMPACT_WIDTH 80

struct _WigTabWidget {
  GtkWidget parent;

  WigTab *tab;

  GtkWidget *favicon;
  GtkWidget *spinner;
  GtkWidget *title_label;
  GtkWidget *close_button;
  GtkWidget *context_menu_popover;

  int target_width;
};

G_DEFINE_FINAL_TYPE(WigTabWidget, wig_tab_widget, GTK_TYPE_WIDGET)

enum { SIGNAL_CLOSE_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void wig_tab_widget_on_icon_changed(WigTabWidget *self, GParamSpec *pspec, WigTab *tab)
{
  if (self->spinner)
    return;

  GIcon *icon = wig_tab_get_icon(tab);
  if (icon) {
    if (!self->favicon) {
      self->favicon = gtk_image_new();
      gtk_image_set_pixel_size(GTK_IMAGE(self->favicon), WIG_TAB_FAVICON_SIZE);
      gtk_widget_set_halign(self->favicon, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand(self->favicon, FALSE);
      gtk_widget_insert_before(self->favicon, GTK_WIDGET(self), self->title_label);
    }
    gtk_image_set_from_gicon(GTK_IMAGE(self->favicon), icon);
  } else {
    g_clear_pointer(&self->favicon, gtk_widget_unparent);
  }
}

static void wig_tab_widget_on_loading_changed(WigTabWidget *self, GParamSpec *pspec, WigTab *tab)
{
  gboolean loading = wig_tab_get_loading(tab);

  gboolean compact = self->target_width >= 0 && self->target_width < WIG_TAB_COMPACT_WIDTH;

  if (loading) {
    g_clear_pointer(&self->favicon, gtk_widget_unparent);
    if (!self->spinner) {
      self->spinner = gtk_spinner_new();
      gtk_widget_set_size_request(self->spinner, WIG_TAB_FAVICON_SIZE, WIG_TAB_FAVICON_SIZE);
      gtk_widget_set_halign(self->spinner, compact ? GTK_ALIGN_CENTER : GTK_ALIGN_START);
      gtk_widget_set_hexpand(self->spinner, compact);
      gtk_widget_insert_before(self->spinner, GTK_WIDGET(self), self->title_label);
    }
    gtk_spinner_set_spinning(GTK_SPINNER(self->spinner), TRUE);
  } else {
    if (self->spinner) {
      gtk_spinner_set_spinning(GTK_SPINNER(self->spinner), FALSE);
      g_clear_pointer(&self->spinner, gtk_widget_unparent);
    }
    wig_tab_widget_on_icon_changed(self, NULL, self->tab);
  }
}

static void wig_tab_widget_dispose(GObject *object)
{
  WigTabWidget *self = WIG_TAB_WIDGET(object);
  g_clear_object(&self->tab);

  g_clear_pointer(&self->close_button, gtk_widget_unparent);
  g_clear_pointer(&self->title_label, gtk_widget_unparent);
  g_clear_pointer(&self->spinner, gtk_widget_unparent);
  g_clear_pointer(&self->favicon, gtk_widget_unparent);
  g_clear_pointer(&self->context_menu_popover, gtk_widget_unparent);
  G_OBJECT_CLASS(wig_tab_widget_parent_class)->dispose(object);
}

static void wig_tab_widget_close_clicked(GtkButton *button, WigTabWidget *self)
{
  g_signal_emit(self, signals[SIGNAL_CLOSE_REQUESTED], 0);
}

static void wig_tab_widget_init(WigTabWidget *self)
{
  self->target_width = -1;

  GtkLayoutManager *layout = gtk_widget_get_layout_manager(GTK_WIDGET(self));
  gtk_box_layout_set_spacing(GTK_BOX_LAYOUT(layout), 6);

  self->title_label = gtk_label_new("New Tab");
  // FIXME: We don't want to render ellisize. Instead the text should fade out at the end.
  gtk_label_set_ellipsize(GTK_LABEL(self->title_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(self->title_label), 0.0f);
  gtk_widget_set_hexpand(self->title_label, TRUE);
  gtk_widget_set_parent(self->title_label, GTK_WIDGET(self));

  self->close_button = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(self->close_button, "flat");
  gtk_widget_add_css_class(self->close_button, "circular");
  gtk_widget_set_parent(self->close_button, GTK_WIDGET(self));
  g_signal_connect(self->close_button, "clicked", G_CALLBACK(wig_tab_widget_close_clicked), self);
}

static void wig_tab_widget_class_init(WigTabWidgetClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_widget_dispose;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-tab");

  signals[SIGNAL_CLOSE_REQUESTED] = g_signal_new("close-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                                 NULL, NULL, NULL, G_TYPE_NONE, 0);
}

void wig_tab_widget_show_context_menu(WigTabWidget *self, WigTabList *list)
{
  g_return_if_fail(WIG_IS_TAB_WIDGET(self));
  g_return_if_fail(WIG_IS_TAB_LIST(list));

  g_clear_pointer(&self->context_menu_popover, gtk_widget_unparent);
  self->context_menu_popover = wig_tab_context_menu_popup(list, self->tab);
  gtk_widget_set_parent(self->context_menu_popover, GTK_WIDGET(self));
  gtk_popover_popup(GTK_POPOVER(self->context_menu_popover));
}

GtkWidget *wig_tab_widget_new(WigTab *tab)
{
  WigTabWidget *self = WIG_TAB_WIDGET(g_object_new(WIG_TYPE_TAB_WIDGET, NULL));

  self->tab = g_object_ref(tab);
  g_object_bind_property(G_OBJECT(tab), "title", self->title_label, "label", G_BINDING_SYNC_CREATE);
  g_signal_connect_object(tab, "notify::icon", G_CALLBACK(wig_tab_widget_on_icon_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(tab, "notify::loading", G_CALLBACK(wig_tab_widget_on_loading_changed), self,
                          G_CONNECT_SWAPPED);
  wig_tab_widget_on_loading_changed(self, NULL, tab);

  return GTK_WIDGET(self);
}

WigTab *wig_tab_widget_get_tab(WigTabWidget *self)
{
  return self->tab;
}

void wig_tab_widget_set_width(WigTabWidget *self, int width)
{
  if (self->target_width == width)
    return;
  self->target_width = width;
  gtk_widget_set_size_request(GTK_WIDGET(self), width, -1);

  gboolean compact = width >= 0 && width < WIG_TAB_COMPACT_WIDTH;
  gtk_widget_set_visible(self->close_button, !compact);
  gtk_widget_set_visible(self->title_label, !compact);
  if (self->favicon)
    gtk_widget_set_hexpand(self->favicon, compact);
  if (self->spinner) {
    gtk_widget_set_hexpand(self->spinner, compact);
    gtk_widget_set_halign(self->spinner, compact ? GTK_ALIGN_CENTER : GTK_ALIGN_START);
  }
}
