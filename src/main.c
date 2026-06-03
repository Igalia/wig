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
#include "wig-window.h"
#include "wpe-display-gtk.h"
#include "wpe-view-gtk.h"
#include <adwaita.h>
#include <gtk/gtk.h>
#include <wpe/webkit.h>

static const gchar **uri_args;

static const GOptionEntry cmd_options[] = {
  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &uri_args, 0, "[URL…]" },
  { NULL, 0, 0, 0, NULL, 0, NULL },
};

static void activate(GApplication *application)
{
  WigApplication *app = WIG_APPLICATION(application);

  WebKitNetworkSession *network_session = wig_application_get_network_session(app);
  webkit_network_session_set_itp_enabled(network_session, TRUE);

  GtkWindow *win = GTK_WINDOW(wig_window_new());
  gtk_window_set_application(win, GTK_APPLICATION(application));

  if (uri_args) {
    int i;

    for (i = 0; uri_args[i] != NULL; i++) {
      WebKitWebView *web_view = wig_application_create_web_view(app);
      wig_window_add_web_view(WIG_WINDOW(win), web_view);

      GFile *file = g_file_new_for_commandline_arg(uri_args[i]);
      gchar *url = g_file_get_uri(file);
      webkit_web_view_load_uri(web_view, url);
      g_free(url);
      g_object_unref(file);
      g_object_unref(web_view);
    }
  } else {
    WebKitWebView *web_view = wig_application_create_web_view(app);
    wig_window_add_web_view(WIG_WINDOW(win), web_view);
    webkit_web_view_load_uri(web_view, "https://wpewebkit.org");
    g_object_unref(web_view);
  }

  gtk_window_present(win);
  g_action_group_activate_action(G_ACTION_GROUP(win), "focus-entry", NULL);
}

int main(int argc, char **argv)
{
  GOptionContext *context = g_option_context_new(NULL);
  g_option_context_add_main_entries(context, cmd_options, NULL);

  GError *error = NULL;
  if (!g_option_context_parse(context, &argc, &argv, &error)) {
    g_printerr("Cannot parse arguments: %s\n", error->message);
    g_error_free(error);
    g_option_context_free(context);
    return 1;
  }
  g_option_context_free(context);

  WigApplication *app = wig_application_new();
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_application_run(G_APPLICATION(app), 0, NULL);
  g_object_unref(app);

  return 0;
}
