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

#include "wig-settings-website-data.h"

#include "wig-application.h"
#include "wig-settings-rows.h"

#include <wpe/webkit.h>

static const struct {
  const char *key;
  const char *title;
  const char *description;
} storage_settings[] = {
  { "enable-html5-local-storage", "Local Storage", "Let a page keep localStorage between visits." },
  { "enable-html5-database", "Databases", "Let a page keep data in an indexedDB of its own." },
};

static const struct {
  WebKitWebsiteDataTypes type;
  const char *name;
} data_types[] = {
  { WEBKIT_WEBSITE_DATA_MEMORY_CACHE, "Memory Cache" },
  { WEBKIT_WEBSITE_DATA_DISK_CACHE, "Disk Cache" },
  { WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE, "Offline Application Cache" },
  { WEBKIT_WEBSITE_DATA_SESSION_STORAGE, "Session Storage" },
  { WEBKIT_WEBSITE_DATA_LOCAL_STORAGE, "Local Storage" },
  { WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES, "IndexedDB Databases" },
  { WEBKIT_WEBSITE_DATA_COOKIES, "Cookies" },
  { WEBKIT_WEBSITE_DATA_DEVICE_ID_HASH_SALT, "Device ID Hash Salt" },
  { WEBKIT_WEBSITE_DATA_HSTS_CACHE, "HSTS Cache" },
  { WEBKIT_WEBSITE_DATA_ITP, "ITP" },
  { WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS, "Service Worker Registrations" },
  { WEBKIT_WEBSITE_DATA_DOM_CACHE, "DOM Cache" },
#if HAVE_WEBSITE_DATA_FILE_SYSTEM
  { WEBKIT_WEBSITE_DATA_FILE_SYSTEM, "File System" },
#endif
};

/* An origin and what it accounts for under one type, so the list can be put in
 * order and totalled without asking for the size over and over. */
typedef struct {
  WebKitWebsiteData *data;
  guint64 size;
} Origin;

typedef struct {
  WigSettingsWebsiteData *pane;
  guint index;
  AdwExpanderRow *row;
  GtkWidget *total;
  GtkWidget *status;
  GPtrArray *entries;
  /* What the last look returned, so removing an origin has the data to name it
   * with instead of asking for all of it again. */
  GList *stored;
} TypeRow;

struct _WigSettingsWebsiteData {
  GtkWidget parent;

  WebKitWebsiteDataManager *manager;

  GtkWidget *page;
  TypeRow types[G_N_ELEMENTS(data_types)];
  gboolean populated;
};

G_DEFINE_FINAL_TYPE(WigSettingsWebsiteData, wig_settings_website_data, GTK_TYPE_WIDGET)

/* A type row lives inside the pane, so a request holds the pane to keep the row
 * it will come back to. */
typedef struct {
  WigSettingsWebsiteData *pane;
  TypeRow *type_row;
} Request;

static void website_data_look(TypeRow *type_row);

static Request *request_new(TypeRow *type_row)
{
  Request *request = g_new0(Request, 1);

  request->pane = g_object_ref(type_row->pane);
  request->type_row = type_row;

  return request;
}

static void request_free(Request *request)
{
  g_object_unref(request->pane);
  g_free(request);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Request, request_free)

static WebKitWebsiteDataTypes type_row_type(TypeRow *type_row)
{
  return data_types[type_row->index].type;
}

static const char *type_row_name(TypeRow *type_row)
{
  return data_types[type_row->index].name;
}

static void on_remove_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(Request) request = user_data;
  g_autoptr(GError) error = NULL;

  if (!webkit_website_data_manager_remove_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error))
    g_warning("website-data: remove failed: %s", error->message);

  /* Whatever is left is what the list should show, whether the removal worked or
   * not. */
  website_data_look(request->type_row);
}

static void origin_removed(GtkButton *button, TypeRow *type_row)
{
  const char *origin = gtk_widget_get_name(GTK_WIDGET(button));

  for (GList *l = type_row->stored; l; l = l->next) {
    if (!g_str_equal(webkit_website_data_get_name(l->data), origin))
      continue;

    g_autolist(WebKitWebsiteData) removing = g_list_prepend(NULL, webkit_website_data_ref(l->data));
    g_debug("website-data: removing %s for '%s'", type_row_name(type_row), origin);
    webkit_website_data_manager_remove(type_row->pane->manager, type_row_type(type_row), removing, NULL, on_remove_done,
                                       request_new(type_row));
    return;
  }
}

static GtkWidget *website_data_origin_row(TypeRow *type_row, const Origin *origin_data)
{
  const char *origin = webkit_website_data_get_name(origin_data->data);

  GtkWidget *row = adw_action_row_new();
  /* An origin is whatever the site calls itself, which is not markup. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), origin);

  /* Only the caches account for what they hold; the rest report nothing. */
  if (origin_data->size > 0) {
    g_autofree char *formatted = g_format_size(origin_data->size);
    GtkWidget *label = gtk_label_new(formatted);

    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), label);
  }

  GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  /* The button is the only thing that knows which origin its row is for. */
  gtk_widget_set_name(remove, origin);
  gtk_widget_set_tooltip_text(remove, "Remove Data");
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(remove, "flat");
  g_signal_connect(remove, "clicked", G_CALLBACK(origin_removed), type_row);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

  return row;
}

/* Biggest first, since what is worth doing something about is what is taking the
 * room. Sizes that match are put in name order so the list does not rearrange
 * itself between looks, which is what the types accounting for nothing would
 * otherwise do. */
static int compare_by_size(gconstpointer first, gconstpointer second)
{
  const Origin *left = first;
  const Origin *right = second;

  if (left->size != right->size)
    return left->size > right->size ? -1 : 1;

  return g_strcmp0(webkit_website_data_get_name(left->data), webkit_website_data_get_name(right->data));
}

/* Most of the types account for nothing at all, and saying so for each of them
 * reads as a column of noise. */
static void website_data_set_total(TypeRow *type_row, guint64 total)
{
  g_autofree char *formatted = total > 0 ? g_format_size(total) : NULL;

  gtk_label_set_label(GTK_LABEL(type_row->total), formatted ? formatted : "");
}

static void website_data_list(TypeRow *type_row)
{
  g_autoptr(GArray) origins = g_array_new(FALSE, FALSE, sizeof(Origin));
  guint64 total = 0;

  for (guint i = 0; i < type_row->entries->len; i++)
    adw_expander_row_remove(type_row->row, g_ptr_array_index(type_row->entries, i));

  g_ptr_array_set_size(type_row->entries, 0);

  for (GList *l = type_row->stored; l; l = l->next) {
    Origin origin = { l->data, webkit_website_data_get_size(l->data, type_row_type(type_row)) };

    total += origin.size;
    g_array_append_val(origins, origin);
  }

  g_array_sort(origins, compare_by_size);

  for (guint i = 0; i < origins->len; i++) {
    GtkWidget *row = website_data_origin_row(type_row, &g_array_index(origins, Origin, i));

    adw_expander_row_add_row(type_row->row, row);
    g_ptr_array_add(type_row->entries, row);
  }

  website_data_set_total(type_row, total);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(type_row->status), "Nothing stored");
  gtk_widget_set_visible(type_row->status, type_row->entries->len == 0);
}

static void on_look_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(Request) request = user_data;
  TypeRow *type_row = request->type_row;
  g_autoptr(GError) error = NULL;

  g_clear_list(&type_row->stored, (GDestroyNotify)webkit_website_data_unref);
  type_row->stored = webkit_website_data_manager_fetch_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error);

  if (error)
    g_warning("website-data: could not look at %s: %s", type_row_name(type_row), error->message);

  website_data_list(type_row);
}

static void website_data_look(TypeRow *type_row)
{
  webkit_website_data_manager_fetch(type_row->pane->manager, type_row_type(type_row), NULL, on_look_done,
                                    request_new(type_row));
}

static void on_clear_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(Request) request = user_data;
  g_autoptr(GError) error = NULL;

  if (!webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error))
    g_warning("website-data: clear failed: %s", error->message);

  website_data_look(request->type_row);
}

static void type_cleared(GtkButton *button, TypeRow *type_row)
{
  g_debug("website-data: clearing %s", type_row_name(type_row));
  webkit_website_data_manager_clear(type_row->pane->manager, type_row_type(type_row), 0, NULL, on_clear_done,
                                    request_new(type_row));
}

static void wig_settings_website_data_build_row(WigSettingsWebsiteData *self, AdwPreferencesGroup *group, guint index)
{
  TypeRow *type_row = &self->types[index];

  type_row->pane = self;
  type_row->index = index;
  type_row->entries = g_ptr_array_new();

  type_row->row = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(type_row->row), data_types[index].name);

  /* A suffix goes in ahead of the ones already there, so the button is added
   * before the size that reads to the left of it. */
  g_autofree char *tooltip = g_strdup_printf("Clear %s", data_types[index].name);
  GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
  gtk_widget_set_tooltip_text(clear, tooltip);
  gtk_widget_set_valign(clear, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(clear, "flat");
  g_signal_connect(clear, "clicked", G_CALLBACK(type_cleared), type_row);
  adw_expander_row_add_suffix(type_row->row, clear);

  /* What each type holds is next to its name, so the list can be read without
   * opening anything. */
  type_row->total = gtk_label_new(NULL);
  gtk_widget_add_css_class(type_row->total, "dim-label");
  gtk_widget_add_css_class(type_row->total, "numeric");
  gtk_widget_set_valign(type_row->total, GTK_ALIGN_CENTER);
  adw_expander_row_add_suffix(type_row->row, type_row->total);

  type_row->status = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(type_row->status), "Looking…");
  adw_expander_row_add_row(type_row->row, type_row->status);

  adw_preferences_group_add(group, GTK_WIDGET(type_row->row));
}

void wig_settings_website_data_index(WigSettingsSearch *search, const char *pane, const char *pane_title)
{
  for (guint i = 0; i < G_N_ELEMENTS(storage_settings); i++)
    wig_settings_search_add(search, storage_settings[i].title, storage_settings[i].description, pane, pane_title);

  for (guint i = 0; i < G_N_ELEMENTS(data_types); i++)
    wig_settings_search_add(search, data_types[i].name, NULL, pane, pane_title);
}

static void wig_settings_website_data_map(GtkWidget *widget)
{
  WigSettingsWebsiteData *self = WIG_SETTINGS_WEBSITE_DATA(widget);

  GTK_WIDGET_CLASS(wig_settings_website_data_parent_class)->map(widget);

  /* Every type is looked at the first time the pane is, since what each one
   * holds is on show whether or not it has been opened. */
  if (self->populated)
    return;

  self->populated = TRUE;
  for (guint i = 0; i < G_N_ELEMENTS(data_types); i++)
    website_data_look(&self->types[i]);
}

static void wig_settings_website_data_dispose(GObject *object)
{
  WigSettingsWebsiteData *self = WIG_SETTINGS_WEBSITE_DATA(object);

  g_clear_pointer(&self->page, gtk_widget_unparent);

  for (guint i = 0; i < G_N_ELEMENTS(data_types); i++) {
    g_clear_pointer(&self->types[i].entries, g_ptr_array_unref);
    g_clear_list(&self->types[i].stored, (GDestroyNotify)webkit_website_data_unref);
  }

  G_OBJECT_CLASS(wig_settings_website_data_parent_class)->dispose(object);
}

static void wig_settings_website_data_class_init(WigSettingsWebsiteDataClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_website_data_dispose;
  widget_class->map = wig_settings_website_data_map;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-website-data");
}

static void wig_settings_website_data_init(WigSettingsWebsiteData *self)
{
  WebKitNetworkSession *session = wig_application_get_network_session(wig_application_get());

  self->manager = webkit_network_session_get_website_data_manager(session);

  self->page = adw_preferences_page_new();
  gtk_widget_set_parent(self->page, GTK_WIDGET(self));

  GSettings *wig_settings = wig_application_get_settings(wig_application_get());
  AdwPreferencesGroup *storage = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

  adw_preferences_group_set_title(storage, "Storage");
  adw_preferences_group_set_description(storage, "What sites may keep here.");

  for (guint i = 0; i < G_N_ELEMENTS(storage_settings); i++)
    adw_preferences_group_add(storage,
                              wig_settings_switch_row_new(wig_settings, storage_settings[i].key,
                                                          storage_settings[i].title, storage_settings[i].description));

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), storage);

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, "Website Data");
  adw_preferences_group_set_description(group, "What sites have stored here, by the kind of storage they used.");

  for (guint i = 0; i < G_N_ELEMENTS(data_types); i++)
    wig_settings_website_data_build_row(self, group, i);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), group);
}

GtkWidget *wig_settings_website_data_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_WEBSITE_DATA, NULL);
}
