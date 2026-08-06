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

#include <tmpl-glib.h>

#include "internal-pages/wig-content-filters.h"
#include "internal-pages/wig-downloads.h"
#include "internal-pages/wig-features.h"
#include "internal-pages/wig-history.h"
#include "internal-pages/wig-internal-page.h"
#include "internal-pages/wig-memory-pressure.h"
#include "internal-pages/wig-user-scripts.h"
#include "internal-pages/wig-user-styles.h"
#include "internal-pages/wig-website-data.h"
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
  GPtrArray *downloads;
  WebKitUserContentManager *user_content_manager;
  GPtrArray *user_scripts;
  GPtrArray *user_style_sheets;
  WebKitUserContentFilterStore *content_filter_store;

  GQueue *closed_tab_history;
  GHashTable *notifications; /* char* -> WebKitNotification* (owned) */
  WigPermissionsManager *permissions_manager;
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

static void on_history_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, WigApplication *app)
{
  g_debug("[load-changed] web_view=%p event=%s (%d) uri=%s", (void *)web_view, wig_load_event_name(load_event),
          load_event, webkit_web_view_get_uri(web_view) ? webkit_web_view_get_uri(web_view) : "(null)");

  if (load_event != WEBKIT_LOAD_COMMITTED)
    return;

  gboolean typed = g_hash_table_remove(app->typed_navigations, web_view);
  record_history_visit(app, web_view, typed);
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

static const GActionEntry app_actions[] = {
  { "quit", wig_application_quit_action },
  { "new-window", wig_application_new_window_action },
};

static void wig_download_record_free(WigDownloadRecord *record)
{
  g_object_unref(record->download);
  g_free(record);
}

static void on_download_finished(WebKitDownload *download, WigDownloadRecord *record)
{
  g_debug("download: finished '%s'", webkit_download_get_destination(download));
  if (record->state == WIG_DOWNLOAD_ACTIVE)
    record->state = WIG_DOWNLOAD_COMPLETE;
}

static void on_download_failed(WebKitDownload *download, GError *error, WigDownloadRecord *record)
{
  g_debug("download: failed '%s': %s", webkit_download_get_destination(download), error->message);
  if (g_error_matches(error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER))
    record->state = WIG_DOWNLOAD_CANCELLED;
  else
    record->state = WIG_DOWNLOAD_FAILED;
}

static gboolean on_decide_destination(WebKitDownload *download, const char *suggested_filename, WigApplication *app)
{
  // FIXME: Show chooser
  if (!suggested_filename || !*suggested_filename) {
    g_warning("download: ignoring download with empty filename");
    webkit_download_cancel(download);
    return TRUE;
  }

  const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  g_autofree char *fallback = dir ? NULL : g_build_filename(g_get_home_dir(), "Downloads", NULL);
  g_autofree char *path = g_build_filename(dir ? dir : fallback, suggested_filename, NULL);
  webkit_download_set_destination(download, path);

  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next) {
    if (wig_window_focus_tab_by_site(WIG_WINDOW(l->data), "wig:downloads"))
      return TRUE;
  }

  GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(app));
  wig_application_add_new_tab_with_uri(app, WIG_WINDOW(active), "wig:downloads");
  return TRUE;
}

static void on_download_started(WebKitNetworkSession *session, WebKitDownload *download, WigApplication *app)
{
  g_debug("download: started '%s'", webkit_uri_request_get_uri(webkit_download_get_request(download)));

  WigDownloadRecord *record = g_new0(WigDownloadRecord, 1);
  record->download = g_object_ref(download);
  g_ptr_array_add(app->downloads, record);

  g_signal_connect(download, "decide-destination", G_CALLBACK(on_decide_destination), app);
  g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), record);
  g_signal_connect(download, "failed", G_CALLBACK(on_download_failed), record);
}

static void wig_application_about_scheme_cb(WebKitURISchemeRequest *request, gpointer user_data)
{
  WigApplication *app = WIG_APPLICATION(user_data);
  const char *uri = webkit_uri_scheme_request_get_uri(request);

  g_debug("wig: scheme handler called for '%s'", uri);

  WebKitWebView *web_view = webkit_uri_scheme_request_get_web_view(request);
  const char *current_uri = webkit_web_view_get_uri(web_view);
  const char *current_scheme = current_uri ? g_uri_peek_scheme(current_uri) : NULL;
  if (g_strcmp0(current_scheme, "wig") != 0) {
    g_warning("wig: rejecting cross-origin request for '%s' from '%s'", uri, current_uri);
    webkit_uri_scheme_request_finish_error(request,
                                           g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                                               "Cross-origin access to wig: URIs is not allowed"));
    return;
  }

  if (g_str_has_prefix(uri, "wig:resources/")) {
    const char *name = uri + strlen("wig:resources/");
    g_autofree char *res_path = g_strconcat("/com/igalia/wig/internal-pages/", name, NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes = g_resources_lookup_data(res_path, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (!bytes) {
      webkit_uri_scheme_request_finish_error(request, g_steal_pointer(&error));
      return;
    }
    gsize size;
    gconstpointer data = g_bytes_get_data(bytes, &size);
    g_autofree char *mime_type = g_content_type_guess(name, data, size, NULL);
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(bytes);
    webkit_uri_scheme_request_finish(request, stream, (goffset)size, mime_type);
    return;
  }

  g_autofree char *html = NULL;
  g_autoptr(TmplScope) scope = NULL;
  if (g_str_equal(uri, "wig:about"))
    html = wig_internal_page_render("/com/igalia/wig/internal-pages/about.html", NULL);
  else if (g_str_has_prefix(uri, "wig:features")) {
    scope = handle_features_uri(request, app->web_settings, FALSE);
    html = wig_internal_page_render("/com/igalia/wig/internal-pages/features.html", scope);
  } else if (g_str_has_prefix(uri, "wig:developer-features")) {
    scope = handle_features_uri(request, app->web_settings, TRUE);
    html = wig_internal_page_render("/com/igalia/wig/internal-pages/features.html", scope);
  } else if (g_str_has_prefix(uri, "wig:memory-pressure")) {
    scope = handle_memory_pressure_uri(request, app->memory_pressure_settings);
    html = wig_internal_page_render("/com/igalia/wig/internal-pages/memory-pressure.html", scope);
  } else if (g_str_has_prefix(uri, "wig:website-data")) {
    WebKitWebsiteDataManager *manager = webkit_network_session_get_website_data_manager(app->network_session);
    handle_website_data_uri(request, manager);
    return; // async
  } else if (g_str_has_prefix(uri, "wig:downloads")) {
    scope = handle_downloads_uri(request, app->downloads);
    html = wig_internal_page_render("/com/igalia/wig/internal-pages/downloads.html", scope);
  } else if (g_str_has_prefix(uri, "wig:history")) {
    scope = handle_history_uri(request, app->history_store);
    html = wig_internal_page_render("/com/igalia/wig/internal-pages/history.html", scope);
  } else if (g_str_has_prefix(uri, "wig:user-scripts")) {
    handle_user_scripts_uri(request, app->user_content_manager, app->user_scripts);
    return; // async
  } else if (g_str_has_prefix(uri, "wig:user-styles")) {
    handle_user_styles_uri(request, app->user_content_manager, app->user_style_sheets);
    return; // async
  } else if (g_str_has_prefix(uri, "wig:content-filters")) {
    handle_content_filters_uri(request, app->user_content_manager, app->content_filter_store);
    return; // async
  } else {
    webkit_uri_scheme_request_finish_error(request, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Not found"));
    return;
  }

  g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_data(g_steal_pointer(&html), -1, g_free);
  webkit_uri_scheme_request_finish(request, stream, -1, "text/html; charset=utf-8");
}

static void wig_application_init(WigApplication *app)
{
  app->closed_tab_history = g_queue_new();
  app->typed_navigations = g_hash_table_new(g_direct_hash, g_direct_equal);
  app->internal_navigations = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  app->downloads = g_ptr_array_new_with_free_func((GDestroyNotify)wig_download_record_free);
  app->user_scripts = g_ptr_array_new_with_free_func((GDestroyNotify)wig_user_script_record_free);
  app->user_style_sheets = g_ptr_array_new_with_free_func((GDestroyNotify)wig_user_style_sheet_record_free);
  app->notifications = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  app->permissions_manager = wig_permissions_manager_new();
}

static void on_notification_clicked_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigApplication *app = WIG_APPLICATION(user_data);
  const char *id = g_variant_get_string(parameter, NULL);
  WebKitNotification *notif = g_hash_table_lookup(app->notifications, id);
  if (notif)
    webkit_notification_clicked(notif);
}

static void wig_application_startup(GApplication *application)
{
  WigApplication *app = WIG_APPLICATION(application);

  G_APPLICATION_CLASS(wig_application_parent_class)->startup(application);

  g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(provider, "/com/igalia/wig/wig-application.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_action_map_add_action_entries(G_ACTION_MAP(application), app_actions, G_N_ELEMENTS(app_actions), application);

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
  app->network_session = webkit_network_session_new(data_dir, cache_dir);
  g_autoptr(GError) history_error = NULL;
  app->history_store = wig_history_store_new(state_dir, &history_error);
  if (!app->history_store)
    g_warning("history: disabled: %s", history_error->message);
  webkit_network_session_set_itp_enabled(app->network_session, TRUE);

  g_autofree char *cookies_path = g_build_filename(data_dir, "cookies.sqlite", NULL);
  webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(app->network_session),
                                               cookies_path, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
#if HAVE_FAVICON_SUPPORT
  webkit_website_data_manager_set_favicons_enabled(
      webkit_network_session_get_website_data_manager(app->network_session), TRUE);
#endif
  app->memory_pressure_settings = webkit_memory_pressure_settings_new();
  webkit_network_session_set_memory_pressure_settings(app->memory_pressure_settings);
  g_signal_connect(app->network_session, "download-started", G_CALLBACK(on_download_started), app);
  app->user_content_manager = webkit_user_content_manager_new();
  g_autofree char *filters_dir = g_build_filename(data_dir, "content-filters", NULL);
  app->content_filter_store = webkit_user_content_filter_store_new(filters_dir);
  app->web_context = webkit_web_context_new();
  webkit_web_context_register_uri_scheme(app->web_context, "wig", wig_application_about_scheme_cb, app, NULL);
  webkit_security_manager_register_uri_scheme_as_no_access(webkit_web_context_get_security_manager(app->web_context),
                                                           "wig");
  app->web_settings = webkit_settings_new_with_settings("enable-developer-extras", TRUE, NULL);

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

  };

  for (gsize i = 0; i < G_N_ELEMENTS(accel_map); i++)
    gtk_application_set_accels_for_action(GTK_APPLICATION(application), accel_map[i].action, accel_map[i].accels);
}

static void wig_application_shutdown(GApplication *application)
{
  WigApplication *app = WIG_APPLICATION(application);

  g_queue_free_full(app->closed_tab_history, (GDestroyNotify)wig_closed_group_free);
  app->closed_tab_history = NULL;

  g_clear_object(&app->display);
  g_clear_object(&app->network_session);
  g_clear_object(&app->web_context);
  g_clear_object(&app->web_settings);
  g_clear_pointer(&app->memory_pressure_settings, webkit_memory_pressure_settings_free);
  g_clear_object(&app->history_store);
  g_clear_pointer(&app->typed_navigations, g_hash_table_unref);
  g_clear_pointer(&app->internal_navigations, g_hash_table_unref);
  g_clear_pointer(&app->downloads, g_ptr_array_unref);
  g_clear_object(&app->user_content_manager);
  g_clear_pointer(&app->user_scripts, g_ptr_array_unref);
  g_clear_pointer(&app->user_style_sheets, g_ptr_array_unref);
  g_clear_object(&app->content_filter_store);
  g_clear_pointer(&app->notifications, g_hash_table_unref);
  g_clear_object(&app->permissions_manager);

  G_APPLICATION_CLASS(wig_application_parent_class)->shutdown(application);
}

static void wig_application_activate(GApplication *application)
{
  WigApplication *app = WIG_APPLICATION(application);
  WigWindow *win = WIG_WINDOW(gtk_application_get_active_window(GTK_APPLICATION(app)));

  if (!win) {
    win = wig_window_new(app);
    wig_application_add_new_tab_with_uri(app, win, "https://wpewebkit.org");
  }

  gtk_window_present(GTK_WINDOW(win));
  g_action_group_activate_action(G_ACTION_GROUP(win), "focus-entry", NULL);
}

static void wig_application_open(GApplication *application, GFile **files, gint n_files, const gchar *hint)
{
  WigApplication *app = WIG_APPLICATION(application);
  WigWindow *win = WIG_WINDOW(gtk_application_get_active_window(GTK_APPLICATION(app)));

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
  g_signal_connect(web_view, "load-changed", G_CALLBACK(on_history_load_changed), app);
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

static void wig_closed_tab_free(WigClosedTab *tab)
{
  webkit_web_view_session_state_unref(tab->state);
  g_free(tab);
}

void wig_closed_group_free(WigClosedGroup *group)
{
  if (!group)
    return;
  g_slist_free_full(group->tabs, (GDestroyNotify)wig_closed_tab_free);
  g_free(group);
}

void wig_application_push_closed_group(WigApplication *app, WigClosedGroup *group)
{
  g_return_if_fail(WIG_IS_APPLICATION(app));
  g_return_if_fail(group != NULL);

  g_debug("Pushing closed group of size %d for window %d", g_slist_length(group->tabs), group->window_id);
  g_queue_push_tail(app->closed_tab_history, group);

  if (g_queue_get_length(app->closed_tab_history) > 20)
    wig_closed_group_free(g_queue_pop_head(app->closed_tab_history));
}

WigClosedGroup *wig_application_pop_closed_group(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  WigClosedGroup *group = g_queue_pop_tail(app->closed_tab_history);
  if (group)
    g_debug("Popping closed group of size %d for window %d", g_slist_length(group->tabs), group->window_id);
  return group;
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
