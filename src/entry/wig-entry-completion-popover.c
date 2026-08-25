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

#include "wig-entry-completion-popover.h"

#include "wig-utils.h"

struct _WigEntryCompletionItem {
  char *title;
  char *url;
  char *subtitle;
  char *entry_text;
};

WigEntryCompletionItem *wig_entry_completion_item_new(const char *title, const char *url, const char *subtitle,
                                                      const char *entry_text)
{
  WigEntryCompletionItem *item = g_new0(WigEntryCompletionItem, 1);
  item->title = g_strdup(title);
  item->url = g_strdup(url);
  item->subtitle = g_strdup(subtitle);
  item->entry_text = g_strdup(entry_text);
  return item;
}

void wig_entry_completion_item_free(WigEntryCompletionItem *item)
{
  g_free(item->title);
  g_free(item->url);
  g_free(item->subtitle);
  g_free(item->entry_text);
  g_free(item);
}

static WigEntryCompletionItem *entry_completion_item_copy(WigEntryCompletionItem *item)
{
  return wig_entry_completion_item_new(item->title, item->url, item->subtitle, item->entry_text);
}

struct _WigEntryCompletionPopover {
  GtkPopover parent;

  GtkWidget *list;
  GPtrArray *items;
};

G_DEFINE_FINAL_TYPE(WigEntryCompletionPopover, wig_entry_completion_popover, GTK_TYPE_POPOVER)

enum {
  SIGNAL_ACTIVATE,
  SIGNAL_SELECTED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void wig_entry_completion_popover_clear_rows(WigEntryCompletionPopover *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->list)))
    gtk_list_box_remove(GTK_LIST_BOX(self->list), child);
}

static GtkWidget *create_entry_completion_row(WigEntryCompletionItem *item)
{
  GtkWidget *row = gtk_list_box_row_new();
  gtk_widget_add_css_class(row, "entry-completion-row");
  gtk_widget_set_focusable(row, FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), TRUE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_top(box, 7);
  gtk_widget_set_margin_bottom(box, 7);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);

  GtkWidget *title_label = gtk_label_new(item->title);
  gtk_widget_add_css_class(title_label, "entry-completion-title");
  gtk_widget_set_hexpand(title_label, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(title_label), 1);
  gtk_box_append(GTK_BOX(box), title_label);

  if (item->subtitle && *item->subtitle) {
    GtkWidget *url_label = gtk_label_new(item->subtitle);
    gtk_widget_add_css_class(url_label, "entry-completion-url");
    gtk_widget_set_hexpand(url_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(url_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(url_label), 1);
    gtk_box_append(GTK_BOX(box), url_label);
  }

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  return row;
}

static void wig_entry_completion_popover_row_activated(GtkListBox *list, GtkListBoxRow *row,
                                                       WigEntryCompletionPopover *self)
{
  int index = gtk_list_box_row_get_index(row);
  if (index < 0 || (guint)index >= self->items->len)
    return;

  WigEntryCompletionItem *item = g_ptr_array_index(self->items, (guint)index);
  g_signal_emit(self, signals[SIGNAL_ACTIVATE], 0, item->url);
}

static void wig_entry_completion_popover_selected_rows_changed(GtkListBox *list, WigEntryCompletionPopover *self)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row(list);
  if (!row)
    return;

  int index = gtk_list_box_row_get_index(row);
  if (index < 0 || (guint)index >= self->items->len)
    return;

  WigEntryCompletionItem *item = g_ptr_array_index(self->items, (guint)index);
  g_signal_emit(self, signals[SIGNAL_SELECTED], 0, item->entry_text);
}

static void wig_entry_completion_popover_dispose(GObject *object)
{
  WigEntryCompletionPopover *self = WIG_ENTRY_COMPLETION_POPOVER(object);

  g_clear_pointer(&self->items, g_ptr_array_unref);

  G_OBJECT_CLASS(wig_entry_completion_popover_parent_class)->dispose(object);
}

static void wig_entry_completion_popover_init(WigEntryCompletionPopover *self)
{
  self->items = g_ptr_array_new_with_free_func((GDestroyNotify)wig_entry_completion_item_free);

  gtk_popover_set_has_arrow(GTK_POPOVER(self), FALSE);
  gtk_popover_set_position(GTK_POPOVER(self), GTK_POS_BOTTOM);
  gtk_popover_set_autohide(GTK_POPOVER(self), FALSE);
  gtk_widget_set_focusable(GTK_WIDGET(self), FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(self), "entry-completion-popover");

  self->list = gtk_list_box_new();
  gtk_widget_set_focusable(self->list, FALSE);
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list), GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(self->list, "entry-completion-list");
  g_signal_connect_object(self->list, "row-activated", G_CALLBACK(wig_entry_completion_popover_row_activated), self,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(self->list, "selected-rows-changed",
                          G_CALLBACK(wig_entry_completion_popover_selected_rows_changed), self, G_CONNECT_DEFAULT);

  gtk_popover_set_child(GTK_POPOVER(self), self->list);
}

static void wig_entry_completion_popover_class_init(WigEntryCompletionPopoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_entry_completion_popover_dispose;

  signals[SIGNAL_ACTIVATE] = g_signal_new("activate", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_SELECTED] = g_signal_new("selected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 1, G_TYPE_STRING);
}

GtkWidget *wig_entry_completion_popover_new(void)
{
  return g_object_new(WIG_TYPE_ENTRY_COMPLETION_POPOVER, NULL);
}

void wig_entry_completion_popover_set_width(WigEntryCompletionPopover *self, int width)
{
  gtk_widget_set_size_request(self->list, width, -1);
}

void wig_entry_completion_popover_set_items(WigEntryCompletionPopover *self, const char *entry_text,
                                            const char *search_engine, GPtrArray *items)
{
  g_assert(entry_text != NULL);
  g_assert(items != NULL);

  wig_entry_completion_popover_clear_rows(self);
  g_ptr_array_set_size(self->items, 0);

  WigUtilUriCompletionType completion_type = wig_util_get_uri_completion_type(entry_text);
  g_autofree char *completion_url = wig_util_complete_uri(entry_text, search_engine);
  gboolean is_navigable = completion_type != WIG_UTIL_URI_COMPLETION_SEARCH;
  g_autofree char *first_title = is_navigable ? g_strdup_printf("Navigate to %s", entry_text)
                                              : g_strdup_printf("%s - Search the Web", entry_text);
  const char *first_subtitle = is_navigable ? NULL : completion_url;
  WigEntryCompletionItem *first_item = wig_entry_completion_item_new(first_title, completion_url, first_subtitle,
                                                                     entry_text);
  g_ptr_array_add(self->items, first_item);
  gtk_list_box_append(GTK_LIST_BOX(self->list), create_entry_completion_row(first_item));

  for (guint i = 0; i < items->len; i++) {
    WigEntryCompletionItem *item = entry_completion_item_copy(g_ptr_array_index(items, i));
    g_ptr_array_add(self->items, item);
    gtk_list_box_append(GTK_LIST_BOX(self->list), create_entry_completion_row(item));
  }
}

static gboolean wig_entry_completion_popover_select_index(WigEntryCompletionPopover *self, guint index)
{
  if (index >= self->items->len)
    return FALSE;

  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->list), (int)index);
  if (!row)
    return FALSE;

  gtk_list_box_select_row(GTK_LIST_BOX(self->list), row);
  return TRUE;
}

gboolean wig_entry_completion_popover_select_next(WigEntryCompletionPopover *self)
{
  if (self->items->len == 0)
    return FALSE;

  GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list));
  if (!row)
    return wig_entry_completion_popover_select_index(self, 0);

  int index = gtk_list_box_row_get_index(row);
  if (index < 0)
    return wig_entry_completion_popover_select_index(self, 0);

  return wig_entry_completion_popover_select_index(self, MIN((guint)index + 1, self->items->len - 1));
}

gboolean wig_entry_completion_popover_select_previous(WigEntryCompletionPopover *self)
{
  if (self->items->len == 0)
    return FALSE;

  GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list));
  if (!row)
    return wig_entry_completion_popover_select_index(self, self->items->len - 1);

  int index = gtk_list_box_row_get_index(row);
  if (index <= 0)
    return wig_entry_completion_popover_select_index(self, 0);

  return wig_entry_completion_popover_select_index(self, (guint)index - 1);
}

guint wig_entry_completion_popover_get_n_items(WigEntryCompletionPopover *self)
{
  return self->items->len;
}
