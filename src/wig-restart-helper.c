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
 * Starts the browser again once it has gone, whether it left to pick up a newly deployed
 * flatpak commit or for any other reason. The browser launches this and quits; it cannot ask
 * for the replacement itself, because activating a name that is still owned reaches the
 * instance on its way out instead of starting a new one. This waits for the name to be
 * released, then activates it.
 *
 * Only the name matters, not the browser process: that it is still unwinding is welcome, as
 * the sandbox lives exactly as long as it does.
 */

#include <gio/gio.h>
#include <stdlib.h>

#define WIG_APPLICATION_ID "com.igalia.wig"

#define WIG_APPLICATION_OBJECT_PATH "/com/igalia/wig"
#define WIG_APPLICATION_INTERFACE "org.freedesktop.Application"

#define WIG_EXIT_TIMEOUT_SECONDS 60

typedef struct {
  GMainLoop *loop;
  guint timeout_id;
  gboolean activated;
} RestartHelper;

/* A build that has not been installed has no service file for the session to find
 * the browser by, but it is sitting right next to this. */
static gboolean spawn_browser(void)
{
  g_autofree char *self = g_file_read_link("/proc/self/exe", NULL);
  if (!self)
    return FALSE;

  g_autofree char *directory = g_path_get_dirname(self);
  g_autofree char *browser = g_build_filename(directory, "wig", NULL);
  if (!g_file_test(browser, G_FILE_TEST_IS_EXECUTABLE)) {
    g_warning("no browser next to %s to start", self);
    return FALSE;
  }

  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) browser_process = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, browser, NULL);
  if (!browser_process) {
    g_warning("could not start %s: %s", browser, error->message);
    return FALSE;
  }

  g_message("started %s", browser);
  return TRUE;
}

static void browser_activated(GObject *source, GAsyncResult *result, gpointer user_data)
{
  RestartHelper *helper = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

  if (reply) {
    helper->activated = TRUE;
    g_message("the browser has been activated again");
  } else {
    g_debug("the session would not activate the browser: %s", error->message);
    helper->activated = spawn_browser();
  }

  g_main_loop_quit(helper->loop);
}

/* Activating the name rather than spawning a command leaves the launch to the session, which
 * runs the exported service file and so gives the new instance the environment any other
 * launch would get. It picks up the newly deployed commit because that is what running the
 * application now resolves to. */
static void activate_browser(GDBusConnection *connection, RestartHelper *helper)
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
  RestartHelper *helper = user_data;

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
  RestartHelper *helper = user_data;

  g_warning("%s did not exit within %d seconds, not restarting it", WIG_APPLICATION_ID, WIG_EXIT_TIMEOUT_SECONDS);
  helper->timeout_id = 0;
  g_main_loop_quit(helper->loop);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
  RestartHelper helper = { .loop = g_main_loop_new(NULL, FALSE) };

  guint watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION, WIG_APPLICATION_ID, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    name_appeared, name_vanished, &helper, NULL);
  helper.timeout_id = g_timeout_add_seconds(WIG_EXIT_TIMEOUT_SECONDS, exit_timed_out, &helper);

  g_main_loop_run(helper.loop);

  g_clear_handle_id(&helper.timeout_id, g_source_remove);
  g_bus_unwatch_name(watch_id);
  g_main_loop_unref(helper.loop);

  return helper.activated ? EXIT_SUCCESS : EXIT_FAILURE;
}
