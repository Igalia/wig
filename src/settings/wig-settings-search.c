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

#include "wig-settings-search.h"

typedef struct {
  char *title;
  char *description;
  char *pane;
  char *pane_title;
} SearchRow;

struct _WigSettingsSearch {
  GtkWidget parent;

  GtkWidget *scroller;
  GtkWidget *results;
  GtkWidget *empty;
  GtkWidget *stack;
  GPtrArray *rows; /* SearchRow, every setting there is */
};

/* The settings rows as something to search: each one is offered with the pane it
 * lives in, and what matches is listed in the pane's place. Activating a result
 * emits ::activated with the pane to go to. */
G_DEFINE_FINAL_TYPE(WigSettingsSearch, wig_settings_search, GTK_TYPE_WIDGET)

enum {
  ACTIVATED_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void search_row_free(gpointer data)
{
  SearchRow *row = data;

  g_free(row->title);
  g_free(row->description);
  g_free(row->pane);
  g_free(row->pane_title);
  g_free(row);
}

static gboolean search_row_matches(SearchRow *row, const char *terms)
{
  g_autofree char *title = row->title ? g_utf8_casefold(row->title, -1) : NULL;
  if (title && strstr(title, terms))
    return TRUE;

  if (!row->description)
    return FALSE;

  g_autofree char *description = g_utf8_casefold(row->description, -1);
  return strstr(description, terms) != NULL;
}

GtkWidget *wig_settings_search_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_SEARCH, NULL);
}

void wig_settings_search_add(WigSettingsSearch *self, const char *title, const char *description, const char *pane,
                             const char *pane_title)
{
  SearchRow *entry = g_new0(SearchRow, 1);

  entry->title = g_strdup(title);
  entry->description = description && *description ? g_strdup(description) : NULL;
  entry->pane = g_strdup(pane);
  entry->pane_title = g_strdup(pane_title);
  g_ptr_array_add(self->rows, entry);
}

static void wig_settings_search_result_activated(AdwActionRow *result, SearchRow *row)
{
  WigSettingsSearch *self = WIG_SETTINGS_SEARCH(gtk_widget_get_ancestor(GTK_WIDGET(result), WIG_TYPE_SETTINGS_SEARCH));

  g_signal_emit(self, signals[ACTIVATED_SIGNAL], 0, row->pane);
}

static void wig_settings_search_clear(WigSettingsSearch *self)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child(self->results)))
    gtk_box_remove(GTK_BOX(self->results), child);
}

/* Settings are offered pane by pane, so a run of matches from the same pane is
 * gathered under its name the way the pane itself would show them. */
void wig_settings_search_set_terms(WigSettingsSearch *self, const char *terms)
{
  g_autofree char *folded = g_utf8_casefold(terms, -1);
  AdwPreferencesGroup *group = NULL;
  const char *group_pane = NULL;
  guint matches = 0;

  wig_settings_search_clear(self);

  for (guint i = 0; i < self->rows->len; i++) {
    SearchRow *row = g_ptr_array_index(self->rows, i);
    if (!search_row_matches(row, folded))
      continue;

    if (!group || g_strcmp0(group_pane, row->pane) != 0) {
      group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
      adw_preferences_group_set_title(group, row->pane_title);
      gtk_box_append(GTK_BOX(self->results), GTK_WIDGET(group));
      group_pane = row->pane;
    }

    GtkWidget *result = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(result), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(result), row->title);
    if (row->description)
      adw_action_row_set_subtitle(ADW_ACTION_ROW(result), row->description);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(result), TRUE);
    g_signal_connect(result, "activated", G_CALLBACK(wig_settings_search_result_activated), row);
    adw_preferences_group_add(group, result);
    matches++;
  }

  gtk_stack_set_visible_child_name(GTK_STACK(self->stack), matches ? "results" : "empty");
}

static void wig_settings_search_dispose(GObject *object)
{
  WigSettingsSearch *self = WIG_SETTINGS_SEARCH(object);

  g_clear_pointer(&self->stack, gtk_widget_unparent);
  g_clear_pointer(&self->rows, g_ptr_array_unref);

  G_OBJECT_CLASS(wig_settings_search_parent_class)->dispose(object);
}

static void wig_settings_search_class_init(WigSettingsSearchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_search_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-search");

  signals[ACTIVATED_SIGNAL] = g_signal_new("activated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void wig_settings_search_init(WigSettingsSearch *self)
{
  self->rows = g_ptr_array_new_with_free_func(search_row_free);

  self->results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
  gtk_widget_set_valign(self->results, GTK_ALIGN_START);

  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), self->results);
  gtk_widget_set_margin_top(clamp, 24);
  gtk_widget_set_margin_bottom(clamp, 24);
  gtk_widget_set_margin_start(clamp, 12);
  gtk_widget_set_margin_end(clamp, 12);

  self->scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller), clamp);

  self->empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty), "edit-find-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(self->empty), "No Results Found");
  adw_status_page_set_description(ADW_STATUS_PAGE(self->empty), "Try a different search.");

  self->stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->stack), self->scroller, "results");
  gtk_stack_add_named(GTK_STACK(self->stack), self->empty, "empty");
  gtk_widget_set_parent(self->stack, GTK_WIDGET(self));
}
