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

#include "wig-history-dialog.h"

#include "wig-application.h"
#include "wig-history-store.h"
#include "wig-web-app-filter.h"

#define HISTORY_DIALOG_LIMIT 100

enum {
  OPEN_URI_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

struct _WigHistoryDialog {
  AdwDialog parent;

  WigHistoryStore *store;
  GListStore *items;
  char *query;
  /* Where the next batch picks up: the time of the oldest visit shown. */
  gint64 before_time;

  gboolean has_more;
  /* Set while a batch is being taken on, since adding to the list moves the
   * scrollbar and asking for the next one from there would never end. */
  gboolean loading;

  GtkWidget *entry;
  GtkWidget *filter;
  GtkWidget *stack;
  GtkWidget *list;
  GtkWidget *empty;
};

G_DEFINE_FINAL_TYPE(WigHistoryDialog, wig_history_dialog, ADW_TYPE_DIALOG)

static char *history_visit_time(gint64 visit_time)
{
  g_autoptr(GDateTime) date_time = g_date_time_new_from_unix_local(visit_time / 1000);

  return date_time ? g_date_time_format(date_time, "%x %X") : g_strdup("");
}

static void wig_history_dialog_load(WigHistoryDialog *self, gboolean append)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = NULL;
  const char *web_app = wig_web_app_filter_get_web_app(WIG_WEB_APP_FILTER(self->filter));

  if (!append) {
    self->before_time = 0;
    g_list_store_remove_all(self->items);
  }

  gint64 oldest = self->before_time;

  self->loading = TRUE;

  if (self->store)
    items = wig_history_store_query(self->store, self->query, web_app, self->before_time, HISTORY_DIALOG_LIMIT,
                                    &self->has_more, &error);

  if (error)
    g_warning("history: query failed: %s", error->message);

  for (guint i = 0; items && i < items->len; i++) {
    WigHistoryItem *item = g_ptr_array_index(items, i);

    self->before_time = wig_history_item_get_last_visit_time(item);
    g_list_store_append(self->items, item);
  }

  if (append && self->before_time == oldest)
    self->has_more = FALSE;

  self->loading = FALSE;

  g_debug("history: showing %u item(s)%s%s from %s, more=%s", g_list_model_get_n_items(G_LIST_MODEL(self->items)),
          self->query ? " matching " : "", self->query ? self->query : "", web_app ? web_app : "the browser",
          self->has_more ? "yes" : "no");

  adw_status_page_set_title(ADW_STATUS_PAGE(self->empty), self->query ? "No Results" : "No History Yet");
  if (self->query)
    adw_status_page_set_description(ADW_STATUS_PAGE(self->empty), "No page you have visited matches that.");
  else if (web_app)
    adw_status_page_set_description(ADW_STATUS_PAGE(self->empty), "Pages you visit in this web app are listed here.");
  else
    adw_status_page_set_description(ADW_STATUS_PAGE(self->empty), "Pages you visit are listed here.");
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack),
                                   g_list_model_get_n_items(G_LIST_MODEL(self->items)) > 0 ? "list" : "empty");
}

static void wig_history_dialog_search_changed(WigHistoryDialog *self, GtkSearchEntry *entry)
{
  const char *terms = gtk_editable_get_text(GTK_EDITABLE(entry));
  const char *query = *terms ? terms : NULL;

  if (g_strcmp0(query, self->query) == 0)
    return;

  g_set_str(&self->query, query);
  wig_history_dialog_load(self, FALSE);
}

static void wig_history_dialog_web_app_changed(WigHistoryDialog *self, GParamSpec *pspec, WigWebAppFilter *filter)
{
  wig_history_dialog_load(self, FALSE);
}

/* The next batch is taken on as the end of what is shown comes into view, so
 * scrolling never stops at a button. A batch that does not fill the view leaves
 * the end still in sight, and the adjustment says so again until it does. */
static void wig_history_dialog_scrolled(WigHistoryDialog *self, GtkAdjustment *adjustment)
{
  double remaining = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_value(adjustment)
      - gtk_adjustment_get_page_size(adjustment);

  if (!self->has_more || self->loading || remaining > gtk_adjustment_get_page_size(adjustment))
    return;

  wig_history_dialog_load(self, TRUE);
}

static void wig_history_dialog_row_activated(WigHistoryDialog *self, guint position)
{
  g_autoptr(WigHistoryItem) item = g_list_model_get_item(G_LIST_MODEL(self->items), position);

  if (item)
    g_signal_emit(self, signals[OPEN_URI_SIGNAL], 0, wig_history_item_get_url(item), FALSE);
}

static void history_row_middle_clicked(GtkGestureClick *gesture, int n_press, double x, double y,
                                       WigHistoryDialog *self)
{
  GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  const char *url = gtk_widget_get_name(row);

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_signal_emit(self, signals[OPEN_URI_SIGNAL], 0, url, TRUE);
}

static void history_row_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item, WigHistoryDialog *self)
{
  GtkWidget *title = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);

  GtkWidget *url = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(url), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(url), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(url, "subtitle");

  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(page, TRUE);
  gtk_widget_set_valign(page, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(page), title);
  gtk_box_append(GTK_BOX(page), url);

  GtkWidget *when = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(when), 1.0);
  gtk_widget_add_css_class(when, "dim-label");
  gtk_widget_add_css_class(when, "numeric");

  GtkWidget *counts = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(counts), 1.0);
  gtk_widget_add_css_class(counts, "dim-label");
  gtk_widget_add_css_class(counts, "caption");

  GtkWidget *visits = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(visits, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(visits), when);
  gtk_box_append(GTK_BOX(visits), counts);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append(GTK_BOX(row), page);
  gtk_box_append(GTK_BOX(row), visits);

  GtkGesture *middle_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle_click), GDK_BUTTON_MIDDLE);
  g_signal_connect_object(middle_click, "released", G_CALLBACK(history_row_middle_clicked), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(middle_click));

  gtk_list_item_set_child(list_item, row);
}

static void history_row_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item, WigHistoryDialog *self)
{
  WigHistoryItem *item = gtk_list_item_get_item(list_item);
  GtkWidget *row = gtk_list_item_get_child(list_item);
  GtkWidget *page = gtk_widget_get_first_child(row);
  GtkWidget *visits = gtk_widget_get_last_child(row);
  const char *title = wig_history_item_get_title(item);
  const char *url = wig_history_item_get_url(item);
  g_autofree char *visited = history_visit_time(wig_history_item_get_last_visit_time(item));
  g_autofree char *counts = g_strdup_printf("%u visits · %u typed", wig_history_item_get_visit_count(item),
                                            wig_history_item_get_typed_count(item));

  /* The row is the only thing that knows which address it is showing. */
  gtk_widget_set_name(row, url);
  gtk_label_set_label(GTK_LABEL(gtk_widget_get_first_child(page)), title && *title ? title : url);
  gtk_label_set_label(GTK_LABEL(gtk_widget_get_last_child(page)), url);
  gtk_label_set_label(GTK_LABEL(gtk_widget_get_first_child(visits)), visited);
  gtk_label_set_label(GTK_LABEL(gtk_widget_get_last_child(visits)), counts);
}

static gboolean wig_history_dialog_start_search(GtkWidget *widget, GVariant *args, gpointer user_data)
{
  WigHistoryDialog *self = WIG_HISTORY_DIALOG(widget);

  gtk_widget_grab_focus(self->entry);
  return TRUE;
}

static void wig_history_dialog_dispose(GObject *object)
{
  WigHistoryDialog *self = WIG_HISTORY_DIALOG(object);

  g_clear_object(&self->items);
  g_clear_pointer(&self->query, g_free);

  G_OBJECT_CLASS(wig_history_dialog_parent_class)->dispose(object);
}

static void wig_history_dialog_class_init(WigHistoryDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_history_dialog_dispose;

  signals[OPEN_URI_SIGNAL] = g_signal_new("open-uri", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  gtk_widget_class_add_binding(widget_class, GDK_KEY_f, GDK_CONTROL_MASK, wig_history_dialog_start_search, NULL);
}

static void wig_history_dialog_init(WigHistoryDialog *self)
{
  self->store = wig_application_get_history_store(wig_application_get());
  self->items = g_list_store_new(WIG_TYPE_HISTORY_ITEM);

  self->entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->entry, TRUE);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->entry), "Search History");
  g_signal_connect_object(self->entry, "search-changed", G_CALLBACK(wig_history_dialog_search_changed), self,
                          G_CONNECT_SWAPPED);

  self->filter = wig_web_app_filter_new();
  gtk_widget_set_tooltip_text(self->filter, "Show History From");
  g_signal_connect_object(self->filter, "notify::web-app", G_CALLBACK(wig_history_dialog_web_app_changed), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(bar), self->entry);
  gtk_box_append(GTK_BOX(bar), self->filter);
  gtk_widget_set_margin_top(bar, 6);
  gtk_widget_set_margin_bottom(bar, 6);
  gtk_widget_set_margin_start(bar, 12);
  gtk_widget_set_margin_end(bar, 12);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect_object(factory, "setup", G_CALLBACK(history_row_setup), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(factory, "bind", G_CALLBACK(history_row_bind), self, G_CONNECT_DEFAULT);

  self->list = gtk_list_view_new(GTK_SELECTION_MODEL(gtk_no_selection_new(g_object_ref(G_LIST_MODEL(self->items)))),
                                 factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(self->list), TRUE);
  gtk_widget_add_css_class(self->list, "rich-list");
  gtk_widget_add_css_class(self->list, "separators");
  g_signal_connect_object(self->list, "activate", G_CALLBACK(wig_history_dialog_row_activated), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), self->list);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroller, TRUE);

  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
  g_signal_connect_object(adjustment, "value-changed", G_CALLBACK(wig_history_dialog_scrolled), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(adjustment, "changed", G_CALLBACK(wig_history_dialog_scrolled), self, G_CONNECT_SWAPPED);

  self->empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty), "document-open-recent-symbolic");

  self->stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->stack), scroller, "list");
  gtk_stack_add_named(GTK_STACK(self->stack), self->empty, "empty");

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), bar);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->stack);

  adw_dialog_set_title(ADW_DIALOG(self), "History");
  adw_dialog_set_content_width(ADW_DIALOG(self), 720);
  adw_dialog_set_content_height(ADW_DIALOG(self), 560);
  adw_dialog_set_child(ADW_DIALOG(self), toolbar);
}

void wig_history_dialog_reload(WigHistoryDialog *self)
{
  wig_history_dialog_load(self, FALSE);
}

AdwDialog *wig_history_dialog_new(const char *web_app)
{
  WigHistoryDialog *self = g_object_new(WIG_TYPE_HISTORY_DIALOG, NULL);

  wig_web_app_filter_set_web_app(WIG_WEB_APP_FILTER(self->filter), web_app);
  if (!web_app)
    wig_history_dialog_load(self, FALSE);

  return ADW_DIALOG(self);
}
