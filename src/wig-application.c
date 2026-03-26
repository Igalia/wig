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
#include "wpe-display-gtk.h"

struct _WigApplication {
  AdwApplication parent;

  WPEDisplay *display;
  WebKitNetworkSession *network_session;
  WebKitWebContext *web_context;
  WebKitSettings *web_settings;
};

G_DEFINE_FINAL_TYPE(WigApplication, wig_application, ADW_TYPE_APPLICATION)

static void wig_application_init(WigApplication *app)
{
}

static void wig_application_startup(GApplication* application)
{
  WigApplication *app = WIG_APPLICATION(application);

  G_APPLICATION_CLASS(wig_application_parent_class)->startup(application);

  app->display = wpe_display_gtk_new();
  wpe_settings_set_boolean(wpe_display_get_settings(app->display), WPE_SETTING_CREATE_VIEWS_WITH_A_TOPLEVEL, FALSE, WPE_SETTINGS_SOURCE_APPLICATION, NULL);

  GError *error = NULL;
  if (!wpe_display_connect(app->display, &error)) {
    g_warning("Failed to connect to display: %s", error->message);
    g_error_free(error);
    g_application_quit(application);
    return;
  }

  char *data_dir = g_build_filename(g_get_user_data_dir(), "wpeplatformgtk", "demo", NULL);
  char *cache_dir = g_build_filename(g_get_user_cache_dir(), "wpeplatformgtk", "demo", NULL);
  app->network_session = webkit_network_session_new(data_dir, cache_dir);
  g_free(data_dir);
  g_free(cache_dir);

  app->web_context = webkit_web_context_new();

  app->web_settings = webkit_settings_new_with_settings("enable-developer-extras", TRUE, NULL);
}

static void wig_application_shutdown(GApplication* application)
{
  WigApplication *app = WIG_APPLICATION(application);
  g_clear_object(&app->display);
  g_clear_object(&app->network_session);
  g_clear_object(&app->web_context);
  g_clear_object(&app->web_settings);

  G_APPLICATION_CLASS(wig_application_parent_class)->shutdown(application);
}

static void wig_application_class_init(WigApplicationClass *klass)
{
  GApplicationClass *gapplication_class = G_APPLICATION_CLASS(klass);
  gapplication_class->startup = wig_application_startup;
  gapplication_class->shutdown = wig_application_shutdown;
}

WigApplication *wig_application_new(void)
{
  return WIG_APPLICATION(g_object_new(WIG_TYPE_APPLICATION,
                                     "application-id", "org.wpePlatformGtk.Demo",
                                     "flags", G_APPLICATION_NON_UNIQUE,
                                     NULL));
}

WigApplication *wig_application_get(void)
{
  GApplication *app = g_application_get_default();
  if (!WIG_IS_APPLICATION(app))
    g_error("Application singleton is not the default");
  return WIG_APPLICATION(app);
}

WPEDisplay *wig_application_get_display(WigApplication* app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->display;
}

WebKitNetworkSession *wig_application_get_network_session(WigApplication* app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->network_session;
}

WebKitWebContext *wig_application_get_web_context(WigApplication* app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->web_context;
}

WebKitSettings *wig_application_get_web_settings(WigApplication* app)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(app), NULL);

  return app->web_settings;
}
