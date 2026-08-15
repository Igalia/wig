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

#include "wig-settings-user-content.h"

#include "wig-application.h"

/* Scripts and style sheets are the same page with different nouns: a list of
 * what is installed, and a form taking source and two choices about how it is
 * applied. Only what those choices are called differs. */
static const struct {
  const char *title;
  const char *description;
  const char *item;
  const char *empty;
  const char *add_title;
  const char *add_button;
  const char *missing_source;
  const char *option_title;
  const char *option_choices[3];
  const char *option_labels[2];
  const char *log_domain;
} kinds[] = {
  [WIG_USER_CONTENT_SCRIPTS] = {
    .title = "User Scripts",
    .description = "JavaScript injected into every page you visit.",
    .item = "Script",
    .empty = "No user scripts installed",
    .add_title = "Add a Script",
    .add_button = "Add Script",
    .missing_source = "A script needs some source.",
    .option_title = "Inject At",
    .option_choices = { "Document Start", "Document End", NULL },
    .option_labels = { "Document Start", "Document End" },
    .log_domain = "user-scripts",
  },
  [WIG_USER_CONTENT_STYLES] = {
    .title = "User Styles",
    .description = "CSS injected into every page you visit.",
    .item = "Style Sheet",
    .empty = "No user styles installed",
    .add_title = "Add a Style Sheet",
    .add_button = "Add Style Sheet",
    .missing_source = "A style sheet needs some source.",
    .option_title = "Level",
    .option_choices = { "User (overrides page styles)", "Author (page styles may override)", NULL },
    .option_labels = { "User Level", "Author Level" },
    .log_domain = "user-styles",
  },
};

static const char *const frames_choices[] = { "All Frames", "Top Frame Only", NULL };

struct _WigSettingsUserContent {
  GtkWidget parent;

  WigUserContentKind kind;
  WebKitUserContentManager *manager;
  /* The application's list, which is what is actually installed. */
  GPtrArray *records;

  GtkWidget *page;
  AdwPreferencesGroup *installed;
  GtkWidget *empty_row;
  GPtrArray *rows;

  GtkWidget *source;
  GtkWidget *option;
  GtkWidget *frames;
  GtkWidget *error;
};

G_DEFINE_FINAL_TYPE(WigSettingsUserContent, wig_settings_user_content, GTK_TYPE_WIDGET)

/* What a row needs to take its own item away again. */
typedef struct {
  WigSettingsUserContent *pane;
  gpointer record;
  GtkWidget *row;
} Entry;

static void wig_settings_user_content_refresh(WigSettingsUserContent *self);

static const char *record_source(WigSettingsUserContent *self, gpointer record)
{
  if (self->kind == WIG_USER_CONTENT_SCRIPTS)
    return ((WigUserScriptRecord *)record)->source;

  return ((WigUserStyleSheetRecord *)record)->source;
}

/* Both kinds offer two ways of being applied, and which one an item took is what
 * its row leads with. */
static guint record_option(WigSettingsUserContent *self, gpointer record)
{
  if (self->kind == WIG_USER_CONTENT_SCRIPTS)
    return ((WigUserScriptRecord *)record)->injection_time == WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END ? 1 : 0;

  return ((WigUserStyleSheetRecord *)record)->level == WEBKIT_USER_STYLE_LEVEL_AUTHOR ? 1 : 0;
}

static guint record_frames(WigSettingsUserContent *self, gpointer record)
{
  WebKitUserContentInjectedFrames frames = self->kind == WIG_USER_CONTENT_SCRIPTS
      ? ((WigUserScriptRecord *)record)->injected_frames
      : ((WigUserStyleSheetRecord *)record)->injected_frames;

  return frames == WEBKIT_USER_CONTENT_INJECT_TOP_FRAME ? 1 : 0;
}

static void record_uninstall(WigSettingsUserContent *self, gpointer record)
{
  if (self->kind == WIG_USER_CONTENT_SCRIPTS)
    webkit_user_content_manager_remove_script(self->manager, ((WigUserScriptRecord *)record)->script);
  else
    webkit_user_content_manager_remove_style_sheet(self->manager, ((WigUserStyleSheetRecord *)record)->stylesheet);
}

static void record_install(WigSettingsUserContent *self, const char *source, guint option, guint frames)
{
  WebKitUserContentInjectedFrames injected = frames == 1 ? WEBKIT_USER_CONTENT_INJECT_TOP_FRAME
                                                         : WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES;

  if (self->kind == WIG_USER_CONTENT_SCRIPTS) {
    WebKitUserScriptInjectionTime time = option == 1 ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END
                                                     : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START;
    WigUserScriptRecord *record = g_new0(WigUserScriptRecord, 1);

    record->source = g_strdup(source);
    record->injection_time = time;
    record->injected_frames = injected;
    record->script = webkit_user_script_new(source, injected, time, NULL, NULL);

    webkit_user_content_manager_add_script(self->manager, record->script);
    g_ptr_array_add(self->records, record);
  } else {
    WebKitUserStyleLevel level = option == 1 ? WEBKIT_USER_STYLE_LEVEL_AUTHOR : WEBKIT_USER_STYLE_LEVEL_USER;
    WigUserStyleSheetRecord *record = g_new0(WigUserStyleSheetRecord, 1);

    record->source = g_strdup(source);
    record->level = level;
    record->injected_frames = injected;
    record->stylesheet = webkit_user_style_sheet_new(source, injected, level, NULL, NULL);

    webkit_user_content_manager_add_style_sheet(self->manager, record->stylesheet);
    g_ptr_array_add(self->records, record);
  }
}

static void wig_settings_user_content_report(WigSettingsUserContent *self, const char *message)
{
  gtk_label_set_label(GTK_LABEL(self->error), message ? message : "");
  gtk_widget_set_visible(self->error, message != NULL);
}

static char *source_text(WigSettingsUserContent *self)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->source));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(buffer, &start, &end);

  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

/* The opening line is usually what an item calls itself, and something has to
 * tell one row from another. */
static char *source_summary(WigSettingsUserContent *self, const char *source)
{
  const char *newline = strchr(source, '\n');
  g_autofree char *first = newline ? g_strndup(source, (gsize)(newline - source)) : g_strdup(source);

  g_strstrip(first);

  return *first ? g_steal_pointer(&first) : g_strdup(kinds[self->kind].item);
}

static char *source_lines(const char *source)
{
  guint lines = 1;

  for (const char *p = source; *p; p++) {
    if (*p == '\n')
      lines++;
  }

  return g_strdup_printf("%u line%s", lines, lines == 1 ? "" : "s");
}

static void entry_removed(GtkButton *button, Entry *entry)
{
  WigSettingsUserContent *self = entry->pane;
  gpointer record = entry->record;

  g_debug("%s: removing one of %u", kinds[self->kind].log_domain, self->records->len);
  record_uninstall(self, record);
  g_ptr_array_remove(self->records, record);
  wig_settings_user_content_refresh(self);
}

static GtkWidget *user_content_source_view(const char *source, gboolean editable)
{
  GtkWidget *view = gtk_text_view_new();

  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 6);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 6);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 6);

  if (source)
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), source, -1);

  return view;
}

static GtkWidget *user_content_source_card(GtkWidget *view, int height)
{
  GtkWidget *scroller = gtk_scrolled_window_new();

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), height);
  gtk_widget_add_css_class(scroller, "card");

  return scroller;
}

static void wig_settings_user_content_add_row(WigSettingsUserContent *self, Entry *entry)
{
  const char *source = record_source(self, entry->record);
  const char *option = kinds[self->kind].option_labels[record_option(self, entry->record)];
  const char *frames = frames_choices[record_frames(self, entry->record)];
  g_autofree char *summary = source_summary(self, source);
  g_autofree char *lines = source_lines(source);
  g_autofree char *subtitle = g_strdup_printf("%s · %s · %s", option, frames, lines);

  entry->row = adw_expander_row_new();
  /* Source is whatever was pasted in, which is not markup. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(entry->row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry->row), summary);
  adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(entry->row), subtitle);

  GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_tooltip_text(remove, "Remove");
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(remove, "flat");
  g_signal_connect(remove, "clicked", G_CALLBACK(entry_removed), entry);
  adw_expander_row_add_suffix(ADW_EXPANDER_ROW(entry->row), remove);

  GtkWidget *card = user_content_source_card(user_content_source_view(source, FALSE), 120);
  gtk_widget_set_margin_top(card, 6);
  gtk_widget_set_margin_bottom(card, 6);
  gtk_widget_set_margin_start(card, 6);
  gtk_widget_set_margin_end(card, 6);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(entry->row), card);

  adw_preferences_group_add(self->installed, entry->row);
}

static void wig_settings_user_content_refresh(WigSettingsUserContent *self)
{
  for (guint i = 0; i < self->rows->len; i++) {
    Entry *entry = g_ptr_array_index(self->rows, i);

    adw_preferences_group_remove(self->installed, entry->row);
  }

  g_ptr_array_set_size(self->rows, 0);

  for (guint i = 0; i < self->records->len; i++) {
    Entry *entry = g_new0(Entry, 1);

    entry->pane = self;
    entry->record = g_ptr_array_index(self->records, i);
    g_ptr_array_add(self->rows, entry);
    wig_settings_user_content_add_row(self, entry);
  }

  gtk_widget_set_visible(self->empty_row, self->records->len == 0);
}

static void entry_added(GtkButton *button, WigSettingsUserContent *self)
{
  g_autofree char *source = source_text(self);

  if (!*source) {
    wig_settings_user_content_report(self, kinds[self->kind].missing_source);
    return;
  }

  wig_settings_user_content_report(self, NULL);
  record_install(self, source, adw_combo_row_get_selected(ADW_COMBO_ROW(self->option)),
                 adw_combo_row_get_selected(ADW_COMBO_ROW(self->frames)));
  g_debug("%s: added one, total %u", kinds[self->kind].log_domain, self->records->len);

  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->source)), "", -1);
  wig_settings_user_content_refresh(self);
}

static GtkWidget *user_content_combo_row(const char *title, const char *const *choices)
{
  GtkWidget *row = adw_combo_row_new();
  g_autoptr(GtkStringList) model = gtk_string_list_new(choices);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));

  return row;
}

static void wig_settings_user_content_build_add_group(WigSettingsUserContent *self)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

  adw_preferences_group_set_title(group, kinds[self->kind].add_title);
  adw_preferences_group_set_description(group, "What is added applies until wig is closed.");

  self->option = user_content_combo_row(kinds[self->kind].option_title, kinds[self->kind].option_choices);
  adw_preferences_group_add(group, self->option);

  self->frames = user_content_combo_row("Frames", frames_choices);
  adw_preferences_group_add(group, self->frames);

  self->source = user_content_source_view(NULL, TRUE);
  GtkWidget *card = user_content_source_card(self->source, 160);
  gtk_widget_set_margin_top(card, 12);
  adw_preferences_group_add(group, card);

  self->error = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->error), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->error), TRUE);
  gtk_widget_add_css_class(self->error, "error");
  gtk_widget_add_css_class(self->error, "caption");
  gtk_widget_set_margin_top(self->error, 6);
  gtk_widget_set_visible(self->error, FALSE);
  adw_preferences_group_add(group, self->error);

  GtkWidget *add = gtk_button_new_with_label(kinds[self->kind].add_button);
  gtk_widget_add_css_class(add, "suggested-action");
  gtk_widget_set_halign(add, GTK_ALIGN_END);
  gtk_widget_set_margin_top(add, 12);
  g_signal_connect(add, "clicked", G_CALLBACK(entry_added), self);
  adw_preferences_group_add(group, add);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), group);
}

static void wig_settings_user_content_dispose(GObject *object)
{
  WigSettingsUserContent *self = WIG_SETTINGS_USER_CONTENT(object);

  g_clear_pointer(&self->page, gtk_widget_unparent);
  g_clear_pointer(&self->rows, g_ptr_array_unref);

  G_OBJECT_CLASS(wig_settings_user_content_parent_class)->dispose(object);
}

static void wig_settings_user_content_class_init(WigSettingsUserContentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_user_content_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-user-content");
}

static void wig_settings_user_content_init(WigSettingsUserContent *self)
{
  self->manager = wig_application_get_user_content_manager(wig_application_get());
  self->rows = g_ptr_array_new_with_free_func(g_free);

  self->page = adw_preferences_page_new();
  gtk_widget_set_parent(self->page, GTK_WIDGET(self));
}

GtkWidget *wig_settings_user_content_new(WigUserContentKind kind)
{
  WigApplication *app = wig_application_get();
  WigSettingsUserContent *self = g_object_new(WIG_TYPE_SETTINGS_USER_CONTENT, NULL);

  /* Which kind this is decides everything the page says, so it is settled
   * before anything is built. */
  self->kind = kind;
  self->records = kind == WIG_USER_CONTENT_SCRIPTS ? wig_application_get_user_scripts(app)
                                                   : wig_application_get_user_style_sheets(app);

  self->installed = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(self->installed, kinds[kind].title);
  adw_preferences_group_set_description(self->installed, kinds[kind].description);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), self->installed);

  self->empty_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->empty_row), kinds[kind].empty);
  adw_preferences_group_add(self->installed, self->empty_row);

  wig_settings_user_content_build_add_group(self);
  wig_settings_user_content_refresh(self);

  return GTK_WIDGET(self);
}
