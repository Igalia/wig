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

#include "wig-new-tab-page.h"

#include "wig-application.h"
#include "wig-bookmarks-store.h"
#include "wig-favicon.h"
#include "wig-history-store.h"

#include <adwaita.h>

#define NEW_TAB_PAGE_SITE_LIMIT 9
#define NEW_TAB_PAGE_FAVICON_SIZE 32
#define NEW_TAB_PAGE_COLUMNS 3
#define NEW_TAB_PAGE_LABEL_CHARS 12
#define NEW_TAB_PAGE_WIDTH 900
#define NEW_TAB_PAGE_TIGHTENING 600

enum {
  OPEN_URI_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

struct _WigNewTabPage {
  WigNativePage parent;

  WigHistoryStore *store;
  WigBookmarksStore *bookmarks;
  WigSession *session;
  GSettings *settings;

  /* What the sections above frequently visited are already showing, so the same
   * site is not offered twice. */
  GHashTable *shown_urls;
  GListStore *favorite_items;
  GListModel *favorite_bookmarks;
  GListStore *frequent_items;
  GtkFilter *frequent_filter;
  GListModel *frequent_shown;

  GtkWidget *overlay;
  GtkWidget *stack;
  GtkWidget *favorites_section;
  GtkWidget *frequent_section;
  GtkWidget *closed_section;
  GtkWidget *favorites;
  GtkWidget *sites;
  GtkWidget *closed;
};

G_DEFINE_FINAL_TYPE(WigNewTabPage, wig_new_tab_page, WIG_TYPE_NATIVE_PAGE)

gboolean uri_is_new_tab_page(const char *uri)
{
  g_autoptr(GUri) parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;

  return parsed && g_strcmp0(g_uri_get_scheme(parsed), "wig") == 0 && g_str_equal(g_uri_get_path(parsed), "new-tab");
}

static char *new_tab_page_site_name(const char *url)
{
  g_autoptr(GUri) parsed = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  const char *host = parsed ? g_uri_get_host(parsed) : NULL;

  if (!host || !*host)
    return g_strdup(url);

  if (g_str_has_prefix(host, "www."))
    host += strlen("www.");

  return g_strdup(host);
}

#if HAVE_FAVICON_SUPPORT
static void new_tab_page_favicon_loaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GtkImage) image = GTK_IMAGE(user_data);
  g_autoptr(GError) error = NULL;
  GIcon *result_icon = wig_favicon_get_finish(WEBKIT_FAVICON_DATABASE(source), result, &error);
  g_autoptr(GObject) icon = result_icon ? G_OBJECT(result_icon) : NULL;

  if (icon)
    gtk_image_set_from_gicon(image, G_ICON(icon));
}

static void new_tab_page_load_favicon(GtkWidget *image, const char *url)
{
  WebKitNetworkSession *session = wig_application_get_network_session(wig_application_get());
  WebKitWebsiteDataManager *data_manager = webkit_network_session_get_website_data_manager(session);
  WebKitFaviconDatabase *database = webkit_website_data_manager_get_favicon_database(data_manager);

  if (!database)
    return;

  wig_favicon_get_async(database, url, NEW_TAB_PAGE_FAVICON_SIZE, NULL, new_tab_page_favicon_loaded,
                        g_object_ref(image));
}
#endif

static void new_tab_page_site_clicked(WigNewTabPage *self, GtkButton *button)
{
  g_signal_emit(self, signals[OPEN_URI_SIGNAL], 0, gtk_widget_get_name(GTK_WIDGET(button)), FALSE);
}

static void new_tab_page_site_middle_clicked(GtkGestureClick *gesture, int n_press, double x, double y,
                                             WigNewTabPage *self)
{
  GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_signal_emit(self, signals[OPEN_URI_SIGNAL], 0, gtk_widget_get_name(button), TRUE);
}

static GtkWidget *new_tab_page_build_card(const char *icon_name, const char *title, const char *subtitle,
                                          GtkWidget **icon)
{
  GtkWidget *image = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(image), NEW_TAB_PAGE_FAVICON_SIZE);

  GtkWidget *name = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(name), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(name), NEW_TAB_PAGE_LABEL_CHARS);
  gtk_widget_add_css_class(name, "site-name");

  GtkWidget *host = gtk_label_new(subtitle);
  gtk_label_set_xalign(GTK_LABEL(host), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(host), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(host), NEW_TAB_PAGE_LABEL_CHARS);
  gtk_widget_add_css_class(host, "caption");
  gtk_widget_add_css_class(host, "dim-label");

  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(labels, TRUE);
  gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(labels), name);
  gtk_box_append(GTK_BOX(labels), host);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_box_append(GTK_BOX(content), image);
  gtk_box_append(GTK_BOX(content), labels);

  GtkWidget *button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(button), content);
  gtk_widget_add_css_class(button, "card");
  gtk_widget_add_css_class(button, "frequent-site");

  *icon = image;
  return button;
}

static GtkWidget *new_tab_page_build_site(WigNewTabPage *self, const char *url, const char *title)
{
  g_autofree char *site = new_tab_page_site_name(url);
  GtkWidget *icon = NULL;
  GtkWidget *button = new_tab_page_build_card("web-browser-symbolic", title && *title ? title : site, site, &icon);

#if HAVE_FAVICON_SUPPORT
  new_tab_page_load_favicon(icon, url);
#endif

  gtk_widget_set_tooltip_text(button, url);
  gtk_widget_set_name(button, url);
  g_signal_connect_object(button, "clicked", G_CALLBACK(new_tab_page_site_clicked), self, G_CONNECT_SWAPPED);

  GtkGesture *middle_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle_click), GDK_BUTTON_MIDDLE);
  g_signal_connect_object(middle_click, "released", G_CALLBACK(new_tab_page_site_middle_clicked), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(middle_click));

  return button;
}

static GtkWidget *new_tab_page_build_closed_tab(const WigSessionTab *tab, guint index)
{
  g_autofree char *site = new_tab_page_site_name(tab->uri);
  GtkWidget *icon = NULL;
  GtkWidget *button = new_tab_page_build_card("web-browser-symbolic", tab->title && *tab->title ? tab->title : site,
                                              site, &icon);

#if HAVE_FAVICON_SUPPORT
  new_tab_page_load_favicon(icon, tab->uri);
#endif

  gtk_widget_set_tooltip_text(button, tab->uri);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.restore-closed");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(button), g_variant_new_uint32(index));

  return button;
}

static GtkWidget *new_tab_page_build_closed_window(const WigSessionWindow *window, guint index)
{
  guint n_tabs = g_slist_length(window->tabs);
  const WigSessionTab *first = window->tabs->data;
  g_autofree char *site = new_tab_page_site_name(first->uri);
  g_autofree char *count = g_strdup_printf("%u Tabs", n_tabs);
  GtkWidget *icon = NULL;
  GtkWidget *button = new_tab_page_build_card("window-new-symbolic", count, site, &icon);

  gtk_widget_set_tooltip_text(button, "Reopen this window");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.restore-closed");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(button), g_variant_new_uint32(index));

  return button;
}

static GtkWidget *new_tab_page_build_section(const char *title, GtkWidget **flow_box)
{
  GtkWidget *heading = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
  gtk_widget_add_css_class(heading, "heading");

  *flow_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(*flow_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(*flow_box), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(*flow_box), 1);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(*flow_box), NEW_TAB_PAGE_COLUMNS);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(*flow_box), 18);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(*flow_box), 18);

  GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_box_append(GTK_BOX(section), heading);
  gtk_box_append(GTK_BOX(section), *flow_box);

  return section;
}

static void new_tab_page_add_card(GtkWidget *flow_box, GtkWidget *card)
{
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box), card);
  gtk_widget_set_focusable(gtk_widget_get_parent(card), FALSE);
}

static GtkWidget *new_tab_page_wrap_card(GtkWidget *card)
{
  GtkWidget *child = gtk_flow_box_child_new();

  gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), card);
  gtk_widget_set_focusable(child, FALSE);

  return child;
}

static gboolean new_tab_page_favorite_matches(gpointer item, gpointer user_data)
{
  return !wig_bookmark_get_is_folder(item);
}

static GtkWidget *new_tab_page_create_favorite_card(gpointer item, gpointer user_data)
{
  WigNewTabPage *self = user_data;
  WigBookmark *bookmark = item;

  return new_tab_page_wrap_card(
      new_tab_page_build_site(self, wig_bookmark_get_url(bookmark), wig_bookmark_get_title(bookmark)));
}

static gboolean new_tab_page_frequent_matches(gpointer item, gpointer user_data)
{
  WigNewTabPage *self = user_data;

  return !g_hash_table_contains(self->shown_urls, wig_history_item_get_url(item));
}

static GtkWidget *new_tab_page_create_frequent_card(gpointer item, gpointer user_data)
{
  WigNewTabPage *self = user_data;
  WigHistoryItem *history = item;

  return new_tab_page_wrap_card(
      new_tab_page_build_site(self, wig_history_item_get_url(history), wig_history_item_get_title(history)));
}

/* Favourites are kept by hand, so all of them are shown rather than the handful
 * the other sections are trimmed to. */
static guint wig_new_tab_page_load_favorites(WigNewTabPage *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = NULL;

  g_list_store_remove_all(self->favorite_items);
  g_hash_table_remove_all(self->shown_urls);

  if (self->bookmarks && g_settings_get_boolean(self->settings, "show-favorites"))
    items = wig_bookmarks_store_get_children(self->bookmarks, WIG_BOOKMARKS_ROOT_FAVORITES, &error);

  if (error)
    g_warning("new-tab: favourites query failed: %s", error->message);

  for (guint i = 0; items && i < items->len; i++)
    g_list_store_append(self->favorite_items, g_ptr_array_index(items, i));

  guint shown = g_list_model_get_n_items(self->favorite_bookmarks);
  for (guint i = 0; i < shown; i++) {
    g_autoptr(WigBookmark) bookmark = g_list_model_get_item(self->favorite_bookmarks, i);
    g_hash_table_add(self->shown_urls, g_strdup(wig_bookmark_get_url(bookmark)));
  }

  return shown;
}
static guint wig_new_tab_page_load_frequent(WigNewTabPage *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = NULL;

  g_list_store_remove_all(self->frequent_items);

  if (self->store && g_settings_get_boolean(self->settings, "show-frequently-visited"))
    items = wig_history_store_query_most_typed(self->store,
                                               NEW_TAB_PAGE_SITE_LIMIT + g_hash_table_size(self->shown_urls), &error);

  if (error)
    g_warning("new-tab: query failed: %s", error->message);

  for (guint i = 0; items && i < items->len; i++)
    g_list_store_append(self->frequent_items, g_ptr_array_index(items, i));

  gtk_filter_changed(self->frequent_filter, GTK_FILTER_CHANGE_DIFFERENT);

  return g_list_model_get_n_items(self->frequent_shown);
}

static guint wig_new_tab_page_load_closed(WigNewTabPage *self)
{
  g_autoptr(GPtrArray) windows = NULL;
  guint shown = 0;

  gtk_flow_box_remove_all(GTK_FLOW_BOX(self->closed));

  if (self->session && g_settings_get_boolean(self->settings, "show-recently-closed"))
    windows = wig_session_list_closed_windows(self->session);

  for (guint i = 0; windows && i < windows->len && shown < NEW_TAB_PAGE_SITE_LIMIT; i++) {
    const WigSessionWindow *window = g_ptr_array_index(windows, i);
    const WigSessionTab *tab = window->tabs ? window->tabs->data : NULL;

    if (!tab)
      continue;

    if (window->tabs->next) {
      new_tab_page_add_card(self->closed, new_tab_page_build_closed_window(window, i));
    } else {
      if (!tab->uri || !*tab->uri || uri_is_new_tab_page(tab->uri))
        continue;

      new_tab_page_add_card(self->closed, new_tab_page_build_closed_tab(tab, i));
    }

    shown++;
  }

  return shown;
}

static void wig_new_tab_page_load(WigNewTabPage *self)
{
  guint favorites = wig_new_tab_page_load_favorites(self);
  guint frequent = wig_new_tab_page_load_frequent(self);
  guint closed = wig_new_tab_page_load_closed(self);
  gboolean anything_enabled = g_settings_get_boolean(self->settings, "show-favorites")
      || g_settings_get_boolean(self->settings, "show-frequently-visited")
      || g_settings_get_boolean(self->settings, "show-recently-closed");

  gtk_widget_set_visible(self->favorites_section, favorites > 0);
  gtk_widget_set_visible(self->frequent_section, frequent > 0);
  gtk_widget_set_visible(self->closed_section, closed > 0);

  g_debug("new-tab: showing %u favourite(s), %u frequently visited site(s) and %u recently closed item(s)", favorites,
          frequent, closed);

  if (favorites > 0 || frequent > 0 || closed > 0)
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "sites");
  else
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), anything_enabled ? "empty" : "off");
}

static void wig_new_tab_page_map(GtkWidget *widget)
{
  wig_new_tab_page_load(WIG_NEW_TAB_PAGE(widget));

  GTK_WIDGET_CLASS(wig_new_tab_page_parent_class)->map(widget);
}

static void wig_new_tab_page_dispose(GObject *object)
{
  WigNewTabPage *self = WIG_NEW_TAB_PAGE(object);

  g_clear_pointer(&self->overlay, gtk_widget_unparent);
  g_clear_pointer(&self->shown_urls, g_hash_table_unref);
  g_clear_object(&self->favorite_items);
  g_clear_object(&self->favorite_bookmarks);
  g_clear_object(&self->frequent_items);
  g_clear_object(&self->frequent_filter);
  g_clear_object(&self->frequent_shown);
  self->stack = NULL;
  self->sites = NULL;

  G_OBJECT_CLASS(wig_new_tab_page_parent_class)->dispose(object);
}

static void wig_new_tab_page_class_init(WigNewTabPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_new_tab_page_dispose;
  widget_class->map = wig_new_tab_page_map;

  signals[OPEN_URI_SIGNAL] = g_signal_new("open-uri", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  gtk_widget_class_set_css_name(widget_class, "wig-new-tab-page");
}

static void wig_new_tab_page_init(WigNewTabPage *self)
{
  self->store = wig_application_get_history_store(wig_application_get());
  self->bookmarks = wig_application_get_bookmarks_store(wig_application_get());
  self->session = wig_application_get_session(wig_application_get());
  self->settings = wig_application_get_settings(wig_application_get());

  self->shown_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  self->favorites_section = new_tab_page_build_section("Favorites", &self->favorites);
  self->frequent_section = new_tab_page_build_section("Frequently Visited", &self->sites);
  self->closed_section = new_tab_page_build_section("Recently Closed", &self->closed);

  self->favorite_items = g_list_store_new(WIG_TYPE_BOOKMARK);
  self->favorite_bookmarks = G_LIST_MODEL(
      gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(self->favorite_items)),
                                GTK_FILTER(gtk_custom_filter_new(new_tab_page_favorite_matches, NULL, NULL))));

  gtk_flow_box_bind_model(GTK_FLOW_BOX(self->favorites), self->favorite_bookmarks, new_tab_page_create_favorite_card,
                          self, NULL);

  self->frequent_items = g_list_store_new(WIG_TYPE_HISTORY_ITEM);
  self->frequent_filter = GTK_FILTER(gtk_custom_filter_new(new_tab_page_frequent_matches, self, NULL));

  GtkFilterListModel *frequent_filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(self->frequent_items)),
                                                                    g_object_ref(self->frequent_filter));
  self->frequent_shown = G_LIST_MODEL(
      gtk_slice_list_model_new(G_LIST_MODEL(frequent_filtered), 0, NEW_TAB_PAGE_SITE_LIMIT));

  gtk_flow_box_bind_model(GTK_FLOW_BOX(self->sites), self->frequent_shown, new_tab_page_create_frequent_card, self,
                          NULL);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 36);
  gtk_widget_set_valign(content, GTK_ALIGN_START);
  gtk_widget_set_margin_top(content, 48);
  gtk_widget_set_margin_bottom(content, 48);
  gtk_widget_set_margin_start(content, 24);
  gtk_widget_set_margin_end(content, 24);
  gtk_box_append(GTK_BOX(content), self->favorites_section);
  gtk_box_append(GTK_BOX(content), self->frequent_section);
  gtk_box_append(GTK_BOX(content), self->closed_section);

  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), content);
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), NEW_TAB_PAGE_WIDTH);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), NEW_TAB_PAGE_TIGHTENING);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "user-bookmarks-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Nothing to Show Yet");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Addresses you type and tabs you close are listed here.");

  self->stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->stack), scroller, "sites");
  gtk_stack_add_named(GTK_STACK(self->stack), empty, "empty");
  gtk_stack_add_named(GTK_STACK(self->stack), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "off");

  g_autoptr(GSimpleActionGroup) actions = g_simple_action_group_new();
  g_autoptr(GAction) show_favorites = g_settings_create_action(self->settings, "show-favorites");
  g_autoptr(GAction) show_sites = g_settings_create_action(self->settings, "show-frequently-visited");
  g_autoptr(GAction) show_closed = g_settings_create_action(self->settings, "show-recently-closed");
  g_action_map_add_action(G_ACTION_MAP(actions), show_favorites);
  g_action_map_add_action(G_ACTION_MAP(actions), show_sites);
  g_action_map_add_action(G_ACTION_MAP(actions), show_closed);
  gtk_widget_insert_action_group(GTK_WIDGET(self), "new-tab", G_ACTION_GROUP(actions));

  g_autoptr(GMenu) menu = g_menu_new();
  g_menu_append(menu, "Favorites", "new-tab.show-favorites");
  g_menu_append(menu, "Frequently Visited", "new-tab.show-frequently-visited");
  g_menu_append(menu, "Recently Closed", "new-tab.show-recently-closed");

  GtkWidget *gear = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(gear), "emblem-system-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(gear), G_MENU_MODEL(menu));
  gtk_menu_button_set_direction(GTK_MENU_BUTTON(gear), GTK_ARROW_UP);
  gtk_widget_set_tooltip_text(gear, "New Tab Page Options");
  gtk_widget_add_css_class(gear, "flat");
  gtk_widget_add_css_class(gear, "circular");
  gtk_widget_set_halign(gear, GTK_ALIGN_END);
  gtk_widget_set_valign(gear, GTK_ALIGN_END);
  gtk_widget_set_margin_end(gear, 12);
  gtk_widget_set_margin_bottom(gear, 12);

  self->overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(self->overlay), self->stack);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), gear);
  gtk_widget_set_parent(self->overlay, GTK_WIDGET(self));

  g_signal_connect_object(self->settings, "changed::show-favorites", G_CALLBACK(wig_new_tab_page_load), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->settings, "changed::show-frequently-visited", G_CALLBACK(wig_new_tab_page_load), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->settings, "changed::show-recently-closed", G_CALLBACK(wig_new_tab_page_load), self,
                          G_CONNECT_SWAPPED);

  if (self->session)
    g_signal_connect_object(self->session, "closed-changed", G_CALLBACK(wig_new_tab_page_load), self,
                            G_CONNECT_SWAPPED);

  if (self->bookmarks)
    g_signal_connect_object(self->bookmarks, "changed", G_CALLBACK(wig_new_tab_page_load), self, G_CONNECT_SWAPPED);

  wig_native_page_set_title(WIG_NATIVE_PAGE(self), WIG_NEW_TAB_PAGE_TITLE);
}

GtkWidget *wig_new_tab_page_new(const char *uri)
{
  return g_object_new(WIG_TYPE_NEW_TAB_PAGE, "uri", uri, NULL);
}
