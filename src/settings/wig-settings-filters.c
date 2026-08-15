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

#include "wig-settings-filters.h"

#include "wig-application.h"

struct _WigSettingsFilters {
  GtkWidget parent;

  WebKitUserContentManager *manager;
  WebKitUserContentFilterStore *store;

  GtkWidget *page;
  AdwPreferencesGroup *installed;
  GtkWidget *empty_row;
  GPtrArray *rows; /* the rows listing filters, so they can be taken away again */

  GtkWidget *name;
  GtkWidget *rules;
  GtkWidget *error;
  GtkWidget *status;
  GtkWidget *spinner;
  GtkWidget *open;
  GtkWidget *add;

  gboolean populated;
};

G_DEFINE_FINAL_TYPE(WigSettingsFilters, wig_settings_filters, GTK_TYPE_WIDGET)

static void wig_settings_filters_refresh(WigSettingsFilters *self);

static void wig_settings_filters_report(WigSettingsFilters *self, const char *message)
{
  gtk_label_set_label(GTK_LABEL(self->error), message ? message : "");
  gtk_widget_set_visible(self->error, message != NULL);
}

/* Compiling a rule list is not instant and happens away from here, so the form
 * says what it is waiting on and stops taking anything else meanwhile. */
static void wig_settings_filters_set_busy(WigSettingsFilters *self, const char *message)
{
  gtk_label_set_label(GTK_LABEL(self->status), message ? message : "");
  gtk_widget_set_visible(self->status, message != NULL);
  gtk_widget_set_visible(self->spinner, message != NULL);
  gtk_widget_set_sensitive(self->add, message == NULL);
  gtk_widget_set_sensitive(self->open, message == NULL);
}

/* Every one of these ends the same way: whatever the store now holds is what the
 * list should show. */
static void on_remove_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigSettingsFilters) self = user_data;
  g_autoptr(GError) error = NULL;

  if (!webkit_user_content_filter_store_remove_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error))
    g_warning("content-filters: remove failed: %s", error->message);

  wig_settings_filters_refresh(self);
}

static void filter_removed(GtkButton *button, WigSettingsFilters *self)
{
  const char *identifier = gtk_widget_get_name(GTK_WIDGET(button));

  g_debug("content-filters: removing '%s'", identifier);
  webkit_user_content_manager_remove_filter_by_id(self->manager, identifier);
  webkit_user_content_filter_store_remove(self->store, identifier, NULL, on_remove_done, g_object_ref(self));
}

static void wig_settings_filters_clear_list(WigSettingsFilters *self)
{
  for (guint i = 0; i < self->rows->len; i++)
    adw_preferences_group_remove(self->installed, g_ptr_array_index(self->rows, i));

  g_ptr_array_set_size(self->rows, 0);
}

static void wig_settings_filters_list(WigSettingsFilters *self, char **identifiers)
{
  wig_settings_filters_clear_list(self);

  for (char **id = identifiers; id && *id; id++) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), *id);

    GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    /* The button is the only thing that knows which filter its row is for. */
    gtk_widget_set_name(remove, *id);
    gtk_widget_set_tooltip_text(remove, "Remove Filter");
    gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(remove, "flat");
    g_signal_connect_object(remove, "clicked", G_CALLBACK(filter_removed), self, G_CONNECT_DEFAULT);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

    adw_preferences_group_add(self->installed, row);
    g_ptr_array_add(self->rows, row);
  }

  gtk_widget_set_visible(self->empty_row, self->rows->len == 0);
}

static void on_fetch_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigSettingsFilters) self = user_data;
  g_auto(GStrv) identifiers = webkit_user_content_filter_store_fetch_identifiers_finish(
      WEBKIT_USER_CONTENT_FILTER_STORE(source), result);

  wig_settings_filters_list(self, identifiers);
}

static void wig_settings_filters_refresh(WigSettingsFilters *self)
{
  webkit_user_content_filter_store_fetch_identifiers(self->store, NULL, on_fetch_done, g_object_ref(self));
}

static void on_save_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigSettingsFilters) self = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(WebKitUserContentFilter) filter = webkit_user_content_filter_store_save_finish(
      WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);

  wig_settings_filters_set_busy(self, NULL);

  if (!filter) {
    g_warning("content-filters: failed to compile: %s", error->message);
    wig_settings_filters_report(self, error->message);
    return;
  }

  webkit_user_content_manager_add_filter(self->manager, filter);
  gtk_editable_set_text(GTK_EDITABLE(self->name), "");
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->rules)), "", -1);
  wig_settings_filters_refresh(self);
}

static void filter_added(GtkButton *button, WigSettingsFilters *self)
{
  const char *identifier = gtk_editable_get_text(GTK_EDITABLE(self->name));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->rules));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *source = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

  if (!*identifier || !*source) {
    wig_settings_filters_report(self, "A filter needs a name and its rules.");
    return;
  }

  wig_settings_filters_report(self, NULL);

  g_autofree char *compiling = g_strdup_printf("Compiling “%s”…", identifier);
  wig_settings_filters_set_busy(self, compiling);

  g_autoptr(GBytes) bytes = g_bytes_new(source, strlen(source));
  webkit_user_content_filter_store_save(self->store, identifier, bytes, NULL, on_save_done, g_object_ref(self));
}

static void on_file_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigSettingsFilters) self = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;

  wig_settings_filters_set_busy(self, NULL);

  if (!g_file_load_contents_finish(G_FILE(source), result, &contents, &length, NULL, &error)) {
    g_warning("content-filters: could not read the file: %s", error->message);
    wig_settings_filters_report(self, error->message);
    return;
  }

  g_autofree char *basename = g_file_get_basename(G_FILE(source));
  if (g_str_has_suffix(basename, ".json"))
    basename[strlen(basename) - strlen(".json")] = '\0';

  gtk_editable_set_text(GTK_EDITABLE(self->name), basename);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->rules)), contents, (int)length);
  wig_settings_filters_report(self, NULL);
}

static void on_file_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigSettingsFilters) self = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);

  if (!file) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
      g_warning("content-filters: could not open the file: %s", error->message);
    return;
  }

  g_autofree char *basename = g_file_get_basename(file);
  g_autofree char *reading = g_strdup_printf("Reading “%s”…", basename);
  wig_settings_filters_set_busy(self, reading);

  g_file_load_contents_async(file, NULL, on_file_read, g_object_ref(self));
}

static void filter_file_opened(GtkButton *button, WigSettingsFilters *self)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();

  gtk_file_filter_set_name(filter, "Content Filters");
  gtk_file_filter_add_suffix(filter, "json");

  g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_title(dialog, "Open Content Filter");

  gtk_file_dialog_open(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))), NULL, on_file_chosen,
                       g_object_ref(self));
}

/* Anything wig will not open is left to whatever the desktop would do with it. */
static gboolean filter_format_link(GtkLabel *label, const char *uri, WigSettingsFilters *self)
{
  return wig_application_open_uri(wig_application_get(), GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))), uri);
}

static void wig_settings_filters_build_add_group(WigSettingsFilters *self)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, "Add a Filter");
  adw_preferences_group_set_description(group, "Rules are compiled as they are added.");

  self->name = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->name), "Name");
  adw_preferences_group_add(group, self->name);

  /* The group's own description is a label wig cannot reach, and a link in it
   * would be handed to the desktop; this one is answered here instead. */
  GtkWidget *format = gtk_label_new(
      "Blocking rules in <a href='https://webkit.org/blog/3476/content-blockers-first-look/'>WebKit content "
      "blocker</a> JSON format.");
  gtk_label_set_use_markup(GTK_LABEL(format), TRUE);
  gtk_label_set_xalign(GTK_LABEL(format), 0.0);
  gtk_label_set_wrap(GTK_LABEL(format), TRUE);
  gtk_widget_add_css_class(format, "caption");
  gtk_widget_add_css_class(format, "dim-label");
  gtk_widget_set_margin_top(format, 12);
  g_signal_connect_object(format, "activate-link", G_CALLBACK(filter_format_link), self, G_CONNECT_DEFAULT);
  adw_preferences_group_add(group, format);

  self->rules = gtk_text_view_new();
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->rules), TRUE);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->rules), 6);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(self->rules), 6);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->rules), 6);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->rules), 6);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), self->rules);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), 160);
  gtk_widget_add_css_class(scroller, "card");
  gtk_widget_set_margin_top(scroller, 12);
  adw_preferences_group_add(group, scroller);

  self->error = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->error), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->error), TRUE);
  gtk_label_set_selectable(GTK_LABEL(self->error), TRUE);
  gtk_widget_add_css_class(self->error, "error");
  gtk_widget_add_css_class(self->error, "caption");
  gtk_widget_set_margin_top(self->error, 6);
  gtk_widget_set_visible(self->error, FALSE);
  adw_preferences_group_add(group, self->error);

  self->open = gtk_button_new_with_label("Open File…");
  g_signal_connect_object(self->open, "clicked", G_CALLBACK(filter_file_opened), self, G_CONNECT_DEFAULT);

  self->add = gtk_button_new_with_label("Add Filter");
  gtk_widget_add_css_class(self->add, "suggested-action");
  g_signal_connect_object(self->add, "clicked", G_CALLBACK(filter_added), self, G_CONNECT_DEFAULT);

  self->spinner = adw_spinner_new();
  gtk_widget_set_size_request(self->spinner, 16, 16);
  gtk_widget_set_valign(self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->spinner, FALSE);

  self->status = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->status), 0.0);
  gtk_widget_add_css_class(self->status, "dim-label");
  gtk_widget_add_css_class(self->status, "caption");
  gtk_widget_set_hexpand(self->status, TRUE);
  gtk_widget_set_visible(self->status, FALSE);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(buttons, 12);
  gtk_box_append(GTK_BOX(buttons), self->spinner);
  gtk_box_append(GTK_BOX(buttons), self->status);
  gtk_box_append(GTK_BOX(buttons), self->open);
  gtk_box_append(GTK_BOX(buttons), self->add);
  adw_preferences_group_add(group, buttons);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), group);
}

static void wig_settings_filters_map(GtkWidget *widget)
{
  WigSettingsFilters *self = WIG_SETTINGS_FILTERS(widget);

  GTK_WIDGET_CLASS(wig_settings_filters_parent_class)->map(widget);

  /* What is installed is asked for the first time the pane is looked at, and
   * again whenever it changes. */
  if (self->populated)
    return;

  self->populated = TRUE;
  wig_settings_filters_refresh(self);
}

static void wig_settings_filters_dispose(GObject *object)
{
  WigSettingsFilters *self = WIG_SETTINGS_FILTERS(object);

  g_clear_pointer(&self->page, gtk_widget_unparent);
  g_clear_pointer(&self->rows, g_ptr_array_unref);

  G_OBJECT_CLASS(wig_settings_filters_parent_class)->dispose(object);
}

static void wig_settings_filters_class_init(WigSettingsFiltersClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_filters_dispose;
  widget_class->map = wig_settings_filters_map;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-filters");
}

static void wig_settings_filters_init(WigSettingsFilters *self)
{
  WigApplication *app = wig_application_get();

  self->manager = wig_application_get_user_content_manager(app);
  self->store = wig_application_get_content_filter_store(app);
  self->rows = g_ptr_array_new();

  self->page = adw_preferences_page_new();
  gtk_widget_set_parent(self->page, GTK_WIDGET(self));

  self->installed = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(self->installed, "Content Filters");
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), self->installed);

  self->empty_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->empty_row), "No filters installed");
  adw_preferences_group_add(self->installed, self->empty_row);

  wig_settings_filters_build_add_group(self);
}

GtkWidget *wig_settings_filters_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_FILTERS, NULL);
}

typedef struct {
  WebKitUserContentManager *manager;
  char *identifier;
} LoadSavedState;

static void on_load_saved_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  LoadSavedState *load = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(WebKitUserContentFilter) filter = webkit_user_content_filter_store_load_finish(
      WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);

  if (filter) {
    webkit_user_content_manager_add_filter(load->manager, filter);
    g_debug("content-filters: restored filter '%s'", load->identifier);
  } else {
    g_warning("content-filters: failed to restore '%s': %s", load->identifier, error->message);
  }

  g_object_unref(load->manager);
  g_free(load->identifier);
  g_free(load);
}

static void on_load_saved_fetch_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WebKitUserContentManager) manager = user_data;
  WebKitUserContentFilterStore *store = WEBKIT_USER_CONTENT_FILTER_STORE(source);
  g_auto(GStrv) identifiers = webkit_user_content_filter_store_fetch_identifiers_finish(store, result);

  for (char **id = identifiers; id && *id; id++) {
    LoadSavedState *load = g_new0(LoadSavedState, 1);
    load->manager = g_object_ref(manager);
    load->identifier = g_strdup(*id);
    webkit_user_content_filter_store_load(store, *id, NULL, on_load_saved_done, load);
  }
}

void wig_content_filters_load_saved(WebKitUserContentManager *manager, WebKitUserContentFilterStore *store)
{
  webkit_user_content_filter_store_fetch_identifiers(store, NULL, on_load_saved_fetch_done, g_object_ref(manager));
}
