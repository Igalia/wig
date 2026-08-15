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

#include "wig-settings-rows.h"

static const char *const search_engine_templates[] = {
  "https://duckduckgo.com/?q=%s",         "https://kagi.com/search?q=%s",
  "https://search.brave.com/search?q=%s", "https://www.startpage.com/sp/search?query=%s",
  "https://www.google.com/search?q=%s",   "https://www.bing.com/search?q=%s",
};

static const char *const search_engine_labels[] = {
  "DuckDuckGo", "Kagi", "Brave Search", "Startpage", "Google", "Bing", "Custom", NULL,
};

/* The engines above are offered by name; anything else the setting holds is
 * shown as Custom and edited as the template it is. */
#define SEARCH_ENGINE_CUSTOM G_N_ELEMENTS(search_engine_templates)

GtkWidget *wig_settings_switch_row_new(GSettings *settings, const char *key, const char *title, const char *subtitle)
{
  GtkWidget *row = adw_switch_row_new();

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  g_settings_bind(settings, key, row, "active", G_SETTINGS_BIND_DEFAULT);

  return row;
}

static void combo_value_setup(GtkSignalListItemFactory *factory, GObject *object)
{
  GtkWidget *label = gtk_label_new(NULL);

  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
  gtk_list_item_set_child(GTK_LIST_ITEM(object), label);
}

static void combo_value_bind(GtkSignalListItemFactory *factory, GObject *object)
{
  GtkListItem *item = GTK_LIST_ITEM(object);
  GtkStringObject *value = gtk_list_item_get_item(item);

  gtk_label_set_label(GTK_LABEL(gtk_list_item_get_child(item)), gtk_string_object_get_string(value));
}

/* The stock item ellipsizes at twenty characters and has no width of its own to
 * hold on to, so a row whose description asks for the space leaves the value
 * cut off. A plain label keeps its natural width, which is also what sits it
 * against the arrow rather than adrift from it. The list in the popover keeps
 * the stock item, and with it the mark against the value in use. */
static void combo_row_show_value_in_full(AdwComboRow *row)
{
  g_autoptr(GtkListItemFactory) factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(combo_value_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(combo_value_bind), NULL);

  adw_combo_row_set_list_factory(row, adw_combo_row_get_factory(row));
  adw_combo_row_set_factory(row, factory);
}

/* The row's model holds the labels, so what travels over the binding is a
 * position in it rather than anything the schema would recognise. */
static gboolean combo_get_nick(GValue *value, GVariant *variant, gpointer user_data)
{
  const char *const *nicks = user_data;
  const char *nick = g_variant_get_string(variant, NULL);

  for (guint i = 0; nicks[i]; i++) {
    if (g_str_equal(nicks[i], nick)) {
      g_value_set_uint(value, i);
      return TRUE;
    }
  }

  return FALSE;
}

static GVariant *combo_set_nick(const GValue *value, const GVariantType *type, gpointer user_data)
{
  const char *const *nicks = user_data;
  guint selected = g_value_get_uint(value);

  if (selected == GTK_INVALID_LIST_POSITION)
    return NULL;

  return g_variant_new_string(nicks[selected]);
}

/* @nicks holds the value stored for each of @labels, in the same order, ending
 * with NULL. It reaches the binding as its user data, which is why it is not
 * const all the way down. */
GtkWidget *wig_settings_combo_row_new(GSettings *settings, const char *key, const char *title, const char *subtitle,
                                      const char **nicks, const char *const *labels)
{
  GtkWidget *row = adw_combo_row_new();
  g_autoptr(GtkStringList) model = gtk_string_list_new(labels);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
  combo_row_show_value_in_full(ADW_COMBO_ROW(row));
  g_settings_bind_with_mapping(settings, key, row, "selected", G_SETTINGS_BIND_DEFAULT, combo_get_nick, combo_set_nick,
                               nicks, NULL);

  return row;
}

static gboolean search_engine_get_selected(GValue *value, GVariant *variant, gpointer user_data)
{
  const char *engine = g_variant_get_string(variant, NULL);

  for (guint i = 0; i < G_N_ELEMENTS(search_engine_templates); i++) {
    if (g_str_equal(search_engine_templates[i], engine)) {
      g_value_set_uint(value, i);
      return TRUE;
    }
  }

  g_value_set_uint(value, SEARCH_ENGINE_CUSTOM);
  return TRUE;
}

/* Picking Custom is a request to type a template, not an engine to switch to, so
 * nothing is written until the entry is applied. */
static GVariant *search_engine_set_selected(const GValue *value, const GVariantType *type, gpointer user_data)
{
  guint selected = g_value_get_uint(value);

  if (selected >= G_N_ELEMENTS(search_engine_templates))
    return NULL;

  return g_variant_new_string(search_engine_templates[selected]);
}

static gboolean search_engine_selection_is_custom(GBinding *binding, const GValue *from, GValue *to, gpointer user_data)
{
  g_value_set_boolean(to, g_value_get_uint(from) == SEARCH_ENGINE_CUSTOM);
  return TRUE;
}

static void search_engine_apply(GSettings *settings, AdwEntryRow *entry)
{
  const char *template = gtk_editable_get_text(GTK_EDITABLE(entry));

  /* Without a placeholder the terms would have nowhere to go, so the row keeps
   * the text and says so rather than storing something that cannot search. */
  if (!strstr(template, "%s")) {
    gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
    return;
  }

  gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
  g_settings_set_string(settings, "search-engine", template);
}

GtkWidget *wig_settings_search_engine_rows_add(AdwPreferencesGroup *group, GSettings *settings)
{
  GtkWidget *row = adw_combo_row_new();
  g_autoptr(GtkStringList) model = gtk_string_list_new(search_engine_labels);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Search Engine");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Used when what is typed in the address bar is not a URL.");
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
  combo_row_show_value_in_full(ADW_COMBO_ROW(row));
  g_settings_bind_with_mapping(settings, "search-engine", row, "selected", G_SETTINGS_BIND_DEFAULT,
                               search_engine_get_selected, search_engine_set_selected, NULL, NULL);
  adw_preferences_group_add(group, row);

  GtkWidget *entry = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry), "URL Template");
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(entry), TRUE);
  g_settings_bind(settings, "search-engine", entry, "text", G_SETTINGS_BIND_GET);
  g_signal_connect_object(entry, "apply", G_CALLBACK(search_engine_apply), settings, G_CONNECT_SWAPPED);
  g_object_bind_property_full(row, "selected", entry, "visible", G_BINDING_SYNC_CREATE,
                              search_engine_selection_is_custom, NULL, NULL, NULL);
  adw_preferences_group_add(group, entry);

  return row;
}
