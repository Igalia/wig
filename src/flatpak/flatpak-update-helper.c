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

/*
 * Restarts the browser into a newly deployed flatpak commit. The browser launches this and
 * quits; it cannot ask for the replacement itself, because activating a name that is still
 * owned reaches the instance on its way out instead of starting a new one. This waits for the
 * name to be released, then activates it.
 *
 * Only the name matters, not the browser process: that it is still unwinding is welcome, as
 * the sandbox lives exactly as long as it does.
 *
 * A window stands in for the browser meanwhile, so the restart is not a stretch of empty
 * screen. It is shown for as long as this runs, which is until the replacement is up.
 */

#include "wig-flatpak.h"

#include <gtk/gtk.h>
#include <stdlib.h>

#define WIG_APPLICATION_ID "com.igalia.wig"

#define WIG_APPLICATION_OBJECT_PATH "/com/igalia/wig"
#define WIG_APPLICATION_INTERFACE "org.freedesktop.Application"

#define WIG_EXIT_TIMEOUT_SECONDS 60

/* Long enough that a restart which is over quickly never puts a window on screen at all,
 * short enough that a slower one is not a stretch of nothing. */
#define WIG_WINDOW_DELAY_SECONDS 3

typedef struct {
  GMainLoop *loop;
  GtkWidget *window;
  guint timeout_id;
  guint window_timeout_id;
  gboolean activated;
} UpdateHelper;

static gboolean present_restarting_window(gpointer user_data)
{
  UpdateHelper *helper = user_data;

  helper->window_timeout_id = 0;

  helper->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(helper->window), "wig");
  gtk_window_set_default_size(GTK_WINDOW(helper->window), 360, 160);
  gtk_window_set_resizable(GTK_WINDOW(helper->window), FALSE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_size_request(spinner, 32, 32);
  gtk_spinner_start(GTK_SPINNER(spinner));
  gtk_box_append(GTK_BOX(box), spinner);

  GtkWidget *label = gtk_label_new("Restarting wig…");
  gtk_box_append(GTK_BOX(box), label);

  gtk_window_set_child(GTK_WINDOW(helper->window), box);
  gtk_window_present(GTK_WINDOW(helper->window));

  return G_SOURCE_REMOVE;
}

static void browser_activated(GObject *source, GAsyncResult *result, gpointer user_data)
{
  UpdateHelper *helper = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

  if (reply) {
    helper->activated = TRUE;
    g_message("the updated browser has been activated");
  } else {
    g_warning("failed to start the updated browser: %s", error->message);
  }

  g_main_loop_quit(helper->loop);
}

/* Activating the name rather than spawning a command leaves the launch to the session, which
 * runs the exported service file and so gives the new instance the environment any other
 * launch would get. It picks up the newly deployed commit because that is what running the
 * application now resolves to.
 *
 * The call is left to complete on its own so that the window keeps drawing while the session
 * brings the browser back, which is the part that takes a noticeable moment. */
static void activate_browser(GDBusConnection *connection, UpdateHelper *helper)
{
  GVariantBuilder platform_data;
  g_variant_builder_init(&platform_data, G_VARIANT_TYPE_VARDICT);

  g_dbus_connection_call(connection, WIG_APPLICATION_ID, WIG_APPLICATION_OBJECT_PATH, WIG_APPLICATION_INTERFACE,
                         "Activate", g_variant_new("(@a{sv})", g_variant_builder_end(&platform_data)), NULL,
                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, browser_activated, helper);
}

static void name_appeared(GDBusConnection *connection, const char *name, const char *name_owner, gpointer user_data)
{
  g_debug("waiting for %s (%s) to exit", name, name_owner);
}

static void name_vanished(GDBusConnection *connection, const char *name, gpointer user_data)
{
  UpdateHelper *helper = user_data;

  if (!connection) {
    g_warning("no session bus, cannot restart the browser");
    g_main_loop_quit(helper->loop);
    return;
  }

  g_debug("%s left the bus", name);

  /* The deadline was for the browser leaving, which it just did. Bringing the replacement up
   * takes as long as it takes, and cutting that short would abandon a restart in progress. */
  g_clear_handle_id(&helper->timeout_id, g_source_remove);

  activate_browser(connection, helper);
}

static gboolean exit_timed_out(gpointer user_data)
{
  UpdateHelper *helper = user_data;

  g_warning("%s did not exit within %d seconds, not restarting it", WIG_APPLICATION_ID, WIG_EXIT_TIMEOUT_SECONDS);
  helper->timeout_id = 0;
  g_main_loop_quit(helper->loop);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
  if (!wig_in_flatpak()) {
    g_warning("only useful from inside a flatpak, where the browser can be restarted");
    return EXIT_FAILURE;
  }

  UpdateHelper helper = { .loop = g_main_loop_new(NULL, FALSE) };

  /* Not fatal without a display: the restart is worth doing either way. */
  if (gtk_init_check())
    helper.window_timeout_id = g_timeout_add_seconds(WIG_WINDOW_DELAY_SECONDS, present_restarting_window, &helper);
  else
    g_debug("no display, restarting without a window");

  guint watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION, WIG_APPLICATION_ID, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    name_appeared, name_vanished, &helper, NULL);
  helper.timeout_id = g_timeout_add_seconds(WIG_EXIT_TIMEOUT_SECONDS, exit_timed_out, &helper);

  g_main_loop_run(helper.loop);

  g_clear_handle_id(&helper.timeout_id, g_source_remove);
  g_clear_handle_id(&helper.window_timeout_id, g_source_remove);
  g_bus_unwatch_name(watch_id);
  if (helper.window)
    gtk_window_destroy(GTK_WINDOW(helper.window));
  g_main_loop_unref(helper.loop);

  return helper.activated ? EXIT_SUCCESS : EXIT_FAILURE;
}
