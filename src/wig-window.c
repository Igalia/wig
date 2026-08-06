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

#include "wig-window.h"

#include "wig-application.h"
#include "wig-entry-completion-popover.h"
#include "wig-permissions-button.h"
#include "wig-search-bar.h"
#include "wig-tab-bar.h"
#include "wig-tab-list.h"
#include "wig-tab-sidebar.h"
#include "wig-tab.h"
#include "wig-utils.h"
#include "wpe-toplevel-gtk.h"
#include "wpe-view-gtk.h"

struct _WigWindow {
  GtkApplicationWindow parent;

  guint id;

  WPEToplevel *toplevel;
  GtkWidget *toolbar_view;
  GtkWidget *header_bar;
  GtkWidget *back_button;
  GtkWidget *forward_button;
  GtkWidget *stop_reload_button;
  GtkWidget *new_tab_button;
  GtkWidget *url_entry;
  GtkWidget *entry_completion_popover;
  GtkWidget *permissions_button;
  WigPermissionsManager *permissions_manager; /* borrowed from application */
  char *current_origin; /* origin string of current_web_view, or NULL */
  WigTabList *tab_list;
  GtkWidget *content_box;
  GtkWidget *paned;
  GtkWidget *tab_bar;
  GtkWidget *tab_separator;
  GtkWidget *tab_sidebar;
  GtkWidget *tab_stack;
  GtkWidget *search_bar;
  GtkWidget *overview_button;
  WebKitWebView *current_web_view;
  guint progress_timeout_id;
  gboolean suppress_entry_completion;
  gboolean url_entry_focused;
  GActionGroup *context_menu_action_group;
  GtkWidget *tab_view_context_menu;
  GtkWidget *back_history_popover;
  GtkWidget *forward_history_popover;
};

G_DEFINE_FINAL_TYPE(WigWindow, wig_window, GTK_TYPE_APPLICATION_WINDOW)

typedef enum {
  PROP_ID = 1,
} WigWindowProps;

static GParamSpec *props[PROP_ID + 1];

static guint wig_window_next_id = 1;

static void wig_window_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigWindow *win = WIG_WINDOW(object);
  switch ((WigWindowProps)prop_id) {
  case PROP_ID:
    g_value_set_uint(value, win->id);
    break;
  }
}

static void wig_window_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigWindow *win = WIG_WINDOW(object);
  switch ((WigWindowProps)prop_id) {
  case PROP_ID: {
    win->id = g_value_get_uint(value);
    break;
  }
  }
}

static void wig_window_go_back(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->current_web_view)
    return;

  webkit_web_view_go_back(win->current_web_view);
}

static void wig_window_go_forward(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->current_web_view)
    return;

  webkit_web_view_go_forward(win->current_web_view);
}

static void wig_window_stop_reload(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->current_web_view)
    return;

  GVariant *state = g_action_get_state(G_ACTION(action));
  if (g_variant_get_boolean(state))
    webkit_web_view_stop_loading(win->current_web_view);
  else
    webkit_web_view_reload(win->current_web_view);

  g_variant_unref(state);
}

static void wig_window_change_stop_reload_state(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  GVariant *state = g_variant_new_boolean(g_variant_get_boolean(parameter));

  gtk_button_set_icon_name(GTK_BUTTON(win->stop_reload_button),
                           g_variant_get_boolean(state) ? "process-stop-symbolic" : "view-refresh-symbolic");

  g_simple_action_set_state(G_SIMPLE_ACTION(action), state);
}

static WigTab *wig_window_get_tab_for_web_view(WigWindow *win, WebKitWebView *web_view)
{
  /* A web view can emit "close" while the window is tearing down, after the tab
   * list has already been cleared. */
  if (!win->tab_list)
    return NULL;

  guint n = wig_tab_list_get_n_tabs(win->tab_list);
  for (guint i = 0; i < n; i++) {
    WigTab *tab = wig_tab_list_get_nth(win->tab_list, i);
    if (wig_tab_get_web_view(tab) == web_view)
      return g_steal_pointer(&tab);
  }
  return NULL;
}

static void wig_window_save_tab_to_history(WigWindow *win, WebKitWebView *web_view)
{
  WebKitWebViewSessionState *state = webkit_web_view_get_session_state(web_view);
  if (!state)
    return;

  WigClosedTab *tab = g_new(WigClosedTab, 1);
  tab->state = g_steal_pointer(&state);
  tab->was_focused = (win->current_web_view == web_view);

  WigClosedGroup *group = g_new(WigClosedGroup, 1);
  group->window_id = win->id;
  group->tabs = g_slist_prepend(NULL, tab);
  wig_application_push_closed_group(WIG_APPLICATION(wig_application_get()), group);
}

static gboolean wig_window_tab_close(WigTabList *list, WigTab *tab, WigWindow *win)
{
  WebKitWebView *web_view = wig_tab_get_web_view(tab);
  wig_window_save_tab_to_history(win, web_view);

  if (wig_tab_list_get_n_tabs(list) == 1) {
    gtk_window_destroy(GTK_WINDOW(win));
    return TRUE;
  }

  return FALSE;
}

static void wig_window_close_tab(WigWindow *win, WebKitWebView *web_view)
{
  WigTab *tab = wig_window_get_tab_for_web_view(win, web_view);
  if (tab)
    wig_tab_list_close(win->tab_list, tab);
}

gboolean wig_window_focus_tab_by_site(WigWindow *win, const char *uri)
{
  g_autoptr(GUri) lookup = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  if (!lookup)
    return FALSE;

  const char *lookup_scheme = g_uri_get_scheme(lookup);
  const char *lookup_host = g_uri_get_host(lookup);
  const char *lookup_path = g_uri_get_path(lookup);

  guint n = wig_tab_list_get_n_tabs(win->tab_list);
  for (guint i = 0; i < n; i++) {
    WigTab *tab = wig_tab_list_get_nth(win->tab_list, i);
    WebKitWebView *web_view = wig_tab_get_web_view(tab);
    const char *tab_uri = webkit_web_view_get_uri(web_view);
    if (!tab_uri)
      continue;

    g_autoptr(GUri) parsed = g_uri_parse(tab_uri, G_URI_FLAGS_NONE, NULL);
    if (!parsed)
      continue;

    const char *tab_scheme = g_uri_get_scheme(parsed);
    const char *tab_host = g_uri_get_host(parsed);

    gboolean match;
    if (lookup_host && *lookup_host)
      match = g_str_equal(lookup_scheme, tab_scheme) && tab_host && g_ascii_strcasecmp(lookup_host, tab_host) == 0;
    else
      match = g_str_equal(lookup_scheme, tab_scheme) && g_strcmp0(lookup_path, g_uri_get_path(parsed)) == 0;

    if (match) {
      wig_tab_list_set_active(win->tab_list, tab);
      webkit_web_view_reload(web_view);
      return TRUE;
    }
  }
  return FALSE;
}

static void wig_window_on_webkit_notification_closed(WebKitNotification *webkit_notif, char *notif_id)
{
  WigApplication *app = wig_application_get();
  g_application_withdraw_notification(G_APPLICATION(app), notif_id);
  wig_application_untrack_notification(app, notif_id);
}

static void free_with_closure(void *data, GClosure *closure)
{
  g_free(data);
}

static gboolean wig_window_on_show_notification(WigWindow *win, WebKitNotification *webkit_notif)
{
  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));

  const char *title = webkit_notification_get_title(webkit_notif);
  g_autoptr(GNotification) notif = g_notification_new(title && *title ? title : "Notification");

  const char *body = webkit_notification_get_body(webkit_notif);
  if (body && *body)
    g_notification_set_body(notif, body);

  g_autofree char *notif_id = g_strdup_printf("wig-%" G_GUINT64_FORMAT, webkit_notification_get_id(webkit_notif));
  g_notification_set_default_action_and_target(notif, "app.notification-clicked", "s", notif_id);

  wig_application_track_notification(app, notif_id, webkit_notif);
  g_application_send_notification(G_APPLICATION(app), notif_id, notif);

  g_signal_connect_data(webkit_notif, "closed", G_CALLBACK(wig_window_on_webkit_notification_closed),
                        g_steal_pointer(&notif_id), free_with_closure, G_CONNECT_DEFAULT);

  return TRUE;
}

static char *wig_window_current_origin(WigWindow *win)
{
  if (!win->current_web_view)
    return NULL;

  const char *uri = webkit_web_view_get_uri(win->current_web_view);
  if (!uri || !*uri)
    return NULL;

  g_autoptr(WebKitSecurityOrigin) origin = webkit_security_origin_new_for_uri(uri);
  return webkit_security_origin_to_string(origin);
}

static void wig_window_update_permissions(WigWindow *win)
{
  g_clear_pointer(&win->current_origin, g_free);
  win->current_origin = wig_window_current_origin(win);

  WigPermissions *permissions = NULL;
  if (win->current_origin)
    permissions = wig_permissions_manager_lookup(win->permissions_manager, win->current_origin);

  wig_permissions_button_set_permissions(WIG_PERMISSIONS_BUTTON(win->permissions_button), permissions);
}

static void wig_window_on_permissions_changed(WigWindow *win, const char *origin)
{
  if (g_strcmp0(origin, win->current_origin) != 0)
    return;

  WigPermissions *permissions = wig_permissions_manager_lookup(win->permissions_manager, origin);
  wig_permissions_button_set_permissions(WIG_PERMISSIONS_BUTTON(win->permissions_button), permissions);
}

static gboolean wig_window_on_permission_request(WebKitWebView *web_view, WebKitPermissionRequest *request,
                                                 WigWindow *win)
{
  /* WebKit runs the whole desktop portal handshake while it validates the
   * constraints, so by the time a screen sharing request reaches us the user has
   * already picked what to share in the portal's own picker. Prompting again
   * would only ask about capture that is set up already, so take the portal as
   * the answer. Capture still cannot start without the PipeWire fd it holds. */
  if (wig_permission_request_is_display_capture(request)) {
    g_debug("allowing screen sharing request, already arranged by the portal");
    webkit_permission_request_allow(request);
    return TRUE;
  }

  if (wig_permission_kinds_for_request(request) == 0)
    return FALSE;

  const char *uri = webkit_web_view_get_uri(web_view);
  if (!uri || !*uri)
    return FALSE;

  g_autoptr(WebKitSecurityOrigin) origin = webkit_security_origin_new_for_uri(uri);
  g_autofree char *origin_str = webkit_security_origin_to_string(origin);
  if (!origin_str)
    return FALSE;

  wig_permissions_manager_handle_request(win->permissions_manager, origin_str, request,
                                         WIG_PERMISSIONS_BUTTON(win->permissions_button));
  return TRUE;
}

static void wig_window_file_chooser_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WebKitFileChooserRequest) request = user_data;
  g_autoptr(GError) error = NULL;

  if (webkit_file_chooser_request_get_select_multiple(request)) {
    g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, &error);
    if (files) {
      guint n = g_list_model_get_n_items(files);
      g_auto(GStrv) paths = g_new0(char *, n + 1);
      for (guint i = 0; i < n; i++) {
        g_autoptr(GFile) file = g_list_model_get_item(files, i);
        paths[i] = g_file_get_path(file);
      }
      webkit_file_chooser_request_select_files(request, (const char *const *)paths);
    } else {
      g_debug("file-chooser failed: %s", error->message);
      webkit_file_chooser_request_cancel(request);
    }
  } else {
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (file) {
      const char *paths[] = { g_file_peek_path(file), NULL };
      webkit_file_chooser_request_select_files(request, paths);
    } else {
      g_debug("file-chooser failed: %s", error->message);
      webkit_file_chooser_request_cancel(request);
    }
  }
}

static gboolean wig_window_on_run_file_chooser(WigWindow *win, WebKitFileChooserRequest *request)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();

  const char *const *mime_types = webkit_file_chooser_request_get_mime_types(request);
  if (mime_types && *mime_types) {
    g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Supported Files");
    for (guint i = 0; mime_types[i]; i++) {
      gtk_file_filter_add_mime_type(filter, mime_types[i]);
    }

    g_autoptr(GtkFileFilter) all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Files");
    gtk_file_filter_add_pattern(all_filter, "*");

    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_list_store_append(filters, all_filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
  }

  if (webkit_file_chooser_request_get_select_multiple(request))
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(win), NULL, wig_window_file_chooser_done, g_object_ref(request));
  else
    gtk_file_dialog_open(dialog, GTK_WINDOW(win), NULL, wig_window_file_chooser_done, g_object_ref(request));

  return TRUE;
}

static gboolean wig_window_decide_policy(WigWindow *win, WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType decision_type, WebKitWebView *web_view);
static WebKitWebView *wig_window_web_view_create(WigWindow *win, WebKitNavigationAction *navigation,
                                                 WebKitWebView *opener);
static gboolean wig_window_web_view_context_menu(WigWindow *win, WebKitContextMenu *context_menu,
                                                 WebKitHitTestResult *hit_test_result);

static WigTab *wig_window_add_tab_for_view(WigWindow *win, WebKitWebView *web_view)
{
  WigTab *tab = wig_tab_list_append(win->tab_list, web_view);

  g_signal_connect_object(web_view, "close", G_CALLBACK(wig_window_close_tab), win, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "show-notification", G_CALLBACK(wig_window_on_show_notification), win,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "permission-request", G_CALLBACK(wig_window_on_permission_request), win,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(web_view, "run-file-chooser", G_CALLBACK(wig_window_on_run_file_chooser), win,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "decide-policy", G_CALLBACK(wig_window_decide_policy), win, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "create", G_CALLBACK(wig_window_web_view_create), win, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "context-menu", G_CALLBACK(wig_window_web_view_context_menu), win,
                          G_CONNECT_SWAPPED);

  wpe_view_set_toplevel(webkit_web_view_get_wpe_view(web_view), win->toplevel);

  return tab;
}

static WebKitWebView *wig_window_create_web_view_for_new_tab(WigWindow *win)
{
  WebKitWebView *view = wig_application_create_web_view(wig_application_get());
  webkit_web_view_load_uri(view, "about:blank");
  return view;
}

static void wig_window_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  g_autoptr(WebKitWebView) web_view = wig_window_create_web_view_for_new_tab(win);
  WigTab *tab = wig_window_add_tab_for_view(win, web_view);
  wig_tab_list_set_active(win->tab_list, tab);
  gtk_widget_grab_focus(win->url_entry);
}

static void wig_window_focus_entry(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  gtk_widget_grab_focus(win->url_entry);
  gtk_editable_select_region(GTK_EDITABLE(win->url_entry), 0, -1);
}

static void wig_window_find(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->search_bar)
    return;

  WigTab *tab = win->tab_list ? wig_tab_list_get_active(win->tab_list) : NULL;
  wig_search_bar_set_tab(WIG_SEARCH_BAR(win->search_bar), tab);
  wig_search_bar_open(WIG_SEARCH_BAR(win->search_bar));
}

static void wig_window_find_next(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (win->search_bar && wig_search_bar_is_open(WIG_SEARCH_BAR(win->search_bar)))
    wig_search_bar_find_next(WIG_SEARCH_BAR(win->search_bar));
}

static void wig_window_find_previous(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (win->search_bar && wig_search_bar_is_open(WIG_SEARCH_BAR(win->search_bar)))
    wig_search_bar_find_previous(WIG_SEARCH_BAR(win->search_bar));
}

/* The find bar took the keyboard away from the page; hand it back. */
static void wig_window_search_bar_closed(WigSearchBar *search_bar, WigWindow *win)
{
  WigTab *tab = win->tab_list ? wig_tab_list_get_active(win->tab_list) : NULL;
  if (tab)
    gtk_widget_grab_focus(wig_tab_get_widget(tab));
}

static void wig_window_show_downloads(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_open_internal_page(wig_application_get(), GTK_WINDOW(user_data), "wig:downloads");
}

static void wig_window_show_history(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_open_internal_page(wig_application_get(), GTK_WINDOW(user_data), "wig:history");
}

static void wig_window_close_tab_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (win->current_web_view)
    wig_window_close_tab(win, win->current_web_view);
}

static void wig_window_reload(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->current_web_view)
    return;

  webkit_web_view_reload(win->current_web_view);
}

static void wig_window_reload_bypass_cache(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->current_web_view)
    return;

  webkit_web_view_reload_bypass_cache(win->current_web_view);
}

static void wig_window_toggle_fullscreen(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (gtk_window_is_fullscreen(GTK_WINDOW(win)))
    gtk_window_unfullscreen(GTK_WINDOW(win));
  else
    gtk_window_fullscreen(GTK_WINDOW(win));
}

static WigWindow *get_window_by_id(WigApplication *app, guint id)
{
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
  for (GList *l = windows; l; l = g_list_next(l)) {
    WigWindow *window = WIG_WINDOW(l->data);
    if (window->id == id)
      return window;
  }

  return NULL;
}

static void wig_window_undo_close_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WigApplication *app = wig_application_get();

  WigClosedGroup *group = wig_application_pop_closed_group(app);
  if (!group)
    return;

  WigWindow *target_win = get_window_by_id(app, group->window_id);
  if (!target_win)
    target_win = g_object_new(WIG_TYPE_WINDOW, "id", group->window_id, "application", app, NULL);

  WigTab *focused_tab = NULL;
  for (GSList *l = group->tabs; l; l = g_slist_next(l)) {
    WigClosedTab *closed_tab = l->data;
    g_autoptr(WebKitWebView) web_view = wig_application_create_web_view(app);
    webkit_web_view_restore_session_state(web_view, closed_tab->state);

    WebKitBackForwardList *bfl = webkit_web_view_get_back_forward_list(web_view);
    WebKitBackForwardListItem *item = webkit_back_forward_list_get_current_item(bfl);
    if (item)
      webkit_web_view_go_to_back_forward_list_item(web_view, item);

    WigTab *tab = wig_window_add_tab_for_view(target_win, web_view);
    if (closed_tab->was_focused)
      focused_tab = tab;
  }

  if (focused_tab)
    wig_tab_list_set_active(target_win->tab_list, focused_tab);

  if (target_win != win)
    gtk_window_present(GTK_WINDOW(target_win));

  wig_closed_group_free(group);
}

#if HAVE_FAVICON_SUPPORT
static void on_favicon_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  WebKitFaviconDatabase *db = WEBKIT_FAVICON_DATABASE(source);
  g_autoptr(GtkImage) image = GTK_IMAGE(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(WebKitImageList) image_list = webkit_favicon_database_get_page_icons_finish(db, result, &error);
  GIcon *icon = wig_util_best_page_icon(image_list, WIG_TAB_FAVICON_SIZE);
  if (icon)
    gtk_image_set_from_gicon(image, icon);
}
#endif

static GtkWidget *wig_window_build_history_row(WebKitBackForwardListItem *item
#if HAVE_FAVICON_SUPPORT
                                               ,
                                               WebKitFaviconDatabase *favicon_db
#endif
)
{
  const char *title = webkit_back_forward_list_item_get_title(item);
  const char *uri = webkit_back_forward_list_item_get_uri(item);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

#if HAVE_FAVICON_SUPPORT
  GtkWidget *image = gtk_image_new();
  gtk_image_set_icon_size(GTK_IMAGE(image), GTK_ICON_SIZE_NORMAL);
  gtk_box_append(GTK_BOX(box), image);

  if (favicon_db && uri)
    webkit_favicon_database_get_page_icons(favicon_db, uri, NULL, on_favicon_ready, g_object_ref(image));
#endif

  GtkWidget *label = gtk_label_new(title && *title ? title : uri);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);

  return box;
}

static GtkWidget *wig_window_build_history_popover(WigWindow *win, WebKitBackForwardListItem *current, GList *items)
{
#if HAVE_FAVICON_SUPPORT
  WigApplication *app = wig_application_get();
  WebKitNetworkSession *session = wig_application_get_network_session(app);
  WebKitWebsiteDataManager *data_manager = webkit_network_session_get_website_data_manager(session);
  WebKitFaviconDatabase *favicon_db = webkit_website_data_manager_get_favicon_database(data_manager);
#endif

  GtkWidget *list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
  gtk_widget_set_size_request(list_box, 240, -1);

  GtkWidget *current_row_widget = wig_window_build_history_row(current
#if HAVE_FAVICON_SUPPORT
                                                               ,
                                                               favicon_db
#endif
  );
  gtk_list_box_append(GTK_LIST_BOX(list_box), current_row_widget);
  GtkListBoxRow *current_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_box), 0);
  gtk_widget_add_css_class(GTK_WIDGET(current_row), "current-history-item");

  for (GList *l = items; l; l = l->next) {
    GtkWidget *row = wig_window_build_history_row(WEBKIT_BACK_FORWARD_LIST_ITEM(l->data)
#if HAVE_FAVICON_SUPPORT
                                                      ,
                                                  favicon_db
#endif
    );

    gtk_list_box_append(GTK_LIST_BOX(list_box), row);
  }

  GtkWidget *popover = gtk_popover_new();
  gtk_widget_add_css_class(popover, "back-history-popover");
  gtk_popover_set_child(GTK_POPOVER(popover), list_box);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  return popover;
}

static void wig_window_back_history_row_activated(GtkListBox *list_box, GtkListBoxRow *row, WigWindow *win)
{
  int index = gtk_list_box_row_get_index(row);
  g_clear_pointer(&win->back_history_popover, gtk_widget_unparent);

  /* Row 0 is the current item — nothing to do. */
  if (index == 0 || !win->current_web_view)
    return;

  WebKitBackForwardList *bfl = webkit_web_view_get_back_forward_list(win->current_web_view);
  WebKitBackForwardListItem *item = webkit_back_forward_list_get_nth_item(bfl, -index);
  if (item)
    webkit_web_view_go_to_back_forward_list_item(win->current_web_view, item);
}

static void wig_window_back_button_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                                 WigWindow *win)
{
  if (!win->current_web_view)
    return;

  WebKitBackForwardList *bfl = webkit_web_view_get_back_forward_list(win->current_web_view);
  g_autoptr(GList) back_list = webkit_back_forward_list_get_back_list(bfl);
  if (!back_list)
    return;

  WebKitBackForwardListItem *current = webkit_back_forward_list_get_current_item(bfl);
  g_clear_pointer(&win->back_history_popover, gtk_widget_unparent);

  win->back_history_popover = wig_window_build_history_popover(win, current, back_list);
  GtkWidget *list_box = gtk_popover_get_child(GTK_POPOVER(win->back_history_popover));
  g_signal_connect_object(list_box, "row-activated", G_CALLBACK(wig_window_back_history_row_activated), win,
                          G_CONNECT_DEFAULT);
  gtk_widget_set_parent(win->back_history_popover, win->back_button);
  gtk_popover_popup(GTK_POPOVER(win->back_history_popover));
}

static void wig_window_forward_history_row_activated(GtkListBox *list_box, GtkListBoxRow *row, WigWindow *win)
{
  int index = gtk_list_box_row_get_index(row);
  g_clear_pointer(&win->forward_history_popover, gtk_widget_unparent);

  /* Row 0 is the current item — nothing to do. */
  if (index == 0 || !win->current_web_view)
    return;

  WebKitBackForwardList *bfl = webkit_web_view_get_back_forward_list(win->current_web_view);
  WebKitBackForwardListItem *item = webkit_back_forward_list_get_nth_item(bfl, index);
  if (item)
    webkit_web_view_go_to_back_forward_list_item(win->current_web_view, item);
}

static void wig_window_forward_button_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                                    WigWindow *win)
{
  if (!win->current_web_view)
    return;

  WebKitBackForwardList *bfl = webkit_web_view_get_back_forward_list(win->current_web_view);
  g_autoptr(GList) forward_list = webkit_back_forward_list_get_forward_list(bfl);
  if (!forward_list)
    return;

  WebKitBackForwardListItem *current = webkit_back_forward_list_get_current_item(bfl);
  g_clear_pointer(&win->forward_history_popover, gtk_widget_unparent);

  win->forward_history_popover = wig_window_build_history_popover(win, current, forward_list);
  GtkWidget *list_box = gtk_popover_get_child(GTK_POPOVER(win->forward_history_popover));
  g_signal_connect_object(list_box, "row-activated", G_CALLBACK(wig_window_forward_history_row_activated), win,
                          G_CONNECT_DEFAULT);
  gtk_widget_set_parent(win->forward_history_popover, win->forward_button);
  gtk_popover_popup(GTK_POPOVER(win->forward_history_popover));
}

static void wig_window_tab_view_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigWindow *win)
{
  gboolean is_sidebar = win->tab_sidebar != NULL;

  g_autoptr(GMenu) menu = g_menu_new();
  g_menu_append(menu, is_sidebar ? "Switch to Tab Bar" : "Switch to Sidebar",
                is_sidebar ? "win.switch-to-tabbar" : "win.switch-to-sidebar");

  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  g_clear_pointer(&win->tab_view_context_menu, gtk_widget_unparent);
  win->tab_view_context_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  gtk_widget_set_parent(win->tab_view_context_menu, widget);
  gtk_popover_set_has_arrow(GTK_POPOVER(win->tab_view_context_menu), FALSE);
  GdkRectangle rect = { (int)x, (int)y, 1, 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(win->tab_view_context_menu), &rect);
  gtk_popover_popup(GTK_POPOVER(win->tab_view_context_menu));
}

static void wig_window_add_tab_view_context_menu(WigWindow *win, GtkWidget *widget)
{
  g_autoptr(GtkGestureClick) gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
  g_signal_connect_object(gesture, "pressed", G_CALLBACK(wig_window_tab_view_right_pressed), win, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(g_steal_pointer(&gesture)));
}

static void wig_window_switch_to_sidebar(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);

  g_clear_pointer(&win->tab_view_context_menu, gtk_widget_unparent);
  g_clear_pointer(&win->tab_bar, gtk_widget_unparent);
  g_clear_pointer(&win->tab_separator, gtk_widget_unparent);

  win->tab_sidebar = wig_tab_sidebar_new(win->tab_list);
  wig_window_add_tab_view_context_menu(win, win->tab_sidebar);
  gtk_paned_set_start_child(GTK_PANED(win->paned), win->tab_sidebar);
}

static void wig_window_switch_to_tabbar(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);

  g_clear_pointer(&win->tab_view_context_menu, gtk_widget_unparent);
  // FIXME: Error finding last focus widget of GtkPaned 0x55daa38feb70, gtk_paned_set_focus_child was called on widget
  // (nil) which is not child of ...
  gtk_paned_set_start_child(GTK_PANED(win->paned), NULL);
  win->tab_sidebar = NULL;

  win->tab_bar = wig_tab_bar_new(win->tab_list);
  wig_window_add_tab_view_context_menu(win, win->tab_bar);
  gtk_box_insert_child_after(GTK_BOX(win->content_box), win->tab_bar, NULL);

  win->tab_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_insert_child_after(GTK_BOX(win->content_box), win->tab_separator, win->tab_bar);
}

static void wig_window_zoom_in(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->current_web_view)
    return;

  webkit_web_view_set_zoom_level(win->current_web_view, webkit_web_view_get_zoom_level(win->current_web_view) + 0.1);
}

static void wig_window_zoom_out(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->current_web_view)
    return;

  webkit_web_view_set_zoom_level(win->current_web_view, webkit_web_view_get_zoom_level(win->current_web_view) - 0.1);
}

static void wig_window_zoom_reset(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  if (!win->current_web_view)
    return;

  webkit_web_view_set_zoom_level(win->current_web_view, 1.0);
}

static void wig_window_reload_middle_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigWindow *win)
{
  g_action_group_activate_action(G_ACTION_GROUP(win), "duplicate-active-tab", NULL);
}

static void wig_window_tab_duplicate(WigTabList *list, guint tab_id, WigWindow *win);

static void wig_window_duplicate_active_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WigTab *tab = wig_tab_list_get_active(win->tab_list);
  if (!tab)
    return;
  wig_window_tab_duplicate(win->tab_list, wig_tab_get_id(tab), win);
}

static const GActionEntry actions[] = {
  { "go-back", wig_window_go_back },
  { "go-forward", wig_window_go_forward },
  { "stop-reload", wig_window_stop_reload, NULL, "false", wig_window_change_stop_reload_state },
  { "new-tab", wig_window_new_tab },
  { "focus-entry", wig_window_focus_entry },
  { "find", wig_window_find },
  { "find-next", wig_window_find_next },
  { "find-previous", wig_window_find_previous },
  { "show-downloads", wig_window_show_downloads },
  { "show-history", wig_window_show_history },
  { "close-tab", wig_window_close_tab_action },
  { "reload", wig_window_reload },
  { "reload-bypass-cache", wig_window_reload_bypass_cache },
  { "toggle-fullscreen", wig_window_toggle_fullscreen },
  { "zoom-in", wig_window_zoom_in },
  { "zoom-out", wig_window_zoom_out },
  { "zoom-reset", wig_window_zoom_reset },
  { "undo-close-tab", wig_window_undo_close_tab },
  { "duplicate-active-tab", wig_window_duplicate_active_tab },
  { "switch-to-sidebar", wig_window_switch_to_sidebar },
  { "switch-to-tabbar", wig_window_switch_to_tabbar },
};

static void wig_window_open_in_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  g_autoptr(WebKitWebView) web_view = wig_window_create_web_view_for_new_tab(win);
  WigTab *tab = wig_window_add_tab_for_view(win, web_view);
  wig_tab_list_set_active(win->tab_list, tab);
  if (parameter) {
    const char *uri = g_variant_get_string(parameter, NULL);
    if (uri)
      webkit_web_view_load_uri(web_view, uri);
  }
}

static const GActionEntry context_menu_actions[] = {
  { "open-in-new-tab", wig_window_open_in_new_tab, "s" },
};

static void wig_window_tab_reload(WigTabList *list, guint tab_id, WigWindow *win)
{
  WigTab *tab = wig_tab_list_get_by_id(list, tab_id);
  if (tab)
    webkit_web_view_reload(wig_tab_get_web_view(tab));
}

static void wig_window_tab_mute(WigTabList *list, guint tab_id, WigWindow *win)
{
  WigTab *tab = wig_tab_list_get_by_id(list, tab_id);
  if (!tab)
    return;
  WebKitWebView *web_view = wig_tab_get_web_view(tab);
  webkit_web_view_set_is_muted(web_view, !webkit_web_view_get_is_muted(web_view));
}

static void wig_window_tab_duplicate(WigTabList *list, guint tab_id, WigWindow *win)
{
  WigTab *tab = wig_tab_list_get_by_id(list, tab_id);
  if (!tab)
    return;
  const char *uri = webkit_web_view_get_uri(wig_tab_get_web_view(tab));
  g_autoptr(WebKitWebView) web_view = wig_window_create_web_view_for_new_tab(win);
  WigTab *new_tab = wig_window_add_tab_for_view(win, web_view);
  if (uri)
    webkit_web_view_load_uri(web_view, uri);
  wig_tab_list_set_active(win->tab_list, new_tab);
}

static void wig_window_tab_copy_link(WigTabList *list, guint tab_id, WigWindow *win)
{
  WigTab *tab = wig_tab_list_get_by_id(list, tab_id);
  if (!tab)
    return;
  const char *uri = webkit_web_view_get_uri(wig_tab_get_web_view(tab));
  if (uri)
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(win)), uri);
}

static void wig_window_update_url(WigWindow *win)
{
  const char *url = win->current_web_view ? webkit_web_view_get_uri(win->current_web_view) : NULL;
  win->suppress_entry_completion = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(win->url_entry), url ? url : "");
  win->suppress_entry_completion = FALSE;
}

static void wig_window_clear_load_progress(WigWindow *win)
{
  gtk_entry_set_progress_fraction(GTK_ENTRY(win->url_entry), 0);
  g_clear_handle_id(&win->progress_timeout_id, g_source_remove);
}

static void wig_window_update_load_progress(WigWindow *win)
{
  gdouble progress = win->current_web_view ? webkit_web_view_get_estimated_load_progress(win->current_web_view) : 0;
  gtk_entry_set_progress_fraction(GTK_ENTRY(win->url_entry), progress);
  g_clear_handle_id(&win->progress_timeout_id, g_source_remove);
  if (progress == 1.0)
    win->progress_timeout_id = g_timeout_add_once(500, (GSourceOnceFunc)wig_window_clear_load_progress, win);
}

static void wig_window_load_uri(WigWindow *win, const char *uri)
{
  g_debug("[load-uri] win=%p current_web_view=%p uri=%s", (void *)win, (void *)win->current_web_view,
          uri ? uri : "(null)");

  if (!win->current_web_view)
    return;

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  wig_application_mark_typed_navigation(app, win->current_web_view, uri);
  wig_application_mark_internal_navigation(app, win->current_web_view, uri);
  webkit_web_view_load_uri(win->current_web_view, uri);
  WigTab *selected = wig_tab_list_get_active(win->tab_list);
  if (selected)
    gtk_widget_grab_focus(wig_tab_get_widget(selected));
}

static void wig_window_load_url(WigWindow *win)
{
  g_autofree char *complete_uri = wig_util_complete_uri(gtk_editable_get_text(GTK_EDITABLE(win->url_entry)));
  gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
  wig_window_load_uri(win, complete_uri);
}

static void wig_window_entry_completion_popover_activate(WigEntryCompletionPopover *popover, const char *uri,
                                                         WigWindow *win)
{
  win->suppress_entry_completion = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(win->url_entry), uri);
  win->suppress_entry_completion = FALSE;

  gtk_popover_popdown(GTK_POPOVER(popover));
  wig_window_load_uri(win, uri);
}

static void wig_window_entry_completion_popover_selected(WigEntryCompletionPopover *popover, const char *text,
                                                         WigWindow *win)
{
  win->suppress_entry_completion = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(win->url_entry), text);
  gtk_editable_set_position(GTK_EDITABLE(win->url_entry), -1);
  win->suppress_entry_completion = FALSE;
}

static void wig_window_update_entry_completion(WigWindow *win)
{
  if (win->suppress_entry_completion || !GTK_IS_POPOVER(win->entry_completion_popover))
    return;

  const char *text = gtk_editable_get_text(GTK_EDITABLE(win->url_entry));
  if (!win->url_entry_focused || !text || !*text) {
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
    return;
  }

  WigHistoryStore *store = wig_application_get_history_store(
      WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win))));
  if (!store) {
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
    return;
  }

  gboolean has_more = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) history_items = wig_history_store_query(store, text, 0, 10, &has_more, &error);
  if (!history_items) {
    g_warning("entry-completion: history query failed: %s", error->message);
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
    return;
  }

  g_autoptr(GPtrArray) completion_items = g_ptr_array_new_with_free_func(
      (GDestroyNotify)wig_entry_completion_item_free);
  for (guint i = 0; i < history_items->len; i++) {
    WigHistoryItem *history_item = g_ptr_array_index(history_items, i);
    const char *title = wig_history_item_get_title(history_item);
    const char *url = wig_history_item_get_url(history_item);
    g_ptr_array_add(completion_items, wig_entry_completion_item_new(title && *title ? title : url, url, url, url));
  }

  wig_entry_completion_popover_set_items(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover), text,
                                         completion_items);
  if (wig_entry_completion_popover_get_n_items(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover)) > 0) {
    int width = gtk_widget_get_width(win->url_entry);
    GdkRectangle pointing_to = {
      .x = 0,
      .y = gtk_widget_get_height(win->url_entry),
      .width = width,
      .height = 1,
    };
    wig_entry_completion_popover_set_width(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover), width);
    gtk_popover_set_pointing_to(GTK_POPOVER(win->entry_completion_popover), &pointing_to);
    gtk_popover_popup(GTK_POPOVER(win->entry_completion_popover));
  } else
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
}

static void wig_window_url_entry_changed(GtkEditable *editable, WigWindow *win)
{
  wig_window_update_entry_completion(win);
}

static gboolean wig_window_url_entry_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                                 GdkModifierType state, WigWindow *win)
{
  if (!GTK_IS_POPOVER(win->entry_completion_popover) || !gtk_widget_get_visible(win->entry_completion_popover)
      || wig_entry_completion_popover_get_n_items(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover)) == 0)
    return FALSE;

  switch (keyval) {
  case GDK_KEY_Down:
  case GDK_KEY_KP_Down:
    return wig_entry_completion_popover_select_next(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover));
  case GDK_KEY_Up:
  case GDK_KEY_KP_Up:
    return wig_entry_completion_popover_select_previous(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover));
  default:
    return FALSE;
  }
}

static void wig_window_url_entry_focus_enter(GtkEventControllerFocus *controller, WigWindow *win)
{
  win->url_entry_focused = TRUE;
}

static void wig_window_url_entry_focus_leave(GtkEventControllerFocus *controller, WigWindow *win)
{
  win->url_entry_focused = FALSE;
  if (GTK_IS_POPOVER(win->entry_completion_popover))
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
}

static gboolean point_is_inside_widget(GtkWidget *root, GtkWidget *widget, double x, double y)
{
  if (!widget || !gtk_widget_get_mapped(widget))
    return FALSE;

  graphene_point_t root_point = GRAPHENE_POINT_INIT((float)x, (float)y);
  graphene_point_t widget_point;
  if (!gtk_widget_compute_point(root, widget, &root_point, &widget_point))
    return FALSE;

  return widget_point.x >= 0 && widget_point.y >= 0 && widget_point.x < gtk_widget_get_width(widget)
      && widget_point.y < gtk_widget_get_height(widget);
}

static void wig_window_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigWindow *win)
{
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  /* Handle typical back/forward mouse buttons. */
  if (button == 8)
    g_action_group_activate_action(G_ACTION_GROUP(win), "go-back", NULL);
  else if (button == 9)
    g_action_group_activate_action(G_ACTION_GROUP(win), "go-forward", NULL);

  if (!GTK_IS_POPOVER(win->entry_completion_popover) || !gtk_widget_get_visible(win->entry_completion_popover))
    return;

  GtkWidget *root = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  if (point_is_inside_widget(root, win->url_entry, x, y)
      || point_is_inside_widget(root, win->entry_completion_popover, x, y))
    return;

  gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
}

static void wig_window_update_navigation_actions(WigWindow *win)
{
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(win), "go-back");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action),
                              win->current_web_view ? webkit_web_view_can_go_back(win->current_web_view) : FALSE);
  action = g_action_map_lookup_action(G_ACTION_MAP(win), "go-forward");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action),
                              win->current_web_view ? webkit_web_view_can_go_forward(win->current_web_view) : FALSE);
}

static const char *wig_decision_type_name(WebKitPolicyDecisionType type)
{
  switch (type) {
  case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
    return "NAVIGATION_ACTION";
  case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION:
    return "NEW_WINDOW_ACTION";
  case WEBKIT_POLICY_DECISION_TYPE_RESPONSE:
    return "RESPONSE";
  default:
    return "UNKNOWN";
  }
}

static const char *wig_navigation_type_name(WebKitNavigationType type)
{
  switch (type) {
  case WEBKIT_NAVIGATION_TYPE_LINK_CLICKED:
    return "LINK_CLICKED";
  case WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED:
    return "FORM_SUBMITTED";
  case WEBKIT_NAVIGATION_TYPE_BACK_FORWARD:
    return "BACK_FORWARD";
  case WEBKIT_NAVIGATION_TYPE_RELOAD:
    return "RELOAD";
  case WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED:
    return "FORM_RESUBMITTED";
  case WEBKIT_NAVIGATION_TYPE_OTHER:
    return "OTHER";
  default:
    return "UNKNOWN";
  }
}

static gboolean wig_window_decide_policy(WigWindow *win, WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType decision_type, WebKitWebView *web_view)
{
  g_debug("[decide-policy] win=%p decision=%p type=%s (%d)", (void *)win, (void *)decision,
          wig_decision_type_name(decision_type), decision_type);

  // RESPONSE decisions are handled at the application level (on_web_view_decide_policy).
  if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION
      && decision_type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;

  WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
      WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  WebKitNavigationType nav_type = webkit_navigation_action_get_navigation_type(action);
  WebKitURIRequest *request = webkit_navigation_action_get_request(action);
  const char *request_uri = request ? webkit_uri_request_get_uri(request) : NULL;
  const char *frame_name = webkit_navigation_action_get_frame_name(action);

  g_debug("[decide-policy]   nav_type=%s (%d) uri=%s button=%u frame_name=%s", wig_navigation_type_name(nav_type),
          nav_type, request_uri ? request_uri : "(null)", webkit_navigation_action_get_mouse_button(action),
          frame_name ? frame_name : "(null)");

  const char *target_scheme = request_uri ? g_uri_peek_scheme(request_uri) : NULL;
  if (g_strcmp0(target_scheme, "wig") == 0
      && !wig_application_take_internal_navigation(wig_application_get(), web_view, request_uri)) {
    const char *current_uri = webkit_web_view_get_uri(web_view);
    const char *current_scheme = current_uri ? g_uri_peek_scheme(current_uri) : NULL;
    if (g_strcmp0(current_scheme, "wig") != 0) {
      g_warning("wig: rejecting navigation to '%s' from '%s'", request_uri, current_uri ? current_uri : "(null)");
      webkit_policy_decision_ignore(decision);
      return TRUE;
    }
  }

  gboolean middle_click = nav_type == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
      && webkit_navigation_action_get_mouse_button(action) == WPE_BUTTON_MIDDLE;

  /* Middle-clicking a link opens it in a background tab. A link aimed at another
   * frame (target="_blank") opens a foreground tab; letting the decision through
   * would make WebKit ask for a whole new window instead. */
  if (middle_click || decision_type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
    g_autoptr(WebKitWebView) new_view = wig_application_create_web_view(wig_application_get());
    WigTab *tab = wig_window_add_tab_for_view(win, new_view);
    webkit_web_view_load_request(new_view, request);
    if (!middle_click)
      wig_tab_list_set_active(win->tab_list, tab);
    webkit_policy_decision_ignore(decision);
    return TRUE;
  }

  webkit_policy_decision_use(decision);
  return TRUE;
}

static void wig_window_web_view_ready_to_show(WigWindow *win, WebKitWebView *web_view)
{
  gtk_widget_grab_focus(wpe_view_gtk_get_widget(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(web_view))));
  gtk_window_present(GTK_WINDOW(win));
}

static WebKitWebView *wig_window_web_view_create(WigWindow *win, WebKitNavigationAction *navigation,
                                                 WebKitWebView *opener)
{
  const char *target_uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(navigation));
  const char *target_scheme = g_uri_peek_scheme(target_uri);
  if (g_strcmp0(target_scheme, "wig") == 0) {
    const char *current_uri = webkit_web_view_get_uri(opener);
    const char *current_scheme = current_uri ? g_uri_peek_scheme(current_uri) : NULL;
    if (g_strcmp0(current_scheme, "wig") != 0) {
      g_warning("wig: rejecting popup to '%s' from '%s'", target_uri, current_uri ? current_uri : "(null)");
      return NULL;
    }
  }

  g_autoptr(WebKitWebView) web_view = WEBKIT_WEB_VIEW(
      g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", win->current_web_view, "settings",
                   webkit_web_view_get_settings(win->current_web_view), NULL));

  WigWindow *new_win = wig_window_new(WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win))));
  wig_window_add_web_view(new_win, web_view);
  g_signal_connect_object(web_view, "ready-to-show", G_CALLBACK(wig_window_web_view_ready_to_show), new_win,
                          G_CONNECT_SWAPPED);
  return web_view;
}

static GMenu *build_context_menu(GList *items, GSimpleActionGroup *action_group, WebKitHitTestResult *hit_test_result)
{
  g_autoptr(GMenu) menu = g_menu_new();
  GMenu *section_menu = menu;
  for (GList *l = items; l != NULL; l = g_list_next(l)) {
    WebKitContextMenuItem *item = WEBKIT_CONTEXT_MENU_ITEM(l->data);

    if (webkit_context_menu_item_is_separator(item)) {
      g_autoptr(GMenu) section = g_menu_new();
      g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
      section_menu = section;
    } else if (webkit_context_menu_item_get_stock_action(item) == WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK
               && webkit_hit_test_result_context_is_link(hit_test_result)) {
      g_autoptr(GMenuItem) menu_item = g_menu_item_new("Open Link in New Tab", NULL);
      const char *uri = webkit_hit_test_result_get_link_uri(hit_test_result);
      g_menu_item_set_action_and_target(menu_item, "popup.open-in-new-tab", "s", uri);
      g_menu_append_item(section_menu, menu_item);
    } else {
      GAction *action = webkit_context_menu_item_get_gaction(item);
      if (action) {
        g_action_map_add_action(G_ACTION_MAP(action_group), action);

        g_autoptr(GMenuItem) menu_item = NULL;
        WebKitContextMenu *subcontext_menu = webkit_context_menu_item_get_submenu(item);
        if (subcontext_menu) {
          g_autoptr(GMenu) submenu = build_context_menu(webkit_context_menu_get_items(subcontext_menu), action_group,
                                                        hit_test_result);
          menu_item = g_menu_item_new_submenu(webkit_context_menu_item_get_title(item), G_MENU_MODEL(submenu));
        } else {
          menu_item = g_menu_item_new(webkit_context_menu_item_get_title(item), NULL);
          g_autofree char *action_name = g_strdup_printf("wpeContextMenu.%s", g_action_get_name(action));
          g_menu_item_set_action_and_target_value(menu_item, action_name,
                                                  webkit_context_menu_item_get_gaction_target(item));
        }
        g_menu_append_item(section_menu, menu_item);
      }
    }
  }
  return g_steal_pointer(&menu);
}

static gboolean wig_window_web_view_context_menu(WigWindow *win, WebKitContextMenu *context_menu,
                                                 WebKitHitTestResult *hit_test_result)
{
  if (!win->current_web_view)
    return FALSE;

  g_autoptr(GSimpleActionGroup) action_group = g_simple_action_group_new();
  g_autoptr(GMenu) menu = build_context_menu(webkit_context_menu_get_items(context_menu), action_group,
                                             hit_test_result);
  if (g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 0)
    return FALSE;

  GdkRectangle target = { 0, 0, 1, 1 };
  gboolean has_position = webkit_context_menu_get_position(context_menu, &target.x, &target.y);

  wpe_view_gtk_show_context_menu(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(win->current_web_view)), G_MENU_MODEL(menu),
                                 G_ACTION_GROUP(action_group), has_position ? &target : NULL);

  return TRUE;
}

static void wig_window_update_stop_reload_actions(WigWindow *win)
{
  if (!win->current_web_view)
    return;

  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(win), "stop-reload");
  bool is_loading = webkit_web_view_is_loading(win->current_web_view);
  g_action_change_state(action, g_variant_new_boolean(is_loading));
}

static void wig_window_on_mouse_target_changed(WigWindow *win, WebKitHitTestResult *hit_test_result, guint modifiers)
{
  WigTab *tab = wig_tab_list_get_active(win->tab_list);
  if (!tab)
    return;

  const char *uri = NULL;
  if (webkit_hit_test_result_context_is_link(hit_test_result))
    uri = webkit_hit_test_result_get_link_uri(hit_test_result);

  wig_tab_set_hovered_link(tab, uri, win->current_origin);
}

static gboolean wig_window_on_enter_fullscreen(WigWindow *win)
{
  gtk_window_fullscreen(GTK_WINDOW(win));
  return TRUE;
}

static gboolean wig_window_on_leave_fullscreen(WigWindow *win)
{
  gtk_window_unfullscreen(GTK_WINDOW(win));
  return TRUE;
}

static void wig_window_fullscreen_changed(WigWindow *win)
{
  bool is_fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(win));

  gtk_widget_set_visible(win->header_bar, !is_fullscreen);
  if (win->tab_bar)
    gtk_widget_set_visible(win->tab_bar, !is_fullscreen);
  if (win->tab_separator)
    gtk_widget_set_visible(win->tab_separator, !is_fullscreen);
  if (win->tab_sidebar)
    gtk_widget_set_visible(win->tab_sidebar, !is_fullscreen);
}

static void wig_window_tab_added(WigTabList *list, WigTab *tab, guint position, WigWindow *win)
{
  gtk_stack_add_child(GTK_STACK(win->tab_stack), wig_tab_get_widget(tab));
}

static void wig_window_tab_removed(WigTabList *list, WigTab *tab, guint position, WigWindow *win)
{
  gtk_stack_remove(GTK_STACK(win->tab_stack), wig_tab_get_widget(tab));
}

static void wig_window_active_tab_changed(WigWindow *win, GParamSpec *pspec, WigTabList *list)
{
  g_autoptr(WebKitWebView) previous_web_view = g_steal_pointer(&win->current_web_view);

  if (previous_web_view) {
    g_signal_handlers_disconnect_by_func(previous_web_view, wig_window_update_url, win);
    g_signal_handlers_disconnect_by_func(previous_web_view, wig_window_update_permissions, win);
    g_signal_handlers_disconnect_by_func(previous_web_view, wig_window_update_load_progress, win);
    g_signal_handlers_disconnect_by_func(previous_web_view, wig_window_update_stop_reload_actions, win);
    g_signal_handlers_disconnect_by_func(previous_web_view, wig_window_on_enter_fullscreen, win);
    g_signal_handlers_disconnect_by_func(previous_web_view, wig_window_on_leave_fullscreen, win);
    g_signal_handlers_disconnect_by_func(previous_web_view, wig_window_on_mouse_target_changed, win);

    WebKitBackForwardList *backForwardlist = webkit_web_view_get_back_forward_list(previous_web_view);
    g_signal_handlers_disconnect_by_func(backForwardlist, wig_window_update_navigation_actions, win);
  }

  WigTab *tab = wig_tab_list_get_active(win->tab_list);
  win->current_web_view = tab ? wig_tab_get_web_view(tab) : NULL;

  if (tab)
    gtk_stack_set_visible_child(GTK_STACK(win->tab_stack), wig_tab_get_widget(tab));

  if (win->search_bar)
    wig_search_bar_set_tab(WIG_SEARCH_BAR(win->search_bar), tab);

  wig_window_update_url(win);
  wig_window_update_navigation_actions(win);
  wig_window_update_stop_reload_actions(win);
  wig_window_update_permissions(win);
  if (!win->current_web_view || webkit_web_view_is_loading(win->current_web_view))
    wig_window_update_load_progress(win);

  if (win->current_web_view) {
    g_object_ref(win->current_web_view);

    g_signal_connect_object(win->current_web_view, "notify::uri", G_CALLBACK(wig_window_update_url), win,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "notify::uri", G_CALLBACK(wig_window_update_permissions), win,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "notify::estimated-load-progress",
                            G_CALLBACK(wig_window_update_load_progress), win, G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "load-changed", G_CALLBACK(wig_window_update_stop_reload_actions),
                            win, G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "enter-fullscreen", G_CALLBACK(wig_window_on_enter_fullscreen), win,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "leave-fullscreen", G_CALLBACK(wig_window_on_leave_fullscreen), win,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "mouse-target-changed",
                            G_CALLBACK(wig_window_on_mouse_target_changed), win, G_CONNECT_SWAPPED);

    WebKitBackForwardList *backForwardlist = webkit_web_view_get_back_forward_list(win->current_web_view);
    g_signal_connect_object(backForwardlist, "changed", G_CALLBACK(wig_window_update_navigation_actions), win,
                            G_CONNECT_SWAPPED);
  }
}

static WigTab *wig_window_create_tab(WigWindow *win)
{
  wig_window_new_tab(NULL, NULL, win);
  return wig_tab_list_get_active(win->tab_list);
}

/* Find which window in this application owns a given tab id. */
static WigWindow *wig_window_find_owner(WigApplication *app, guint32 tab_id)
{
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
  for (GList *l = windows; l; l = g_list_next(l)) {
    if (!WIG_IS_WINDOW(l->data))
      continue;
    WigWindow *w = WIG_WINDOW(l->data);
    if (wig_tab_list_get_by_id(w->tab_list, tab_id))
      return w;
  }
  return NULL;
}

/* tab.move-to(uu): move tab_id to this window at insert_index.
 * If the tab already lives here, this is a reorder. */
static void wig_window_move_tab_to(GtkWidget *widget, const char *action_name, GVariant *parameter)
{
  WigWindow *dst = WIG_WINDOW(widget);
  guint32 tab_id, insert_index;
  g_variant_get(parameter, "(uu)", &tab_id, &insert_index);

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(dst)));
  WigWindow *src = wig_window_find_owner(app, tab_id);
  if (!src)
    return;

  WigTab *tab = wig_tab_list_get_by_id(src->tab_list, tab_id);
  if (!tab)
    return;

  if (src == dst) {
    /* Same window — reorder only. */
    guint current = wig_tab_list_index_of(dst->tab_list, tab);
    guint target = (guint)insert_index;
    if (target != current && target != current + 1)
      wig_tab_list_move(dst->tab_list, tab, target);
    return;
  }

  /* Cross-window move: source must keep at least one tab. */
  if (wig_tab_list_get_n_tabs(src->tab_list) <= 1)
    return;

  WebKitWebView *web_view = wig_tab_get_web_view(tab);

  /* Hold a ref so the widget survives gtk_stack_remove in wig_tab_list_detach. */
  GtkWidget *tab_widget = g_object_ref(wig_tab_get_widget(tab));
  g_autoptr(WigTab) owned_tab = wig_tab_list_detach(src->tab_list, tab);

  g_signal_handlers_disconnect_by_func(web_view, wig_window_close_tab, src);
  g_signal_connect_object(web_view, "close", G_CALLBACK(wig_window_close_tab), dst, G_CONNECT_SWAPPED);

  guint n = wig_tab_list_get_n_tabs(dst->tab_list);
  guint pos = MIN((guint)insert_index, n);
  wig_tab_list_attach(dst->tab_list, owned_tab);
  g_object_unref(tab_widget);
  /* attach appends; reorder if not at end */
  if (pos < n)
    wig_tab_list_move(dst->tab_list, owned_tab, pos);
  wig_tab_list_set_active(dst->tab_list, owned_tab);

  wpe_view_set_toplevel(webkit_web_view_get_wpe_view(web_view), dst->toplevel);
  gtk_window_present(GTK_WINDOW(dst));
}

static void wig_window_detach_tab(GtkWidget *widget, const char *action_name, GVariant *parameter)
{
  WigWindow *win = WIG_WINDOW(widget);
  guint32 tab_id = g_variant_get_uint32(parameter);
  WigTab *tab = wig_tab_list_get_by_id(win->tab_list, tab_id);
  if (!tab)
    return;

  if (wig_tab_list_get_n_tabs(win->tab_list) <= 1)
    return;

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  WigWindow *new_win = wig_window_new(app);

  WebKitWebView *web_view = wig_tab_get_web_view(tab);

  /* Detach reuses the existing WigTab — no close-tab signal, no history save.
   * This fires tab-removed on the old list, which removes the WPE widget from
   * the old stack. We hold a ref so it survives unparenting. */
  GtkWidget *tab_widget = g_object_ref(wig_tab_get_widget(tab));
  g_autoptr(WigTab) owned_tab = wig_tab_list_detach(win->tab_list, tab);

  /* Re-wire the web view's "close" signal from the old window to the new one. */
  g_signal_handlers_disconnect_by_func(web_view, wig_window_close_tab, win);
  g_signal_connect_object(web_view, "close", G_CALLBACK(wig_window_close_tab), new_win, G_CONNECT_SWAPPED);

  /* Attach before changing the toplevel so the widget is in the new stack when
   * wpe_view_set_toplevel migrates the native surface. */
  wig_tab_list_attach(new_win->tab_list, owned_tab);
  wig_tab_list_set_active(new_win->tab_list, owned_tab);
  g_object_unref(tab_widget);

  wpe_view_set_toplevel(webkit_web_view_get_wpe_view(web_view), new_win->toplevel);
  gtk_window_present(GTK_WINDOW(new_win));
}

static void wig_window_constructed(GObject *object)
{
  G_OBJECT_CLASS(wig_window_parent_class)->constructed(object);

  WigWindow *win = WIG_WINDOW(object);
  g_action_map_add_action_entries(G_ACTION_MAP(win), actions, G_N_ELEMENTS(actions), win);

  win->context_menu_action_group = G_ACTION_GROUP(g_simple_action_group_new());
  g_action_map_add_action_entries(G_ACTION_MAP(win->context_menu_action_group), context_menu_actions,
                                  G_N_ELEMENTS(context_menu_actions), win);
  gtk_widget_insert_action_group(GTK_WIDGET(win), "popup", win->context_menu_action_group);

  g_signal_connect(win, "notify::fullscreened", G_CALLBACK(wig_window_fullscreen_changed), NULL);

  GtkGestureClick *window_click = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(window_click), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(window_click), GTK_PHASE_CAPTURE);
  g_signal_connect_object(window_click, "pressed", G_CALLBACK(wig_window_click_pressed), win, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(GTK_WIDGET(win), GTK_EVENT_CONTROLLER(window_click));

  win->toolbar_view = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(win->toolbar_view), ADW_TOOLBAR_FLAT);

  win->header_bar = gtk_header_bar_new();

  GtkWidget *start_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(box, "navigation-box");

  win->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->back_button), "win.go-back");
  gtk_widget_add_css_class(win->back_button, "toolbar-button");
  gtk_box_append(GTK_BOX(box), win->back_button);

  GtkGestureClick *back_right = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(back_right), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(back_right), GTK_PHASE_CAPTURE);
  g_signal_connect_object(back_right, "pressed", G_CALLBACK(wig_window_back_button_right_pressed), win,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(win->back_button, GTK_EVENT_CONTROLLER(back_right));

  win->forward_button = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->forward_button), "win.go-forward");
  gtk_widget_add_css_class(win->forward_button, "toolbar-button");
  gtk_box_append(GTK_BOX(box), win->forward_button);

  GtkGestureClick *forward_right = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(forward_right), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(forward_right), GTK_PHASE_CAPTURE);
  g_signal_connect_object(forward_right, "pressed", G_CALLBACK(wig_window_forward_button_right_pressed), win,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(win->forward_button, GTK_EVENT_CONTROLLER(forward_right));
  gtk_box_append(GTK_BOX(start_box), box);

  win->stop_reload_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->stop_reload_button), "win.stop-reload");
  gtk_widget_add_css_class(win->stop_reload_button, "toolbar-button");
  gtk_box_append(GTK_BOX(start_box), win->stop_reload_button);

  GtkGestureClick *reload_middle = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(reload_middle), GDK_BUTTON_MIDDLE);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(reload_middle), GTK_PHASE_CAPTURE);
  g_signal_connect_object(reload_middle, "pressed", G_CALLBACK(wig_window_reload_middle_pressed), win,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(win->stop_reload_button, GTK_EVENT_CONTROLLER(reload_middle));

  gtk_header_bar_pack_start(GTK_HEADER_BAR(win->header_bar), start_box);

  win->url_entry = gtk_entry_new();
  win->entry_completion_popover = wig_entry_completion_popover_new();
  gtk_widget_set_parent(win->entry_completion_popover, win->url_entry);
  g_signal_connect_object(win->entry_completion_popover, "activate",
                          G_CALLBACK(wig_window_entry_completion_popover_activate), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->entry_completion_popover, "selected",
                          G_CALLBACK(wig_window_entry_completion_popover_selected), win, G_CONNECT_DEFAULT);

  g_signal_connect_object(win->url_entry, "activate", G_CALLBACK(wig_window_load_url), win, G_CONNECT_SWAPPED);
  g_signal_connect_object(win->url_entry, "changed", G_CALLBACK(wig_window_url_entry_changed), win, G_CONNECT_DEFAULT);
  gtk_widget_set_hexpand(win->url_entry, TRUE);

  GtkEventController *url_focus = gtk_event_controller_focus_new();
  g_signal_connect_object(url_focus, "enter", G_CALLBACK(wig_window_url_entry_focus_enter), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(url_focus, "leave", G_CALLBACK(wig_window_url_entry_focus_leave), win, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(win->url_entry, url_focus);

  GtkEventController *url_keys = gtk_event_controller_key_new();
  g_signal_connect_object(url_keys, "key-pressed", G_CALLBACK(wig_window_url_entry_key_pressed), win,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(win->url_entry, url_keys);

  win->permissions_button = wig_permissions_button_new();
  win->permissions_manager = wig_application_get_permissions_manager(wig_application_get());
  g_signal_connect_object(win->permissions_manager, "changed", G_CALLBACK(wig_window_on_permissions_changed), win,
                          G_CONNECT_SWAPPED);

  GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(entry_box, "linked");
  gtk_box_append(GTK_BOX(entry_box), win->permissions_button);
  gtk_box_append(GTK_BOX(entry_box), win->url_entry);

  GtkWidget *clamp = adw_clamp_new();
  gtk_widget_set_hexpand(clamp, TRUE);
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 860);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 560);
  adw_clamp_set_child(ADW_CLAMP(clamp), entry_box);
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(win->header_bar), clamp);

  win->tab_list = wig_tab_list_new();
  gtk_widget_insert_action_group(GTK_WIDGET(win), "tabs", G_ACTION_GROUP(wig_tab_list_get_action_group(win->tab_list)));
  g_signal_connect_object(win->tab_list, "close-tab", G_CALLBACK(wig_window_tab_close), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "notify::active-tab", G_CALLBACK(wig_window_active_tab_changed), win,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(win->tab_list, "tab-added", G_CALLBACK(wig_window_tab_added), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "tab-removed", G_CALLBACK(wig_window_tab_removed), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "reload-tab", G_CALLBACK(wig_window_tab_reload), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "mute-tab", G_CALLBACK(wig_window_tab_mute), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "duplicate-tab", G_CALLBACK(wig_window_tab_duplicate), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "copy-link-tab", G_CALLBACK(wig_window_tab_copy_link), win, G_CONNECT_DEFAULT);

  win->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content_box = win->content_box;

  win->tab_bar = wig_tab_bar_new(win->tab_list);
  g_signal_connect_object(win->tab_list, "create-tab", G_CALLBACK(wig_window_create_tab), win, G_CONNECT_SWAPPED);
  wig_window_add_tab_view_context_menu(win, win->tab_bar);
  gtk_box_append(GTK_BOX(content_box), win->tab_bar);

  win->tab_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(content_box), win->tab_separator);

  win->tab_stack = gtk_stack_new();

  win->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  GtkWidget *paned = win->paned;
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_end_child(GTK_PANED(paned), win->tab_stack);
  gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_position(GTK_PANED(paned), 200);
  gtk_box_append(GTK_BOX(content_box), paned);

  win->search_bar = wig_search_bar_new();
  g_signal_connect_object(win->search_bar, "closed", G_CALLBACK(wig_window_search_bar_closed), win, G_CONNECT_DEFAULT);
  gtk_box_append(GTK_BOX(content_box), win->search_bar);

  gtk_window_set_child(GTK_WINDOW(win), content_box);
  gtk_window_set_titlebar(GTK_WINDOW(win), win->header_bar);

  WigApplication *app = wig_application_get();
  win->toplevel = wpe_toplevel_gtk_new(WPE_DISPLAY_GTK(wig_application_get_display(app)), 0, GTK_WINDOW(win));

  if (win->id == 0)
    win->id = wig_window_next_id++;
}

static void wig_window_dispose(GObject *object)
{
  WigWindow *win = WIG_WINDOW(object);
  g_clear_handle_id(&win->progress_timeout_id, g_source_remove);

  /* Tearing down the tabs below emits signals that would otherwise reach the bar
   * while the window is already going away. */
  win->search_bar = NULL;

  if (win->tab_list) {
    WigApplication *app = wig_application_get();
    guint n_tabs = wig_tab_list_get_n_tabs(win->tab_list);
    if (n_tabs > 0) {
      g_autoptr(WigClosedGroup) group = g_new(WigClosedGroup, 1);
      group->window_id = win->id;
      group->tabs = NULL;

      for (guint i = 0; i < n_tabs; i++) {
        WigTab *item = wig_tab_list_get_nth(win->tab_list, i);
        WebKitWebView *wv = wig_tab_get_web_view(item);
        WebKitWebViewSessionState *state = webkit_web_view_get_session_state(wv);

        if (state) {
          WigClosedTab *closed = g_new(WigClosedTab, 1);
          closed->state = state;
          closed->was_focused = (wv == win->current_web_view);
          group->tabs = g_slist_prepend(group->tabs, closed);
        }
      }
      group->tabs = g_slist_reverse(group->tabs);

      if (group->tabs)
        wig_application_push_closed_group(app, g_steal_pointer(&group));
    }
    g_clear_object(&win->tab_list);
  }

  g_clear_pointer(&win->tab_view_context_menu, gtk_widget_unparent);
  g_clear_pointer(&win->entry_completion_popover, gtk_widget_unparent);
  g_clear_pointer(&win->back_history_popover, gtk_widget_unparent);
  g_clear_pointer(&win->forward_history_popover, gtk_widget_unparent);
  G_OBJECT_CLASS(wig_window_parent_class)->dispose(object);
}

static void wig_window_finalize(GObject *object)
{
  WigWindow *win = WIG_WINDOW(object);
  g_clear_object(&win->current_web_view);
  g_clear_object(&win->toplevel);
  g_clear_object(&win->context_menu_action_group);
  g_clear_pointer(&win->current_origin, g_free);

  G_OBJECT_CLASS(wig_window_parent_class)->finalize(object);
}

static void wig_window_init(WigWindow *win)
{
}

static void wig_window_class_init(WigWindowClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->constructed = wig_window_constructed;
  gobject_class->dispose = wig_window_dispose;
  gobject_class->finalize = wig_window_finalize;
  gobject_class->get_property = wig_window_get_property;
  gobject_class->set_property = wig_window_set_property;

  props[PROP_ID] = g_param_spec_uint("id", NULL, NULL, 0, G_MAXUINT, 0,
                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(gobject_class, G_N_ELEMENTS(props), props);

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_install_action(widget_class, "tab.detach", "u", wig_window_detach_tab);
  gtk_widget_class_install_action(widget_class, "tab.move-to", "(uu)", wig_window_move_tab_to);
}

WigWindow *wig_window_new(WigApplication *application)
{
  return g_object_new(WIG_TYPE_WINDOW, "application", application, NULL);
}

void wig_window_add_web_view(WigWindow *win, WebKitWebView *web_view)
{
  g_return_if_fail(WIG_IS_WINDOW(win));
  g_return_if_fail(WEBKIT_IS_WEB_VIEW(web_view));

  WigTab *tab = wig_window_add_tab_for_view(win, web_view);
  wig_tab_list_set_active(win->tab_list, tab);
}
