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
  GSettings *settings;

  GtkWidget *overlay;
  GtkWidget *stack;
  GtkWidget *sites;
  GtkWidget *sites_section;
  GtkWidget *favorites;
  GtkWidget *favorites_section;
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

static GtkWidget *new_tab_page_build_site(WigNewTabPage *self, const char *url, const char *title)
{
  g_autofree char *site = new_tab_page_site_name(url);

  GtkWidget *image = gtk_image_new_from_icon_name("web-browser-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(image), NEW_TAB_PAGE_FAVICON_SIZE);

#if HAVE_FAVICON_SUPPORT
  new_tab_page_load_favicon(image, url);
#endif

  GtkWidget *name = gtk_label_new(title && *title ? title : site);
  gtk_label_set_xalign(GTK_LABEL(name), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(name), NEW_TAB_PAGE_LABEL_CHARS);
  gtk_widget_add_css_class(name, "site-name");

  GtkWidget *host = gtk_label_new(site);
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

static GtkWidget *new_tab_page_build_grid(void)
{
  GtkWidget *grid = gtk_flow_box_new();

  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(grid), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 1);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), NEW_TAB_PAGE_COLUMNS);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 18);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 18);

  return grid;
}

static GtkWidget *new_tab_page_build_section(const char *title, GtkWidget *grid)
{
  GtkWidget *heading = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
  gtk_widget_add_css_class(heading, "heading");

  GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_box_append(GTK_BOX(section), heading);
  gtk_box_append(GTK_BOX(section), grid);

  return section;
}

static void new_tab_page_append_site(WigNewTabPage *self, GtkWidget *box, const char *url, const char *title)
{
  GtkWidget *site = new_tab_page_build_site(self, url, title);

  gtk_flow_box_append(GTK_FLOW_BOX(box), site);
  gtk_widget_set_focusable(gtk_widget_get_parent(site), FALSE);
}

/* Favourites are kept by hand, so all of them are shown rather than the handful
 * the frequently visited list is trimmed to. */
static guint wig_new_tab_page_load_favorites(WigNewTabPage *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = NULL;
  guint shown = 0;

  gtk_flow_box_remove_all(GTK_FLOW_BOX(self->favorites));

  if (self->bookmarks && g_settings_get_boolean(self->settings, "show-favorites"))
    items = wig_bookmarks_store_get_children(self->bookmarks, WIG_BOOKMARKS_ROOT_FAVORITES, &error);

  if (error)
    g_warning("new-tab: favourites query failed: %s", error->message);

  for (guint i = 0; items && i < items->len; i++) {
    WigBookmark *bookmark = g_ptr_array_index(items, i);
    if (wig_bookmark_get_is_folder(bookmark))
      continue;

    new_tab_page_append_site(self, self->favorites, wig_bookmark_get_url(bookmark), wig_bookmark_get_title(bookmark));
    shown++;
  }

  return shown;
}

static void wig_new_tab_page_load(WigNewTabPage *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = NULL;
  guint n_favorites = wig_new_tab_page_load_favorites(self);
  gboolean show_sites = g_settings_get_boolean(self->settings, "show-frequently-visited");

  gtk_flow_box_remove_all(GTK_FLOW_BOX(self->sites));

  if (show_sites && self->store)
    items = wig_history_store_query_most_typed(self->store, NEW_TAB_PAGE_SITE_LIMIT, &error);

  if (error)
    g_warning("new-tab: query failed: %s", error->message);

  for (guint i = 0; items && i < items->len; i++) {
    WigHistoryItem *item = g_ptr_array_index(items, i);
    new_tab_page_append_site(self, self->sites, wig_history_item_get_url(item), wig_history_item_get_title(item));
  }

  guint n_sites = items ? items->len : 0;

  gtk_widget_set_visible(self->favorites_section, n_favorites > 0);
  gtk_widget_set_visible(self->sites_section, n_sites > 0);

  g_debug("new-tab: showing %u favourite(s) and %u frequently visited site(s)%s", n_favorites, n_sites,
          show_sites ? "" : ", frequently visited is turned off");

  if (n_favorites > 0 || n_sites > 0)
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "sites");
  else
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), show_sites ? "empty" : "off");
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
  self->stack = NULL;
  self->sites = NULL;
  self->sites_section = NULL;
  self->favorites = NULL;
  self->favorites_section = NULL;

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
  self->settings = wig_application_get_settings(wig_application_get());

  if (self->bookmarks)
    g_signal_connect_object(self->bookmarks, "changed", G_CALLBACK(wig_new_tab_page_load), self, G_CONNECT_SWAPPED);

  self->favorites = new_tab_page_build_grid();
  self->favorites_section = new_tab_page_build_section("Favorites", self->favorites);

  self->sites = new_tab_page_build_grid();
  self->sites_section = new_tab_page_build_section("Frequently Visited", self->sites);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 30);
  gtk_widget_set_valign(content, GTK_ALIGN_START);
  gtk_widget_set_margin_top(content, 48);
  gtk_widget_set_margin_bottom(content, 48);
  gtk_widget_set_margin_start(content, 24);
  gtk_widget_set_margin_end(content, 24);
  gtk_box_append(GTK_BOX(content), self->favorites_section);
  gtk_box_append(GTK_BOX(content), self->sites_section);

  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), content);
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), NEW_TAB_PAGE_WIDTH);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), NEW_TAB_PAGE_TIGHTENING);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "user-bookmarks-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Nothing Frequently Visited");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Addresses you type are listed here.");

  self->stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->stack), scroller, "sites");
  gtk_stack_add_named(GTK_STACK(self->stack), empty, "empty");
  gtk_stack_add_named(GTK_STACK(self->stack), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "off");

  g_autoptr(GSimpleActionGroup) actions = g_simple_action_group_new();
  g_autoptr(GAction) show_favorites = g_settings_create_action(self->settings, "show-favorites");
  g_action_map_add_action(G_ACTION_MAP(actions), show_favorites);
  g_autoptr(GAction) show_sites = g_settings_create_action(self->settings, "show-frequently-visited");
  g_action_map_add_action(G_ACTION_MAP(actions), show_sites);
  gtk_widget_insert_action_group(GTK_WIDGET(self), "new-tab", G_ACTION_GROUP(actions));

  g_autoptr(GMenu) menu = g_menu_new();
  g_menu_append(menu, "Favorites", "new-tab.show-favorites");
  g_menu_append(menu, "Frequently Visited", "new-tab.show-frequently-visited");

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

  wig_native_page_set_title(WIG_NATIVE_PAGE(self), WIG_NEW_TAB_PAGE_TITLE);
}

GtkWidget *wig_new_tab_page_new(const char *uri)
{
  return g_object_new(WIG_TYPE_NEW_TAB_PAGE, "uri", uri, NULL);
}
