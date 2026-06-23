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
#include "wig-tab-view.h"
#include "wig-utils.h"
#include "wpe-toplevel-gtk.h"
#include "wpe-view-gtk.h"

struct _WigWindow {
  AdwApplicationWindow parent;

  guint id;

  WPEToplevel *toplevel;
  GtkWidget *toolbar_view;
  GtkWidget *header_bar;
  GtkWidget *back_button;
  GtkWidget *forward_button;
  GtkWidget *stop_reload_button;
  GtkWidget *new_tab_button;
  GtkWidget *url_entry;
  AdwTabBar *tab_bar;
  AdwTabView *tab_view;
  GtkWidget *tab_overview;
  GtkWidget *overview_button;
  WebKitWebView *current_web_view;
  guint progress_timeout_id;
  GActionGroup *context_menu_action_group;
};

G_DEFINE_FINAL_TYPE(WigWindow, wig_window, ADW_TYPE_APPLICATION_WINDOW)

enum { PROP_0, PROP_ID, N_PROPS };
static GParamSpec *props[N_PROPS];

static guint wig_window_next_id = 1;

static void wig_window_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigWindow *win = WIG_WINDOW(object);
  switch (prop_id) {
  case PROP_ID:
    g_value_set_uint(value, win->id);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_window_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigWindow *win = WIG_WINDOW(object);
  switch (prop_id) {
  case PROP_ID: {
    win->id = g_value_get_uint(value);
    break;
  }
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
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

static AdwTabPage *wig_window_get_tab_page_for_web_view(WigWindow *win, WebKitWebView *web_view)
{
  int n_pages = adw_tab_view_get_n_pages(win->tab_view);
  for (int i = 0; i < n_pages; i++) {
    AdwTabPage *tab_page = adw_tab_view_get_nth_page(win->tab_view, i);
    WigTabView *tab_view = WIG_TAB_VIEW(adw_tab_page_get_child(tab_page));

    if (wig_tab_view_get_web_view(tab_view) == web_view)
      return tab_page;
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

static gboolean wig_window_tab_close_page(AdwTabView *tab_view_adw, AdwTabPage *page, WigWindow *win)
{
  WigTabView *tab_view = WIG_TAB_VIEW(adw_tab_page_get_child(page));
  WebKitWebView *web_view = wig_tab_view_get_web_view(tab_view);

  wig_window_save_tab_to_history(win, web_view);

  if (adw_tab_view_get_n_pages(tab_view_adw) == 1) {
    adw_tab_view_close_page_finish(tab_view_adw, page, TRUE);
    gtk_window_destroy(GTK_WINDOW(win));
    return TRUE;
  }

  return FALSE;
}

static void wig_window_close_tab(WigWindow *win, WebKitWebView *web_view)
{
  AdwTabPage *tab_page = wig_window_get_tab_page_for_web_view(win, web_view);
  if (tab_page)
    adw_tab_view_close_page(win->tab_view, tab_page);
}

static gboolean wig_window_transform_tab_title(GBinding *binding, const GValue *from, GValue *to, gpointer user_data)
{
  const char *title = g_value_get_string(from);
  g_value_set_string(to, (title && *title) ? title : "New Tab");
  return TRUE;
}

gboolean wig_window_focus_tab_by_site(WigWindow *win, const char *uri)
{
  g_autoptr(GUri) lookup = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  if (!lookup)
    return FALSE;

  const char *lookup_scheme = g_uri_get_scheme(lookup);
  const char *lookup_host = g_uri_get_host(lookup);

  int n_pages = adw_tab_view_get_n_pages(win->tab_view);
  for (int i = 0; i < n_pages; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(win->tab_view, i);
    WigTabView *tab_view = WIG_TAB_VIEW(adw_tab_page_get_child(page));
    WebKitWebView *web_view = wig_tab_view_get_web_view(tab_view);
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
      match = g_str_equal(lookup_scheme, tab_scheme);

    if (match) {
      adw_tab_view_set_selected_page(win->tab_view, page);
      webkit_web_view_reload(web_view);
      return TRUE;
    }
  }
  return FALSE;
}

static AdwTabPage *wig_window_add_tab_page_for_view(WigWindow *win, WebKitWebView *web_view)
{
  GtkWidget *tab_view = wig_tab_view_new(web_view);
  AdwTabPage *tab_page = adw_tab_view_append(win->tab_view, tab_view);
  g_object_bind_property_full(G_OBJECT(web_view), "title", tab_page, "title", G_BINDING_SYNC_CREATE,
                              wig_window_transform_tab_title, NULL, NULL, NULL);
  g_object_bind_property(G_OBJECT(web_view), "is-loading", tab_page, "loading", G_BINDING_SYNC_CREATE);

  g_signal_connect_object(web_view, "close", G_CALLBACK(wig_window_close_tab), win, G_CONNECT_SWAPPED);

  wpe_view_set_toplevel(webkit_web_view_get_wpe_view(web_view), win->toplevel);

  return tab_page;
}

static WebKitWebView *wig_window_create_web_view_for_new_tab(WigWindow *win)
{
  return wig_application_create_web_view(wig_application_get());
}

static void wig_window_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  g_autoptr(WebKitWebView) web_view = wig_window_create_web_view_for_new_tab(win);
  AdwTabPage *tab_page = wig_window_add_tab_page_for_view(win, web_view);
  adw_tab_view_set_selected_page(win->tab_view, tab_page);
  gtk_widget_grab_focus(win->url_entry);
}

static void wig_window_tab_overview(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  adw_tab_overview_set_open(ADW_TAB_OVERVIEW(win->tab_overview),
                            !adw_tab_overview_get_open(ADW_TAB_OVERVIEW(win->tab_overview)));
}

static void wig_window_focus_entry(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  gtk_widget_grab_focus(win->url_entry);
  gtk_editable_select_region(GTK_EDITABLE(win->url_entry), 0, -1);
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

  AdwTabPage *focused_page = NULL;
  for (GSList *l = group->tabs; l; l = g_slist_next(l)) {
    WigClosedTab *closed_tab = l->data;
    g_autoptr(WebKitWebView) web_view = wig_application_create_web_view(app);
    webkit_web_view_restore_session_state(web_view, closed_tab->state);

    WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(web_view);
    WebKitBackForwardListItem *item = webkit_back_forward_list_get_current_item(list);
    if (item)
      webkit_web_view_go_to_back_forward_list_item(web_view, item);

    AdwTabPage *tab_page = wig_window_add_tab_page_for_view(target_win, web_view);
    if (closed_tab->was_focused)
      focused_page = tab_page;
  }

  if (focused_page)
    adw_tab_view_set_selected_page(target_win->tab_view, focused_page);

  if (target_win != win)
    gtk_window_present(GTK_WINDOW(target_win));

  wig_closed_group_free(group);
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

static const GActionEntry actions[] = {
  { "go-back", wig_window_go_back },
  { "go-forward", wig_window_go_forward },
  { "stop-reload", wig_window_stop_reload, NULL, "false", wig_window_change_stop_reload_state },
  { "new-tab", wig_window_new_tab },
  { "tab-overview", wig_window_tab_overview },
  { "focus-entry", wig_window_focus_entry },
  { "close-tab", wig_window_close_tab_action },
  { "reload", wig_window_reload },
  { "reload-bypass-cache", wig_window_reload_bypass_cache },
  { "toggle-fullscreen", wig_window_toggle_fullscreen },
  { "zoom-in", wig_window_zoom_in },
  { "zoom-out", wig_window_zoom_out },
  { "zoom-reset", wig_window_zoom_reset },
  { "undo-close-tab", wig_window_undo_close_tab },
};

static void wig_window_open_in_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  g_autoptr(WebKitWebView) web_view = wig_window_create_web_view_for_new_tab(win);
  AdwTabPage *tab_page = wig_window_add_tab_page_for_view(win, web_view);
  adw_tab_view_set_selected_page(win->tab_view, tab_page);
  if (parameter) {
    const char *uri = g_variant_get_string(parameter, NULL);
    if (uri)
      webkit_web_view_load_uri(web_view, uri);
  }
}

static const GActionEntry context_menu_actions[] = {
  { "open-in-new-tab", wig_window_open_in_new_tab, "s" },
};

static void wig_window_update_url(WigWindow *win)
{
  const char *url = win->current_web_view ? webkit_web_view_get_uri(win->current_web_view) : NULL;
  gtk_editable_set_text(GTK_EDITABLE(win->url_entry), url ? url : "");
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

static void wig_window_load_url(WigWindow *win)
{
  if (!win->current_web_view)
    return;

  g_autofree char *complete_uri = wig_util_complete_uri(gtk_editable_get_text(GTK_EDITABLE(win->url_entry)));
  webkit_web_view_load_uri(win->current_web_view, complete_uri);
  wig_tab_view_grab_focus(WIG_TAB_VIEW(adw_tab_page_get_child(adw_tab_view_get_selected_page(win->tab_view))));
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

static gboolean wig_window_decide_policy(WigWindow *win, WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType decision_type)
{
  if (decision_type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
    if (!webkit_response_policy_decision_is_mime_type_supported(WEBKIT_RESPONSE_POLICY_DECISION(decision))) {
      webkit_policy_decision_download(decision);
      return TRUE;
    }
    return FALSE;
  }

  if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
    return FALSE;

  WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
      WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  if (webkit_navigation_action_get_navigation_type(action) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
      || webkit_navigation_action_get_mouse_button(action) != WPE_BUTTON_MIDDLE)
    return FALSE;

  g_autoptr(WebKitWebView) web_view = wig_window_create_web_view_for_new_tab(win);
  wig_window_add_tab_page_for_view(win, web_view);
  webkit_web_view_load_request(web_view, webkit_navigation_action_get_request(action));

  webkit_policy_decision_ignore(decision);
  return TRUE;
}

static void wig_window_web_view_ready_to_show(WigWindow *win, WebKitWebView *web_view)
{
  gtk_widget_grab_focus(wpe_view_gtk_get_widget(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(web_view))));
  gtk_window_present(GTK_WINDOW(win));
}

static WebKitWebView *wig_window_web_view_create(WigWindow *win, WebKitNavigationAction *navigation)
{
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

static void wig_window_fullscreen_changed(WigWindow *win)
{
  bool is_fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(win));

  gtk_widget_set_visible(GTK_WIDGET(win->header_bar), !is_fullscreen);
  gtk_widget_set_visible(GTK_WIDGET(win->tab_bar), !is_fullscreen);
}

static void wig_window_selected_page_changed(AdwTabView *tab_view_adw, GParamSpec *pspec, WigWindow *win)
{
  g_autoptr(WebKitWebView) previous_web_view = g_steal_pointer(&win->current_web_view);

  if (previous_web_view) {
    g_signal_handlers_disconnect_by_data(previous_web_view, win);

    WebKitBackForwardList *backForwardlist = webkit_web_view_get_back_forward_list(previous_web_view);
    g_signal_handlers_disconnect_by_data(backForwardlist, win);
  }

  AdwTabPage *tab_page = adw_tab_view_get_selected_page(tab_view_adw);
  win->current_web_view = tab_page ? wig_tab_view_get_web_view(WIG_TAB_VIEW(adw_tab_page_get_child(tab_page))) : NULL;

  wig_window_update_url(win);
  wig_window_update_navigation_actions(win);
  wig_window_update_stop_reload_actions(win);
  if (!win->current_web_view || webkit_web_view_is_loading(win->current_web_view))
    wig_window_update_load_progress(win);

  if (win->current_web_view) {
    g_object_ref(win->current_web_view);

    g_signal_connect_object(win->current_web_view, "notify::uri", G_CALLBACK(wig_window_update_url), win,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "notify::estimated-load-progress",
                            G_CALLBACK(wig_window_update_load_progress), win, G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "decide-policy", G_CALLBACK(wig_window_decide_policy), win,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "create", G_CALLBACK(wig_window_web_view_create), win,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "context-menu", G_CALLBACK(wig_window_web_view_context_menu), win,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(win->current_web_view, "load-changed", G_CALLBACK(wig_window_update_stop_reload_actions),
                            win, G_CONNECT_SWAPPED);

    WebKitBackForwardList *backForwardlist = webkit_web_view_get_back_forward_list(win->current_web_view);
    g_signal_connect_object(backForwardlist, "changed", G_CALLBACK(wig_window_update_navigation_actions), win,
                            G_CONNECT_SWAPPED);
  }
}

static AdwTabPage *wig_window_create_tab(WigWindow *win)
{
  wig_window_new_tab(NULL, NULL, win);
  return adw_tab_view_get_selected_page(win->tab_view);
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

  win->toolbar_view = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(win->toolbar_view), ADW_TOOLBAR_RAISED_BORDER);

  win->header_bar = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(win->toolbar_view), win->header_bar);

  GtkWidget *start_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(box, "navigation-box");

  win->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->back_button), "win.go-back");
  gtk_widget_add_css_class(win->back_button, "toolbar-button");
  gtk_box_append(GTK_BOX(box), win->back_button);

  win->forward_button = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->forward_button), "win.go-forward");
  gtk_widget_add_css_class(win->forward_button, "toolbar-button");
  gtk_box_append(GTK_BOX(box), win->forward_button);
  gtk_box_append(GTK_BOX(start_box), box);

  win->stop_reload_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->stop_reload_button), "win.stop-reload");
  gtk_widget_add_css_class(win->stop_reload_button, "toolbar-button");
  gtk_box_append(GTK_BOX(start_box), win->stop_reload_button);

  win->new_tab_button = gtk_button_new_from_icon_name("tab-new-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->new_tab_button), "win.new-tab");
  gtk_widget_add_css_class(win->new_tab_button, "toolbar-button");
  gtk_box_append(GTK_BOX(start_box), win->new_tab_button);

  adw_header_bar_pack_start(ADW_HEADER_BAR(win->header_bar), start_box);

  win->url_entry = gtk_entry_new();
  g_signal_connect_object(win->url_entry, "activate", G_CALLBACK(wig_window_load_url), win, G_CONNECT_SWAPPED);
  GtkWidget *clamp = adw_clamp_new();
  gtk_widget_set_hexpand(clamp, TRUE);
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 860);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 560);
  adw_clamp_set_child(ADW_CLAMP(clamp), win->url_entry);
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(win->header_bar), clamp);

  win->tab_bar = adw_tab_bar_new();
  adw_tab_bar_set_autohide(win->tab_bar, TRUE);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(win->toolbar_view), GTK_WIDGET(win->tab_bar));

  win->tab_view = adw_tab_view_new();
  g_signal_connect(win->tab_view, "notify::selected-page", G_CALLBACK(wig_window_selected_page_changed), win);
  g_signal_connect(win->tab_view, "close-page", G_CALLBACK(wig_window_tab_close_page), win);
  adw_tab_bar_set_view(win->tab_bar, win->tab_view);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(win->toolbar_view), GTK_WIDGET(win->tab_view));

  win->tab_overview = adw_tab_overview_new();
  adw_tab_overview_set_enable_new_tab(ADW_TAB_OVERVIEW(win->tab_overview), TRUE);
  g_signal_connect_object(win->tab_overview, "create-tab", G_CALLBACK(wig_window_create_tab), win, G_CONNECT_SWAPPED);
  adw_tab_overview_set_view(ADW_TAB_OVERVIEW(win->tab_overview), win->tab_view);
  adw_tab_overview_set_child(ADW_TAB_OVERVIEW(win->tab_overview), win->toolbar_view);

  GtkWidget *end_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  win->overview_button = gtk_button_new_from_icon_name("view-grid-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->overview_button), "win.tab-overview");
  gtk_widget_add_css_class(win->overview_button, "toolbar-button");
  gtk_box_append(GTK_BOX(end_box), win->overview_button);

  adw_header_bar_pack_end(ADW_HEADER_BAR(win->header_bar), end_box);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), win->tab_overview);

  WigApplication *app = wig_application_get();
  win->toplevel = wpe_toplevel_gtk_new(WPE_DISPLAY_GTK(wig_application_get_display(app)), 0, GTK_WINDOW(win));

  if (win->id == 0)
    win->id = wig_window_next_id++;
}

static void wig_window_dispose(GObject *object)
{
  WigWindow *win = WIG_WINDOW(object);
  g_clear_handle_id(&win->progress_timeout_id, g_source_remove);

  if (win->tab_view) {
    WigApplication *app = wig_application_get();
    int n_pages = adw_tab_view_get_n_pages(win->tab_view);
    if (n_pages > 0) {
      WigClosedGroup *group = g_new(WigClosedGroup, 1);
      group->window_id = win->id;
      group->tabs = NULL;

      for (int i = 0; i < n_pages; i++) {
        AdwTabPage *tab_page = adw_tab_view_get_nth_page(win->tab_view, i);
        WigTabView *tab_view = WIG_TAB_VIEW(adw_tab_page_get_child(tab_page));
        WebKitWebView *wv = wig_tab_view_get_web_view(tab_view);
        WebKitWebViewSessionState *state = webkit_web_view_get_session_state(wv);

        if (state) {
          WigClosedTab *tab = g_new(WigClosedTab, 1);
          tab->state = state;
          tab->was_focused = (wv == win->current_web_view);
          group->tabs = g_slist_prepend(group->tabs, tab);
        }
      }
      group->tabs = g_slist_reverse(group->tabs);

      if (group->tabs)
        wig_application_push_closed_group(app, group);
      else
        wig_closed_group_free(group);
    }
  }

  G_OBJECT_CLASS(wig_window_parent_class)->dispose(object);
}

static void wig_window_finalize(GObject *object)
{
  WigWindow *win = WIG_WINDOW(object);
  g_clear_object(&win->current_web_view);
  g_clear_object(&win->toplevel);
  g_clear_object(&win->context_menu_action_group);

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

  g_object_class_install_properties(gobject_class, N_PROPS, props);
}

WigWindow *wig_window_new(WigApplication *application)
{
  return g_object_new(WIG_TYPE_WINDOW, "application", application, NULL);
}

void wig_window_add_web_view(WigWindow *win, WebKitWebView *web_view)
{
  g_return_if_fail(WIG_IS_WINDOW(win));
  g_return_if_fail(WEBKIT_IS_WEB_VIEW(web_view));

  AdwTabPage *tab_page = wig_window_add_tab_page_for_view(win, web_view);
  adw_tab_view_set_selected_page(win->tab_view, tab_page);
}
