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

#include "wig-application.h"

#include <errno.h>

#include "wig-downloads-manager.h"
#include "wig-flatpak.h"
#include "wig-history-page.h"
#include "wig-settings-features.h"
#include "wig-settings-filters.h"
#include "wig-settings-page.h"
#include "wig-settings.h"
#include "wig-window.h"
#include "wpe-display-gtk.h"

struct _WigApplication {
  AdwApplication parent;

  WPEDisplay *display;
  WebKitNetworkSession *network_session;
  WebKitWebContext *web_context;
  WebKitSettings *web_settings;
  WebKitMemoryPressureSettings *memory_pressure_settings;
  WigHistoryStore *history_store;
  GHashTable *typed_navigations; /* WebKitWebView* -> char* pending URI */
  GHashTable *internal_navigations; /* WebKitWebView* -> char* pending wig: URI */
  WigDownloadsManager *downloads;
  WebKitUserContentManager *user_content_manager;
  GPtrArray *user_scripts;
  GPtrArray *user_style_sheets;
  WebKitUserContentFilterStore *content_filter_store;

  GSettings *settings;

  WigSession *session;
  gboolean quitting;
  GHashTable *notifications; /* char* -> WebKitNotification* (owned) */
  WigPermissionsManager *permissions_manager;
  WigUpdateMonitor *update_monitor;
};

G_DEFINE_FINAL_TYPE(WigApplication, wig_application, ADW_TYPE_APPLICATION)

static void wig_application_quit_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  g_application_quit(G_APPLICATION(user_data));
}

static void wig_application_add_new_tab_with_uri(WigApplication *app, WigWindow *win, const char *uri)
{
  g_autoptr(WebKitWebView) web_view = wig_application_create_web_view(app);
  wig_window_add_web_view(win, web_view);
  wig_application_mark_internal_navigation(app, web_view, uri);
  webkit_web_view_load_uri(web_view, uri);
}

static WebKitWebView *wig_application_focus_internal_page_in_window(WigWindow *win, const char *uri,
                                                                    WebKitWebView *ignore, gboolean reload)
{
  return wig_window_focus_tab_by_site(win, uri, ignore, reload);
}

static WigWindow *wig_application_find_browser_window(WigApplication *app)
{
  GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (WIG_IS_WINDOW(active))
    return WIG_WINDOW(active);

  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next) {
    if (WIG_IS_WINDOW(l->data))
      return WIG_WINDOW(l->data);
  }

  return NULL;
}

gboolean wig_application_open_uri(WigApplication *app, GtkWindow *win, const char *uri)
{
  const char *scheme = uri ? g_uri_peek_scheme(uri) : NULL;

  if (g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0) {
    g_warning("wig: refusing to open '%s'", uri ? uri : "(null)");
    return FALSE;
  }

  if (!WIG_IS_WINDOW(win))
    win = GTK_WINDOW(wig_application_find_browser_window(app));

  if (!win) {
    g_warning("wig: no window to open '%s' in", uri);
    return FALSE;
  }

  g_autoptr(WebKitWebView) web_view = wig_application_create_web_view(app);
  wig_window_add_web_view(WIG_WINDOW(win), web_view);
  webkit_web_view_load_uri(web_view, uri);

  return TRUE;
}

gboolean wig_application_focus_internal_page(WigApplication *app, const char *uri, WebKitWebView *ignore)
{
  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next) {
    if (!WIG_IS_WINDOW(l->data))
      continue;

    if (wig_application_focus_internal_page_in_window(WIG_WINDOW(l->data), uri, ignore, FALSE)) {
      gtk_window_present(GTK_WINDOW(l->data));
      return TRUE;
    }
  }

  return FALSE;
}

void wig_application_open_internal_page(WigApplication *app, GtkWindow *win, const char *uri)
{
  if (WIG_IS_WINDOW(win) && wig_application_focus_internal_page_in_window(WIG_WINDOW(win), uri, NULL, TRUE))
    return;

  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next) {
    if (l->data == win)
      continue;
    if (!WIG_IS_WINDOW(l->data))
      continue;

    if (wig_application_focus_internal_page_in_window(WIG_WINDOW(l->data), uri, NULL, TRUE)) {
      gtk_window_present(GTK_WINDOW(l->data));
      return;
    }
  }

  if (!WIG_IS_WINDOW(win))
    win = GTK_WINDOW(wig_application_find_browser_window(app));

  if (win)
    wig_application_add_new_tab_with_uri(app, WIG_WINDOW(win), uri);
}

static gboolean uri_should_be_recorded(const char *uri)
{
  if (!uri || !*uri)
    return FALSE;

  const char *scheme = g_uri_peek_scheme(uri);
  if (!scheme)
    return FALSE;

  return !g_str_equal(scheme, "wig") && !g_str_equal(scheme, "about") && !g_str_equal(scheme, "webkit");
}

static void history_web_view_finalized(gpointer data, GObject *web_view)
{
  WigApplication *app = WIG_APPLICATION(data);
  g_hash_table_remove(app->typed_navigations, web_view);
  g_hash_table_remove(app->internal_navigations, web_view);
}

static void record_history_visit(WigApplication *app, WebKitWebView *web_view, gboolean typed)
{
  if (!app->history_store)
    return;

  const char *uri = webkit_web_view_get_uri(web_view);
  if (!uri_should_be_recorded(uri))
    return;

  const char *title = webkit_web_view_get_title(web_view);
  g_autoptr(GError) error = NULL;
  wig_history_store_record_visit(app->history_store, uri, title ? title : "", typed, g_get_real_time() / 1000, &error);
  if (error)
    g_warning("history: record visit: %s", error->message);
}

static const char *wig_load_event_name(WebKitLoadEvent load_event)
{
  switch (load_event) {
  case WEBKIT_LOAD_STARTED:
    return "STARTED";
  case WEBKIT_LOAD_REDIRECTED:
    return "REDIRECTED";
  case WEBKIT_LOAD_COMMITTED:
    return "COMMITTED";
  case WEBKIT_LOAD_FINISHED:
    return "FINISHED";
  default:
    return "UNKNOWN";
  }
}

static void on_web_view_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, WigApplication *app)
{
  g_debug("[load-changed] web_view=%p event=%s (%d) uri=%s", (void *)web_view, wig_load_event_name(load_event),
          load_event, webkit_web_view_get_uri(web_view) ? webkit_web_view_get_uri(web_view) : "(null)");

  if (load_event != WEBKIT_LOAD_COMMITTED)
    return;

  gboolean typed = g_hash_table_remove(app->typed_navigations, web_view);
  record_history_visit(app, web_view, typed);

  const char *uri = webkit_web_view_get_uri(web_view);
  if (uri) {
    g_autoptr(WebKitSecurityOrigin) security_origin = webkit_security_origin_new_for_uri(uri);
    g_autofree char *origin = security_origin ? webkit_security_origin_to_string(security_origin) : NULL;
    if (origin)
      wig_permissions_manager_visit(app->permissions_manager, origin);
  }

  wig_session_queue_save(app->session);
}

static gboolean on_load_failed(WebKitWebView *web_view, WebKitLoadEvent load_event, const char *failing_uri,
                               GError *error, WigApplication *app)
{
  g_debug("[load-failed] web_view=%p event=%s (%d) uri=%s error=%s domain=%s code=%d", (void *)web_view,
          wig_load_event_name(load_event), load_event, failing_uri ? failing_uri : "(null)",
          error ? error->message : "(null)", error ? g_quark_to_string(error->domain) : "(null)",
          error ? error->code : 0);

  /* A frame load interrupted by a policy change (e.g. a download starting, or a
   * navigation that was ignored/redirected) is not a real error: the previously
   * committed page should stay visible. Returning TRUE suppresses WebKit's
   * default error page, which would otherwise replace the rendered content.
   * Cancelled loads are treated the same way. */
  if (g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE)
      || g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) {
    g_debug("[load-failed]   -> suppressing error page (policy/cancelled), returning TRUE");
    return TRUE;
  }

  return FALSE;
}

static void on_history_title_changed(WebKitWebView *web_view, GParamSpec *pspec, WigApplication *app)
{
  if (!app->history_store)
    return;

  const char *uri = webkit_web_view_get_uri(web_view);
  if (!uri_should_be_recorded(uri))
    return;

  const char *title = webkit_web_view_get_title(web_view);
  if (!title || !*title)
    return;

  g_autoptr(GError) error = NULL;
  wig_history_store_update_title(app->history_store, uri, title, &error);
  if (error)
    g_warning("history: update title: %s", error->message);
}

static void wig_application_new_window_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigApplication *app = WIG_APPLICATION(user_data);
  WigWindow *win = wig_window_new(app);

  g_autoptr(WebKitWebView) web_view = wig_application_create_web_view(app);
  wig_window_add_web_view(win, web_view);

  gtk_window_present(GTK_WINDOW(win));
  g_action_group_activate_action(G_ACTION_GROUP(win), "focus-entry", NULL);
}

static void wig_application_download_update_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigApplication *app = WIG_APPLICATION(user_data);

  wig_update_monitor_download(app->update_monitor);
}

/* Nothing here is about the update: the helper waits for wig to let go of its
 * name and then asks for it back, which is a restart whatever the reason for
 * one. Tabs are put back so restarting costs the session nothing. */
static void wig_application_restart_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigApplication *app = WIG_APPLICATION(user_data);
  g_autoptr(GError) error = NULL;

  if (!wig_update_monitor_spawn_restart_helper(&error)) {
    g_warning("restart: staying put, the restart helper did not start: %s", error->message);
    return;
  }

  wig_session_set_restore_on_next_start(app->session);
  g_application_quit(G_APPLICATION(app));
}

static const GActionEntry app_actions[] = {
  { "quit", wig_application_quit_action },
  { "new-window", wig_application_new_window_action },
  { "restart", wig_application_restart_action },
};

static const GActionEntry app_update_actions[] = {
  { "download-update", wig_application_download_update_action },
  { "restart-to-update", wig_application_restart_action },
};

/* Enchant looks dictionaries up by language tag, so the encoding and modifier
 * suffixes a locale name carries have to go, and "C" names no language at all.
 * Handing WebKit no language at all would leave it free to fall back to whatever
 * dictionary happens to be installed first, which underlines every word. */
static void wig_application_enable_spell_checking(WigApplication *app)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  for (const char *const *name = g_get_language_names(); *name; name++) {
    if (g_str_equal(*name, "C") || g_str_equal(*name, "POSIX") || strpbrk(*name, ".@"))
      continue;
    g_strv_builder_add(builder, *name);
  }

  g_auto(GStrv) languages = g_strv_builder_end(builder);
  g_autofree char *joined = g_strjoinv(", ", languages);
  g_debug("spell checking: languages %s", *languages ? joined : "(none)");

  webkit_web_context_set_spell_checking_enabled(app->web_context, TRUE);
  if (*languages)
    webkit_web_context_set_spell_checking_languages(app->web_context, (const char *const *)languages);
}

static void wig_application_initialize_notification_permissions(WigApplication *app)
{
  g_autolist(WebKitSecurityOrigin) allowed = wig_permissions_manager_list_origins(
      app->permissions_manager, WIG_PERMISSION_NOTIFICATION, WEBKIT_PERMISSION_STATE_GRANTED);
  g_autolist(WebKitSecurityOrigin) denied = wig_permissions_manager_list_origins(
      app->permissions_manager, WIG_PERMISSION_NOTIFICATION, WEBKIT_PERMISSION_STATE_DENIED);

  g_debug("permissions: notifications allowed for %u origin(s), denied for %u", g_list_length(allowed),
          g_list_length(denied));
  webkit_web_context_initialize_notification_permissions(app->web_context, allowed, denied);
}

static void wig_user_script_record_free(WigUserScriptRecord *record)
{
  g_free(record->source);
  webkit_user_script_unref(record->script);
  g_free(record);
}

static void wig_user_style_sheet_record_free(WigUserStyleSheetRecord *record)
{
  g_free(record->source);
  webkit_user_style_sheet_unref(record->stylesheet);
  g_free(record);
}

static void wig_application_init(WigApplication *app)
{
  g_autofree char *state_dir = g_build_filename(g_get_user_state_dir(), "com.igalia.wig", NULL);

  app->typed_navigations = g_hash_table_new(g_direct_hash, g_direct_equal);
  app->internal_navigations = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  app->user_scripts = g_ptr_array_new_with_free_func((GDestroyNotify)wig_user_script_record_free);
  app->user_style_sheets = g_ptr_array_new_with_free_func((GDestroyNotify)wig_user_style_sheet_record_free);
  app->notifications = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  app->permissions_manager = wig_permissions_manager_new(state_dir);
}

static void wig_application_update_cookie_policy(WigApplication *app)
{
  WebKitCookieAcceptPolicy policy = (WebKitCookieAcceptPolicy)g_settings_get_enum(app->settings,
                                                                                  "cookie-accept-policy");

  g_debug("cookies: accepting %s",
          policy == WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS      ? "all of them"
              : policy == WEBKIT_COOKIE_POLICY_ACCEPT_NEVER ? "none of them"
                                                            : "only the site being visited");
  webkit_cookie_manager_set_accept_policy(webkit_network_session_get_cookie_manager(app->network_session), policy);
}

static void wig_application_cookie_policy_changed(WigApplication *app)
{
  wig_application_update_cookie_policy(app);
}

/* Ordered as WebKitNetworkProxyMode is. */
static void wig_application_update_proxy_settings(WigApplication *app)
{
  WebKitNetworkProxyMode mode = (WebKitNetworkProxyMode)g_settings_get_enum(app->settings, "proxy-mode");
  g_autofree char *url = g_settings_get_string(app->settings, "proxy-url");
  g_auto(GStrv) ignore_hosts = g_settings_get_strv(app->settings, "proxy-ignore-hosts");

  /* Choosing a custom proxy comes before typing its address, and a custom proxy
   * with nowhere to send anything would quietly become no proxy at all. Until
   * there is an address to use, the system configuration stands. */
  if (mode == WEBKIT_NETWORK_PROXY_MODE_CUSTOM && (!url || !*url)) {
    g_debug("proxy: no proxy URL yet, leaving the system configuration in place");
    mode = WEBKIT_NETWORK_PROXY_MODE_DEFAULT;
  }

  WebKitNetworkProxySettings *proxy_settings = NULL;
  if (mode == WEBKIT_NETWORK_PROXY_MODE_CUSTOM)
    proxy_settings = webkit_network_proxy_settings_new(url, (const char *const *)ignore_hosts);

  g_debug("proxy: %s%s, %u site(s) bypassing it",
          mode == WEBKIT_NETWORK_PROXY_MODE_DEFAULT        ? "as the system is configured"
              : mode == WEBKIT_NETWORK_PROXY_MODE_NO_PROXY ? "none"
                                                           : "through ",
          mode == WEBKIT_NETWORK_PROXY_MODE_CUSTOM ? url : "", ignore_hosts ? g_strv_length(ignore_hosts) : 0);
  webkit_network_session_set_proxy_settings(app->network_session, mode, proxy_settings);
  g_clear_pointer(&proxy_settings, webkit_network_proxy_settings_free);
}

static void wig_application_proxy_settings_changed(WigApplication *app)
{
  wig_application_update_proxy_settings(app);
}

static const char *autoplay_policy_name(WebKitAutoplayPolicy autoplay)
{
  switch (autoplay) {
  case WEBKIT_AUTOPLAY_ALLOW:
    return "anything";
  case WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND:
    return "only what is muted";
  case WEBKIT_AUTOPLAY_DENY:
    return "nothing";
  }

  return "only what is muted";
}

/* Policies are made for each navigation rather than kept and handed out again:
 * they carry what has been decided about the site being navigated to, so two
 * navigations only share one if they are going to the same place. */
WebKitWebsitePolicies *wig_application_create_website_policies(WigApplication *app, const char *uri)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  g_autoptr(WebKitSecurityOrigin) origin = uri ? webkit_security_origin_new_for_uri(uri) : NULL;
  g_autofree char *site = origin ? webkit_security_origin_to_string(origin) : NULL;

  /* Autoplay without sound is what the property carries when nothing is passed,
   * so a site with no rule of its own is left as every other site is. */
  WebKitAutoplayPolicy autoplay = WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND;
  wig_permissions_manager_get_autoplay(app->permissions_manager, site, &autoplay);

  /* No user agent of its own is the default of the property, so a site without
   * one is told whatever every other site is told. */
  const char *user_agent = wig_permissions_manager_get_user_agent(app->permissions_manager, site);

  /* Only a site being treated differently is worth saying anything about; the
   * rest would be a line for every navigation saying nothing happened. */
  if (autoplay != WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND || user_agent)
    g_debug("policies: %s plays %s and is told '%s'", site, autoplay_policy_name(autoplay),
            user_agent ? user_agent : "the usual");

#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  /* The setting says what to do with sites in general, and a site with a rule of
   * its own says what to do with that one. */
  WebKitHTTPSNavigationPolicy https_navigation = (WebKitHTTPSNavigationPolicy)g_settings_get_enum(
      app->settings, "https-navigation-policy");
  wig_permissions_manager_get_https_navigation(app->permissions_manager, site, &https_navigation);

  return webkit_website_policies_new_with_policies("autoplay", autoplay, "custom-user-agent", user_agent,
                                                   "https-navigation-policy", https_navigation, NULL);
#else
  return webkit_website_policies_new_with_policies("autoplay", autoplay, "custom-user-agent", user_agent, NULL);
#endif
}

static void on_notification_clicked_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigApplication *app = WIG_APPLICATION(user_data);
  const char *id = g_variant_get_string(parameter, NULL);
  WebKitNotification *notif = g_hash_table_lookup(app->notifications, id);
  if (notif)
    webkit_notification_clicked(notif);
}

gboolean wig_application_is_quitting(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), FALSE);

  return app->quitting;
}

static void wig_application_begin_quit(WigApplication *app)
{
  if (app->quitting)
    return;

  g_debug("application: quitting");
  app->quitting = TRUE;
  wig_session_set_quitting(app->session);
}

/* The application runs until its last window goes away, so that window leaving
 * is the point where the session stops changing. Saving before chaining up
 * keeps it in the window list, and so in the state that gets written. */
static void wig_application_window_removed(GtkApplication *application, GtkWindow *window)
{
  WigApplication *app = WIG_APPLICATION(application);
  GList *windows = gtk_application_get_windows(application);
  gboolean last = windows && !windows->next && windows->data == window;

  if (last)
    wig_session_save(app->session);

  GTK_APPLICATION_CLASS(wig_application_parent_class)->window_removed(application, window);

  if (last)
    wig_application_begin_quit(app);
  else
    wig_session_save(app->session);
}

/* The window list runs most recently focused first, so prepending leaves the
 * windows in the order they should be brought back in. */
static GSList *wig_application_collect_session_windows(gpointer user_data)
{
  WigApplication *app = WIG_APPLICATION(user_data);
  GSList *windows = NULL;
  WigSessionWindow *most_recent = NULL;
  gboolean any_focused = FALSE;

  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next) {
    if (!WIG_IS_WINDOW(l->data))
      continue;

    WigSessionWindow *captured = wig_window_capture_session(WIG_WINDOW(l->data));
    if (!captured->tabs) {
      wig_session_window_free(captured);
      continue;
    }

    if (!most_recent)
      most_recent = captured;
    any_focused |= captured->focused;
    windows = g_slist_prepend(windows, captured);
  }

  /* No window is active while the browser sits in the background, which is a
   * likely moment for a save; the most recently focused one still stands in. */
  if (!any_focused && most_recent)
    most_recent->focused = TRUE;

  return windows;
}

/* Settings named after a WebKitSettings property drive that property, so a new
 * one of those is a key in the schema, a row in the page, and a name here. */
static const char *const web_settings_keys[] = {
  "javascript-can-open-windows-automatically",
  "auto-load-images",
  "media-playback-requires-user-gesture",
  "zoom-text-only",
  "enable-javascript",
  "enable-developer-extras",
  "enable-webgl",
  "enable-media",
  "enable-encrypted-media",
  "enable-media-stream",
  "enable-webrtc",
  "enable-html5-local-storage",
  "enable-html5-database",
  "user-agent",
};

static void wig_application_startup(GApplication *application)
{
  WigApplication *app = WIG_APPLICATION(application);

  G_APPLICATION_CLASS(wig_application_parent_class)->startup(application);

  g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(provider, "/com/igalia/wig/wig-application.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_action_map_add_action_entries(G_ACTION_MAP(application), app_actions, G_N_ELEMENTS(app_actions), application);

  app->settings = wig_settings_new();

  g_autoptr(GAction) tab_layout_action = g_settings_create_action(app->settings, "tab-layout");
  g_action_map_add_action(G_ACTION_MAP(application), tab_layout_action);

  if (wig_in_flatpak()) {
    app->update_monitor = wig_update_monitor_new();
    g_action_map_add_action_entries(G_ACTION_MAP(application), app_update_actions, G_N_ELEMENTS(app_update_actions),
                                    application);
  }

  g_autoptr(GSimpleAction) notif_clicked_action = g_simple_action_new("notification-clicked", G_VARIANT_TYPE_STRING);
  g_signal_connect(notif_clicked_action, "activate", G_CALLBACK(on_notification_clicked_action), application);
  g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(notif_clicked_action));

  app->display = wpe_display_gtk_new();
  wpe_settings_set_boolean(wpe_display_get_settings(app->display), WPE_SETTING_CREATE_VIEWS_WITH_A_TOPLEVEL, FALSE,
                           WPE_SETTINGS_SOURCE_APPLICATION, NULL);

  g_autoptr(GError) error = NULL;
  if (!wpe_display_connect(app->display, &error)) {
    g_warning("Failed to connect to display: %s", error->message);
    g_application_quit(application);
    return;
  }

  g_autofree char *data_dir = g_build_filename(g_get_user_data_dir(), "com.igalia.wig", NULL);
  g_autofree char *state_dir = g_build_filename(g_get_user_state_dir(), "com.igalia.wig", NULL);
  g_autofree char *cache_dir = g_build_filename(g_get_user_cache_dir(), "com.igalia.wig", NULL);

  /* Only network sessions created after this call pick the settings up. */
  app->memory_pressure_settings = webkit_memory_pressure_settings_new();
  webkit_network_session_set_memory_pressure_settings(app->memory_pressure_settings);

  app->network_session = webkit_network_session_new(data_dir, cache_dir);
  g_autoptr(GError) history_error = NULL;
  app->history_store = wig_history_store_new(state_dir, &history_error);
  if (!app->history_store)
    g_warning("history: disabled: %s", history_error->message);
  g_autoptr(GError) permissions_error = NULL;
  if (!wig_permissions_manager_load(app->permissions_manager, &permissions_error))
    g_warning("persistant permissions disabled: %s", permissions_error->message);

  app->session = wig_session_new(state_dir);
  wig_session_set_collect_func(app->session, wig_application_collect_session_windows, app);
  wig_session_load(app->session);
  webkit_network_session_set_itp_enabled(app->network_session, TRUE);

  g_autofree char *cookies_path = g_build_filename(data_dir, "cookies.sqlite", NULL);
  webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(app->network_session),
                                               cookies_path, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  wig_application_update_cookie_policy(app);
  g_signal_connect_object(app->settings, "changed::cookie-accept-policy",
                          G_CALLBACK(wig_application_cookie_policy_changed), app, G_CONNECT_SWAPPED);
  wig_application_update_proxy_settings(app);
  g_signal_connect_object(app->settings, "changed::proxy-mode", G_CALLBACK(wig_application_proxy_settings_changed), app,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(app->settings, "changed::proxy-url", G_CALLBACK(wig_application_proxy_settings_changed), app,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(app->settings, "changed::proxy-ignore-hosts",
                          G_CALLBACK(wig_application_proxy_settings_changed), app, G_CONNECT_SWAPPED);
#if HAVE_FAVICON_SUPPORT
  webkit_website_data_manager_set_favicons_enabled(
      webkit_network_session_get_website_data_manager(app->network_session), TRUE);
#endif
  app->downloads = wig_downloads_manager_new(app->network_session);
  app->user_content_manager = webkit_user_content_manager_new();
  g_autofree char *filters_dir = g_build_filename(data_dir, "content-filters", NULL);
  app->content_filter_store = webkit_user_content_filter_store_new(filters_dir);
  wig_content_filters_load_saved(app->user_content_manager, app->content_filter_store);
  app->web_context = webkit_web_context_new();
  g_signal_connect_swapped(app->web_context, "initialize-notification-permissions",
                           G_CALLBACK(wig_application_initialize_notification_permissions), app);
  webkit_security_manager_register_uri_scheme_as_empty_document(
      webkit_web_context_get_security_manager(app->web_context), "wig");
  wig_application_enable_spell_checking(app);
  app->web_settings = webkit_settings_new();
  wig_features_apply_overrides(app->web_settings, app->settings);
  for (guint i = 0; i < G_N_ELEMENTS(web_settings_keys); i++)
    g_settings_bind(app->settings, web_settings_keys[i], app->web_settings, web_settings_keys[i], G_SETTINGS_BIND_GET);

  /* The sandbox refuses paths that do not exist yet, and the CDM is only staged
   * there on first use. */
  g_autofree char *widevine_path = g_build_filename(g_get_user_cache_dir(), "widevine", NULL);
  if (g_mkdir_with_parents(widevine_path, 0700) == 0)
    webkit_web_context_add_path_to_sandbox(app->web_context, widevine_path, TRUE);
  else
    g_warning("widevine: could not create '%s': %s", widevine_path, g_strerror(errno));

  static const struct {
    const char *action;
    const char *accels[3];
  } accel_map[] = {
    { "win.focus-entry", { "<Primary>l", NULL } },
    { "win.new-tab", { "<Primary>t", NULL } },
    { "app.new-window", { "<Primary>n", NULL } },
    { "app.quit", { "<Primary>q", NULL } },
    { "win.close-tab", { "<Primary>w", NULL } },
    { "win.reload", { "<Primary>r", "F5", NULL } },
    { "win.reload-bypass-cache", { "<Primary><Shift>r", "<Primary>F5", NULL } },
    { "win.toggle-fullscreen", { "F11", NULL } },
    { "win.zoom-in", { "<Primary>plus", "<Primary>equal", NULL } },
    { "win.zoom-out", { "<Primary>minus", NULL } },
    { "win.zoom-reset", { "<Primary>0", NULL } },
    { "win.undo-close-tab", { "<Primary><Shift>t", NULL } },
    { "win.find", { "<Primary>f", NULL } },
    { "win.find-next", { "<Primary>g", "F3", NULL } },
    { "win.find-previous", { "<Primary><Shift>g", "<Shift>F3", NULL } },
    { "win.show-downloads", { "<Primary>j", NULL } },
    { "win.show-history", { "<Primary>h", NULL } },
    { "win.insert-emoji", { "<Primary>period", NULL } },
    { "win.toggle-inspector", { "F12", NULL } },

  };

  for (gsize i = 0; i < G_N_ELEMENTS(accel_map); i++)
    gtk_application_set_accels_for_action(GTK_APPLICATION(application), accel_map[i].action, accel_map[i].accels);
}

static void wig_application_shutdown(GApplication *application)
{
  WigApplication *app = WIG_APPLICATION(application);

  /* Quitting while windows are still up (app.quit, a signal) reaches here with
   * everything to save still in place. */
  wig_session_save(app->session);
  wig_permissions_manager_save(app->permissions_manager);
  wig_application_begin_quit(app);
  g_clear_object(&app->session);

  g_clear_object(&app->display);
  g_clear_object(&app->network_session);
  g_clear_object(&app->web_context);
  g_clear_object(&app->web_settings);
  g_clear_pointer(&app->memory_pressure_settings, webkit_memory_pressure_settings_free);
  g_clear_object(&app->history_store);
  g_clear_pointer(&app->typed_navigations, g_hash_table_unref);
  g_clear_pointer(&app->internal_navigations, g_hash_table_unref);
  g_clear_object(&app->downloads);
  g_clear_object(&app->settings);
  g_clear_object(&app->user_content_manager);
  g_clear_pointer(&app->user_scripts, g_ptr_array_unref);
  g_clear_pointer(&app->user_style_sheets, g_ptr_array_unref);
  g_clear_object(&app->content_filter_store);
  g_clear_pointer(&app->notifications, g_hash_table_unref);
  g_clear_object(&app->permissions_manager);
  g_clear_object(&app->update_monitor);

  G_APPLICATION_CLASS(wig_application_parent_class)->shutdown(application);
}

static WigSessionWindow *steal_focused_session_window(GSList **windows)
{
  GSList *found = NULL;

  for (GSList *l = *windows; l && !found; l = l->next) {
    WigSessionWindow *window = l->data;
    if (window->focused)
      found = l;
  }

  if (!found)
    found = g_slist_last(*windows);

  if (!found)
    return NULL;

  WigSessionWindow *window = found->data;
  *windows = g_slist_delete_link(*windows, found);
  return window;
}

static gboolean wig_session_window_has_pinned_tab(const WigSessionWindow *window)
{
  for (const GSList *l = window->tabs; l; l = l->next) {
    if (((const WigSessionTab *)l->data)->pinned)
      return TRUE;
  }
  return FALSE;
}

static WigWindow *wig_application_restore_session(WigApplication *app, gboolean pinned_only)
{
  GSList *saved = wig_session_take_restored_windows(app->session);
  g_autoptr(WigSessionWindow) focused_saved = steal_focused_session_window(&saved);
  if (!focused_saved) {
    g_slist_free_full(saved, (GDestroyNotify)wig_session_window_free);
    return NULL;
  }

  wig_session_set_restoring(app->session, TRUE);

  WigWindow *focused = NULL;
  if (!pinned_only || wig_session_window_has_pinned_tab(focused_saved))
    focused = wig_window_restore(app, focused_saved, pinned_only);

  for (GSList *l = saved; l; l = l->next) {
    if (pinned_only && !wig_session_window_has_pinned_tab(l->data))
      continue;

    WigWindow *win = wig_window_restore(app, l->data, pinned_only);
    if (!focused)
      focused = win;
  }

  wig_session_set_restoring(app->session, FALSE);

  g_slist_free_full(saved, (GDestroyNotify)wig_session_window_free);
  return focused;
}

static gboolean wig_application_should_restore_tabs(WigApplication *app)
{
  gboolean restarted = wig_session_take_restore_on_next_start(app->session);

  return restarted || g_settings_get_boolean(app->settings, "restore-tabs");
}

static void wig_application_activate(GApplication *application)
{
  WigApplication *app = WIG_APPLICATION(application);
  WigWindow *win = wig_application_find_browser_window(app);
  gboolean fresh = FALSE;

  if (!win) {
    gboolean restore_tabs = wig_application_should_restore_tabs(app);
    win = wig_application_restore_session(app, !restore_tabs);

    /* A window restored for its pinned tabs alone is still a fresh start, so it
     * gets the tab one would have opened with. */
    if (!win || !restore_tabs) {
      if (!win)
        win = wig_window_new(app);
      wig_application_add_new_tab_with_uri(app, win, "https://wpewebkit.org");
      fresh = TRUE;
    }
  }

  gtk_window_present(GTK_WINDOW(win));
  if (fresh)
    g_action_group_activate_action(G_ACTION_GROUP(win), "focus-entry", NULL);
}

static void wig_application_open(GApplication *application, GFile **files, gint n_files, const gchar *hint)
{
  WigApplication *app = WIG_APPLICATION(application);
  WigWindow *win = wig_application_find_browser_window(app);

  if (!win)
    win = wig_application_restore_session(app, !wig_application_should_restore_tabs(app));

  if (!win)
    win = wig_window_new(app);

  for (int i = 0; i < n_files; i++) {
    g_autofree char *uri = g_file_get_uri(files[i]);
    const char *scheme = g_uri_peek_scheme(uri);
    if (g_strcmp0(scheme, "wig") == 0) {
      char *query = strchr(uri, '?');
      if (query) {
        g_debug("open: stripping query from wig: URI '%s'", uri);
        *query = '\0';
      }
      wig_application_open_internal_page(app, GTK_WINDOW(win), uri);
      continue;
    }
    wig_application_add_new_tab_with_uri(app, win, uri);
  }

  gtk_window_present(GTK_WINDOW(win));
  g_action_group_activate_action(G_ACTION_GROUP(win), "focus-entry", NULL);
}

static void wig_application_class_init(WigApplicationClass *klass)
{
  GApplicationClass *gapplication_class = G_APPLICATION_CLASS(klass);
  gapplication_class->startup = wig_application_startup;
  gapplication_class->activate = wig_application_activate;
  gapplication_class->open = wig_application_open;
  gapplication_class->shutdown = wig_application_shutdown;

  GtkApplicationClass *gtkapplication_class = GTK_APPLICATION_CLASS(klass);
  gtkapplication_class->window_removed = wig_application_window_removed;
}

WigApplication *wig_application_new(void)
{
  return WIG_APPLICATION(g_object_new(WIG_TYPE_APPLICATION, "application-id", "com.igalia.wig", "flags",
                                      G_APPLICATION_HANDLES_OPEN, NULL));
}

WigApplication *wig_application_get(void)
{
  GApplication *app = g_application_get_default();
  if (!WIG_IS_APPLICATION(app))
    g_error("Application singleton is not the default");
  return WIG_APPLICATION(app);
}

WPEDisplay *wig_application_get_display(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->display;
}

WebKitNetworkSession *wig_application_get_network_session(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->network_session;
}

static gboolean on_web_view_decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision,
                                          WebKitPolicyDecisionType decision_type, WigApplication *app)
{
  if (decision_type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
    return FALSE;

  WebKitResponsePolicyDecision *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);

  WebKitURIResponse *uri_response = webkit_response_policy_decision_get_response(response);
  WebKitURIRequest *uri_request = webkit_response_policy_decision_get_request(response);
  const char *mime_type = uri_response ? webkit_uri_response_get_mime_type(uri_response) : NULL;
  const char *response_uri = uri_response ? webkit_uri_response_get_uri(uri_response) : NULL;
  guint status_code = uri_response ? webkit_uri_response_get_status_code(uri_response) : 0;
  gboolean mime_supported = webkit_response_policy_decision_is_mime_type_supported(response);
  gboolean main_resource = webkit_response_policy_decision_is_main_frame_main_resource(response);

  g_debug("[response-policy]   uri=%s request_uri=%s mime=%s status=%u mime_supported=%d main_resource=%d",
          response_uri ? response_uri : "(null)", uri_request ? webkit_uri_request_get_uri(uri_request) : "(null)",
          mime_type ? mime_type : "(null)", status_code, mime_supported, main_resource);

  if (mime_supported) {
    webkit_policy_decision_use(decision);
    return TRUE;
  }

  if (main_resource && mime_type && *mime_type) {
    webkit_policy_decision_download(decision);
    return TRUE;
  }

  webkit_policy_decision_ignore(decision);
  return TRUE;
}

WebKitWebView *wig_application_create_web_view(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  WebKitWebView *web_view = WEBKIT_WEB_VIEW(g_object_new(
      WEBKIT_TYPE_WEB_VIEW, "display", app->display, "web-context", app->web_context, "network-session",
      app->network_session, "settings", app->web_settings, "user-content-manager", app->user_content_manager, NULL));
  g_signal_connect(web_view, "decide-policy", G_CALLBACK(on_web_view_decide_policy), app);
  g_signal_connect(web_view, "load-changed", G_CALLBACK(on_web_view_load_changed), app);
  g_signal_connect(web_view, "load-failed", G_CALLBACK(on_load_failed), app);
  g_signal_connect(web_view, "notify::title", G_CALLBACK(on_history_title_changed), app);
  g_object_weak_ref(G_OBJECT(web_view), history_web_view_finalized, app);

  return web_view;
}

WigHistoryStore *wig_application_get_history_store(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->history_store;
}

WigDownloadsManager *wig_application_get_downloads_manager(WigApplication *app)
{
  return app->downloads;
}

void wig_application_mark_internal_navigation(WigApplication *app, WebKitWebView *web_view, const char *uri)
{
  g_return_if_fail(WIG_IS_APPLICATION(app));
  g_return_if_fail(WEBKIT_IS_WEB_VIEW(web_view));

  if (g_strcmp0(g_uri_peek_scheme(uri), "wig") != 0)
    return;

  g_debug("[mark-internal-navigation] web_view=%p uri=%s", (void *)web_view, uri);
  g_hash_table_insert(app->internal_navigations, web_view, g_strdup(uri));
}

gboolean wig_application_take_internal_navigation(WigApplication *app, WebKitWebView *web_view, const char *uri)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), FALSE);
  g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), FALSE);

  const char *pending = g_hash_table_lookup(app->internal_navigations, web_view);
  if (g_strcmp0(pending, uri) != 0)
    return FALSE;

  g_hash_table_remove(app->internal_navigations, web_view);
  return TRUE;
}

void wig_application_mark_typed_navigation(WigApplication *app, WebKitWebView *web_view, const char *uri)
{
  g_return_if_fail(WIG_IS_APPLICATION(app));
  g_return_if_fail(WEBKIT_IS_WEB_VIEW(web_view));

  g_debug("[mark-typed-navigation] web_view=%p uri=%s recordable=%d", (void *)web_view, uri ? uri : "(null)",
          uri_should_be_recorded(uri));

  if (!uri_should_be_recorded(uri))
    return;

  g_hash_table_insert(app->typed_navigations, web_view, GINT_TO_POINTER(1));
}

GSettings *wig_application_get_settings(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->settings;
}

WebKitSettings *wig_application_get_web_settings(WigApplication *app)
{
  return app->web_settings;
}

WebKitUserContentManager *wig_application_get_user_content_manager(WigApplication *app)
{
  return app->user_content_manager;
}

GPtrArray *wig_application_get_user_scripts(WigApplication *app)
{
  return app->user_scripts;
}

GPtrArray *wig_application_get_user_style_sheets(WigApplication *app)
{
  return app->user_style_sheets;
}

WebKitUserContentFilterStore *wig_application_get_content_filter_store(WigApplication *app)
{
  return app->content_filter_store;
}

WebKitMemoryPressureSettings *wig_application_get_memory_pressure_settings(WigApplication *app)
{
  return app->memory_pressure_settings;
}

WigSession *wig_application_get_session(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->session;
}

void wig_application_track_notification(WigApplication *app, const char *id, WebKitNotification *notif)
{
  g_return_if_fail(WIG_IS_APPLICATION(app));
  g_return_if_fail(id != NULL);
  g_return_if_fail(WEBKIT_IS_NOTIFICATION(notif));

  g_hash_table_insert(app->notifications, g_strdup(id), g_object_ref(notif));
}

void wig_application_untrack_notification(WigApplication *app, const char *id)
{
  g_return_if_fail(WIG_IS_APPLICATION(app));
  g_return_if_fail(id != NULL);

  g_hash_table_remove(app->notifications, id);
}

WigPermissionsManager *wig_application_get_permissions_manager(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->permissions_manager;
}

WigUpdateMonitor *wig_application_get_update_monitor(WigApplication *app)
{
  return app->update_monitor;
}
