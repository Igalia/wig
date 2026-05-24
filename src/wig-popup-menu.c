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

#include "wig-popup-menu.h"

typedef struct {
  WebKitOptionMenu *menu;
  GtkWidget *popover;
  gboolean activated;
} PopupMenuData;

static void popup_menu_data_free(PopupMenuData *data)
{
  g_object_unref(data->menu);
  g_free(data);
}

static void popup_menu_closed(GtkPopover *popover, PopupMenuData *data)
{
  if (!data->activated)
    webkit_option_menu_close(data->menu);
}

static void popup_menu_row_activated(GtkListBox *list_box, GtkListBoxRow *row, PopupMenuData *data)
{
  data->activated = TRUE;
  webkit_option_menu_activate_item(data->menu, gtk_list_box_row_get_index(row));
  gtk_popover_popdown(GTK_POPOVER(data->popover));
}

GtkWidget *wig_popup_menu_new(WebKitOptionMenu *menu, GtkWidget *parent, WebKitRectangle *rectangle)
{
  GtkWidget *list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_SINGLE);

  guint n_items = webkit_option_menu_get_n_items(menu);
  GtkListBoxRow *selected_row = NULL;

  for (guint i = 0; i < n_items; i++) {
    WebKitOptionMenuItem *item = webkit_option_menu_get_item(menu, i);
    const char *label_text = webkit_option_menu_item_get_label(item);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *child;

    if (*label_text) {
        child = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(child), 0.0f);
    } else {
        child = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_widget_set_can_target(row, FALSE);
    }

    if (webkit_option_menu_item_is_group_label(item)) {
      gtk_widget_add_css_class(child, "heading");
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
      gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    } else if (webkit_option_menu_item_is_group_child(item)) {
      gtk_widget_set_margin_start(child, 12);
    }

    if (!webkit_option_menu_item_is_enabled(item))
      gtk_widget_set_sensitive(row, FALSE);

    const char *tooltip = webkit_option_menu_item_get_tooltip(item);
    if (tooltip && *tooltip)
      gtk_widget_set_tooltip_text(row, tooltip);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), child);
    gtk_list_box_append(GTK_LIST_BOX(list_box), row);

    if (webkit_option_menu_item_is_selected(item))
      selected_row = GTK_LIST_BOX_ROW(row);
  }

  if (selected_row)
    gtk_list_box_select_row(GTK_LIST_BOX(list_box), selected_row);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 300);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list_box);

  GtkWidget *popover = gtk_popover_new();
  gtk_widget_add_css_class(popover, "menu");
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
  gtk_widget_set_halign(popover, GTK_ALIGN_START);
  gtk_popover_set_child(GTK_POPOVER(popover), scrolled);
  gtk_widget_set_parent(popover, parent);

  GdkRectangle rect = { rectangle->x, rectangle->y, rectangle->width, rectangle->height };
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

  PopupMenuData *data = g_new(PopupMenuData, 1);
  data->menu = g_object_ref(menu);
  data->popover = popover;
  g_signal_connect_data(popover, "closed",
                        G_CALLBACK(popup_menu_closed),
                        data, (GClosureNotify)popup_menu_data_free, 0);
  g_signal_connect(list_box, "row-activated", G_CALLBACK(popup_menu_row_activated), data);

  gtk_popover_popup(GTK_POPOVER(popover));

  return popover;
}
