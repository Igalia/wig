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
#include "internal-pages/wig-features.h"
#include "internal-pages/wig-internal-page.h"
#include "internal-pages/wig-memory-pressure.h"
#include "wig-window.h"
#include "wpe-display-gtk.h"
#include <tmpl-glib.h>

struct _WigApplication {
  AdwApplication parent;

  WPEDisplay *display;
  WebKitNetworkSession *network_session;
  WebKitWebContext *web_context;
  WebKitSettings *web_settings;
  WebKitMemoryPressureSettings *memory_pressure_settings;

  GQueue *closed_tab_history;
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
  webkit_web_view_load_uri(web_view, uri);
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

static void wig_application_about_scheme_cb(WebKitURISchemeRequest *request, gpointer user_data)
{
  WigApplication *app = WIG_APPLICATION(user_data);
  const char *uri = webkit_uri_scheme_request_get_uri(request);

  g_debug("wig: scheme handler called for '%s'", uri);

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
}

static void wig_application_startup(GApplication *application)
{
  WigApplication *app = WIG_APPLICATION(application);

  G_APPLICATION_CLASS(wig_application_parent_class)->startup(application);

  g_action_map_add_action_entries(G_ACTION_MAP(application), app_actions, G_N_ELEMENTS(app_actions), application);

  app->display = wpe_display_gtk_new();
  wpe_settings_set_boolean(wpe_display_get_settings(app->display), WPE_SETTING_CREATE_VIEWS_WITH_A_TOPLEVEL, FALSE,
                           WPE_SETTINGS_SOURCE_APPLICATION, NULL);

  GError *error = NULL;
  if (!wpe_display_connect(app->display, &error)) {
    g_warning("Failed to connect to display: %s", error->message);
    g_error_free(error);
    g_application_quit(application);
    return;
  }

  g_autofree char *data_dir = g_build_filename(g_get_user_data_dir(), "com.igalia.wig", NULL);
  g_autofree char *cache_dir = g_build_filename(g_get_user_cache_dir(), "com.igalia.wig", NULL);
  app->network_session = webkit_network_session_new(data_dir, cache_dir);
  webkit_network_session_set_itp_enabled(app->network_session, TRUE);
  app->memory_pressure_settings = webkit_memory_pressure_settings_new();
  webkit_network_session_set_memory_pressure_settings(app->memory_pressure_settings);
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

WebKitWebContext *wig_application_get_web_context(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->web_context;
}

WebKitSettings *wig_application_get_web_settings(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->web_settings;
}

WebKitWebView *wig_application_create_web_view(WigApplication *app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", app->display, "web-context", app->web_context,
                                      "network-session", app->network_session, "settings", app->web_settings, NULL));
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
