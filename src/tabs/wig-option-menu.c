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

#include "wig-option-menu.h"

#define WIG_OPTION_MENU_MAX_HEIGHT 400

struct _WigOptionMenu {
  GtkPopover parent;

  WebKitOptionMenu *menu;
  GtkWidget *list_box;
  int selected_index;
  gboolean answered;
};

G_DEFINE_FINAL_TYPE(WigOptionMenu, wig_option_menu, GTK_TYPE_POPOVER)

/* WebKit has to be told exactly once how the menu ended, and it keeps the
 * element's value when told nothing was picked. Closing is expected even after
 * activating an item: by then WebKit has dropped the menu and ignores it. */
static void wig_option_menu_answer(WigOptionMenu *self, int index)
{
  if (self->answered)
    return;

  self->answered = TRUE;

  if (index >= 0)
    webkit_option_menu_activate_item(self->menu, (guint)index);
  webkit_option_menu_close(self->menu);
}

static void wig_option_menu_row_activated(WigOptionMenu *self, GtkListBoxRow *row)
{
  wig_option_menu_answer(self, gtk_list_box_row_get_index(row));
  gtk_popover_popdown(GTK_POPOVER(self));
}

/* The page can take the menu away on its own, by navigating or by dropping the
 * element the menu belongs to. */
static void wig_option_menu_close_requested(WigOptionMenu *self)
{
  self->answered = TRUE;
  gtk_popover_popdown(GTK_POPOVER(self));
}

static void wig_option_menu_closed(GtkPopover *popover)
{
  wig_option_menu_answer(WIG_OPTION_MENU(popover), -1);

  /* GtkPopover leaves the class handler for its own signal unset. */
  GtkPopoverClass *popover_class = GTK_POPOVER_CLASS(wig_option_menu_parent_class);
  if (popover_class->closed)
    popover_class->closed(popover);
}

/* Focus follows the element's current value so the keyboard starts where the
 * page is, which also scrolls a long list to that item. */
static void wig_option_menu_map(GtkWidget *widget)
{
  WigOptionMenu *self = WIG_OPTION_MENU(widget);

  GTK_WIDGET_CLASS(wig_option_menu_parent_class)->map(widget);

  if (self->selected_index < 0)
    return;

  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->list_box), self->selected_index);
  if (row)
    gtk_widget_grab_focus(GTK_WIDGET(row));
}

static void wig_option_menu_dispose(GObject *object)
{
  WigOptionMenu *self = WIG_OPTION_MENU(object);

  g_clear_object(&self->menu);

  G_OBJECT_CLASS(wig_option_menu_parent_class)->dispose(object);
}

static void wig_option_menu_class_init(WigOptionMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GtkPopoverClass *popover_class = GTK_POPOVER_CLASS(klass);

  object_class->dispose = wig_option_menu_dispose;
  widget_class->map = wig_option_menu_map;
  popover_class->closed = wig_option_menu_closed;
}

static void wig_option_menu_init(WigOptionMenu *self)
{
  self->selected_index = -1;

  self->list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list_box), GTK_SELECTION_SINGLE);
  g_signal_connect_object(self->list_box, "row-activated", G_CALLBACK(wig_option_menu_row_activated), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), WIG_OPTION_MENU_MAX_HEIGHT);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), self->list_box);

  gtk_widget_add_css_class(GTK_WIDGET(self), "menu");
  gtk_popover_set_has_arrow(GTK_POPOVER(self), FALSE);
  gtk_popover_set_position(GTK_POPOVER(self), GTK_POS_BOTTOM);
  gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_START);
  gtk_popover_set_child(GTK_POPOVER(self), scrolled);
}

/* An <optgroup> arrives as a label item followed by its options, and a separator
 * as an item with no label at all, so both are rows that cannot be picked. */
static GtkWidget *wig_option_menu_build_row(WebKitOptionMenuItem *item)
{
  GtkWidget *row = gtk_list_box_row_new();
  const char *label = webkit_option_menu_item_get_label(item);
  GtkWidget *child;

  if (label && *label) {
    child = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(child), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_END);
  } else {
    child = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_can_target(row, FALSE);
  }

  if (webkit_option_menu_item_is_group_label(item))
    gtk_widget_add_css_class(child, "heading");
  else if (webkit_option_menu_item_is_group_child(item))
    gtk_widget_set_margin_start(child, 12);

  gboolean selectable = (label && *label) && !webkit_option_menu_item_is_group_label(item);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), selectable);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), selectable);

  if (!webkit_option_menu_item_is_enabled(item))
    gtk_widget_set_sensitive(row, FALSE);

  const char *tooltip = webkit_option_menu_item_get_tooltip(item);
  if (tooltip && *tooltip)
    gtk_widget_set_tooltip_text(row, tooltip);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), child);

  return row;
}

/* Shows the menu for a <select> element over `parent`, which is the widget the
 * page is drawn into: `rectangle` is the element's area within it. The popover
 * is parented to `parent` and stays until it is unparented, so the caller keeps
 * it and drops it when it closes. */
GtkWidget *wig_option_menu_show(GtkWidget *parent, WebKitOptionMenu *menu, WebKitRectangle *rectangle)
{
  WigOptionMenu *self = WIG_OPTION_MENU(g_object_new(WIG_TYPE_OPTION_MENU, NULL));

  self->menu = g_object_ref(menu);
  g_signal_connect_object(menu, "close", G_CALLBACK(wig_option_menu_close_requested), self, G_CONNECT_SWAPPED);

  guint n_items = webkit_option_menu_get_n_items(menu);
  for (guint i = 0; i < n_items; i++) {
    WebKitOptionMenuItem *item = webkit_option_menu_get_item(menu, i);
    GtkWidget *row = wig_option_menu_build_row(item);

    gtk_list_box_append(GTK_LIST_BOX(self->list_box), row);

    if (webkit_option_menu_item_is_selected(item)) {
      self->selected_index = (int)i;
      gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), GTK_LIST_BOX_ROW(row));
    }
  }

  /* A menu narrower than the element it drops out of looks detached from it. */
  gtk_widget_set_size_request(GTK_WIDGET(self), rectangle->width, -1);

  GdkRectangle rect = { rectangle->x, rectangle->y, rectangle->width, rectangle->height };
  gtk_widget_set_parent(GTK_WIDGET(self), parent);
  gtk_popover_set_pointing_to(GTK_POPOVER(self), &rect);
  gtk_popover_popup(GTK_POPOVER(self));

  return GTK_WIDGET(self);
}
