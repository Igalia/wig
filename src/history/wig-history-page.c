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

#include "wig-history-page.h"

#include "wig-application.h"
#include "wig-history-store.h"

#include <adwaita.h>

/* What one look at the store asks for, the same batch the page used to render at
 * a time. */
#define HISTORY_PAGE_LIMIT 100

/* Rows carry more than a line of text, so they are given more room than the
 * clamp would otherwise allow. */
#define HISTORY_PAGE_WIDTH 900
#define HISTORY_PAGE_TIGHTENING 600

enum {
  OPEN_URI_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

struct _WigHistoryPage {
  WigNativePage parent;

  WigHistoryStore *store;
  GListStore *items;
  char *query;
  /* Where the next batch picks up: the time of the oldest visit shown. */
  gint64 before_time;

  gboolean has_more;
  /* Set while a batch is being taken on, since adding to the list moves the
   * scrollbar and asking for the next one from there would never end. */
  gboolean loading;

  GtkWidget *toolbar;
  GtkWidget *entry;
  GtkWidget *stack;
  GtkWidget *list;
  GtkWidget *empty;
};

G_DEFINE_FINAL_TYPE(WigHistoryPage, wig_history_page, WIG_TYPE_NATIVE_PAGE)

gboolean uri_is_history_page(const char *uri)
{
  g_autoptr(GUri) parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;

  return parsed && g_strcmp0(g_uri_get_scheme(parsed), "wig") == 0 && g_str_equal(g_uri_get_path(parsed), "history");
}

static char *history_query_for_uri(const char *uri)
{
  g_autoptr(GUri) parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
  const char *raw_query = parsed ? g_uri_get_query(parsed) : NULL;
  g_autoptr(GHashTable) params = raw_query ? g_uri_parse_params(raw_query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
  const char *query = params ? g_hash_table_lookup(params, "q") : NULL;

  return query && *query ? g_strdup(query) : NULL;
}

static char *history_visit_time(gint64 visit_time)
{
  g_autoptr(GDateTime) date_time = g_date_time_new_from_unix_local(visit_time / 1000);

  return date_time ? g_date_time_format(date_time, "%x %X") : g_strdup("");
}

static void wig_history_page_load(WigHistoryPage *self, gboolean append)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = NULL;

  if (!append) {
    self->before_time = 0;
    g_list_store_remove_all(self->items);
  }

  gint64 oldest = self->before_time;

  self->loading = TRUE;

  if (self->store)
    items = wig_history_store_query(self->store, self->query, self->before_time, HISTORY_PAGE_LIMIT, &self->has_more,
                                    &error);

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

  g_debug("history: showing %u item(s)%s%s, more=%s", g_list_model_get_n_items(G_LIST_MODEL(self->items)),
          self->query ? " matching " : "", self->query ? self->query : "", self->has_more ? "yes" : "no");

  adw_status_page_set_title(ADW_STATUS_PAGE(self->empty), self->query ? "No Results" : "No History Yet");
  adw_status_page_set_description(ADW_STATUS_PAGE(self->empty),
                                  self->query ? "No page you have visited matches that."
                                              : "Pages you visit are listed here.");
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack),
                                   g_list_model_get_n_items(G_LIST_MODEL(self->items)) > 0 ? "list" : "empty");
}

static void wig_history_page_search_changed(WigHistoryPage *self, GtkSearchEntry *entry)
{
  const char *terms = gtk_editable_get_text(GTK_EDITABLE(entry));
  const char *query = *terms ? terms : NULL;

  if (g_strcmp0(query, self->query) == 0)
    return;

  g_set_str(&self->query, query);
  wig_history_page_load(self, FALSE);
}

/* The next batch is taken on as the end of what is shown comes into view, so
 * scrolling never stops at a button. A batch that does not fill the view leaves
 * the end still in sight, and the adjustment says so again until it does. */
static void wig_history_page_scrolled(WigHistoryPage *self, GtkAdjustment *adjustment)
{
  double remaining = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_value(adjustment)
      - gtk_adjustment_get_page_size(adjustment);

  if (!self->has_more || self->loading || remaining > gtk_adjustment_get_page_size(adjustment))
    return;

  wig_history_page_load(self, TRUE);
}

static void wig_history_page_row_activated(WigHistoryPage *self, guint position)
{
  g_autoptr(WigHistoryItem) item = g_list_model_get_item(G_LIST_MODEL(self->items), position);

  if (item)
    g_signal_emit(self, signals[OPEN_URI_SIGNAL], 0, wig_history_item_get_url(item), FALSE);
}

static void history_row_middle_clicked(GtkGestureClick *gesture, int n_press, double x, double y, WigHistoryPage *self)
{
  GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  const char *url = gtk_widget_get_name(row);

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_signal_emit(self, signals[OPEN_URI_SIGNAL], 0, url, TRUE);
}

static void history_row_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item, WigHistoryPage *self)
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

static void history_row_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item, WigHistoryPage *self)
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

static void wig_history_page_apply_uri(WigHistoryPage *self)
{
  g_autofree char *query = history_query_for_uri(wig_native_page_get_uri(WIG_NATIVE_PAGE(self)));

  g_set_str(&self->query, query);
  gtk_editable_set_text(GTK_EDITABLE(self->entry), query ? query : "");
  wig_history_page_load(self, FALSE);
}

static void wig_history_page_uri_changed(WigHistoryPage *self)
{
  g_autofree char *query = history_query_for_uri(wig_native_page_get_uri(WIG_NATIVE_PAGE(self)));

  if (g_strcmp0(query, self->query) != 0)
    wig_history_page_apply_uri(self);
}

static gboolean wig_history_page_start_search(WigNativePage *page)
{
  WigHistoryPage *self = WIG_HISTORY_PAGE(page);

  gtk_widget_grab_focus(self->entry);
  return TRUE;
}

static void wig_history_page_dispose(GObject *object)
{
  WigHistoryPage *self = WIG_HISTORY_PAGE(object);

  g_clear_pointer(&self->toolbar, gtk_widget_unparent);
  g_clear_object(&self->items);
  g_clear_pointer(&self->query, g_free);

  G_OBJECT_CLASS(wig_history_page_parent_class)->dispose(object);
}

static void wig_history_page_class_init(WigHistoryPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_history_page_dispose;
  WIG_NATIVE_PAGE_CLASS(klass)->start_search = wig_history_page_start_search;

  /* Following an entry is the tab's to carry out, since the page is only what is
   * drawn over it. */
  signals[OPEN_URI_SIGNAL] = g_signal_new("open-uri", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  gtk_widget_class_set_css_name(widget_class, "wig-history-page");
}

static void wig_history_page_init(WigHistoryPage *self)
{
  self->store = wig_application_get_history_store(wig_application_get());
  self->items = g_list_store_new(WIG_TYPE_HISTORY_ITEM);

  self->entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->entry, TRUE);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->entry), "Search History");
  g_signal_connect_object(self->entry, "search-changed", G_CALLBACK(wig_history_page_search_changed), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *entry_clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(entry_clamp), self->entry);
  adw_clamp_set_maximum_size(ADW_CLAMP(entry_clamp), HISTORY_PAGE_WIDTH);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(entry_clamp), HISTORY_PAGE_TIGHTENING);
  gtk_widget_set_margin_top(entry_clamp, 6);
  gtk_widget_set_margin_bottom(entry_clamp, 6);
  gtk_widget_set_margin_start(entry_clamp, 12);
  gtk_widget_set_margin_end(entry_clamp, 12);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect_object(factory, "setup", G_CALLBACK(history_row_setup), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(factory, "bind", G_CALLBACK(history_row_bind), self, G_CONNECT_DEFAULT);

  self->list = gtk_list_view_new(GTK_SELECTION_MODEL(gtk_no_selection_new(g_object_ref(G_LIST_MODEL(self->items)))),
                                 factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(self->list), TRUE);
  gtk_widget_add_css_class(self->list, "rich-list");
  gtk_widget_add_css_class(self->list, "separators");
  g_signal_connect_object(self->list, "activate", G_CALLBACK(wig_history_page_row_activated), self, G_CONNECT_SWAPPED);

  GtkWidget *clamp = adw_clamp_scrollable_new();
  adw_clamp_scrollable_set_child(ADW_CLAMP_SCROLLABLE(clamp), self->list);
  adw_clamp_scrollable_set_maximum_size(ADW_CLAMP_SCROLLABLE(clamp), HISTORY_PAGE_WIDTH);
  adw_clamp_scrollable_set_tightening_threshold(ADW_CLAMP_SCROLLABLE(clamp), HISTORY_PAGE_TIGHTENING);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroller, TRUE);

  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
  g_signal_connect_object(adjustment, "value-changed", G_CALLBACK(wig_history_page_scrolled), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(adjustment, "changed", G_CALLBACK(wig_history_page_scrolled), self, G_CONNECT_SWAPPED);

  self->empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty), "document-open-recent-symbolic");

  self->stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->stack), scroller, "list");
  gtk_stack_add_named(GTK_STACK(self->stack), self->empty, "empty");

  self->toolbar = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(self->toolbar), ADW_TOOLBAR_FLAT);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(self->toolbar), entry_clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(self->toolbar), self->stack);
  gtk_widget_set_parent(self->toolbar, GTK_WIDGET(self));

  wig_native_page_set_title(WIG_NATIVE_PAGE(self), WIG_HISTORY_PAGE_TITLE);
}

GtkWidget *wig_history_page_new(const char *uri)
{
  WigHistoryPage *self = g_object_new(WIG_TYPE_HISTORY_PAGE, "uri", uri, NULL);

  /* The address only arrives once construction is over, so the first look at the
   * store happens here rather than in init, where a search in it would not be
   * known yet and everything would be listed before being listed again.
   * Connecting afterwards leaves the handler to later addresses only, since the
   * address this page was made with is already accounted for. */
  wig_history_page_apply_uri(self);
  g_signal_connect(self, "notify::uri", G_CALLBACK(wig_history_page_uri_changed), NULL);

  return GTK_WIDGET(self);
}
