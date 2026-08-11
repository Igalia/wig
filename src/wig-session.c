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

#include "wig-session.h"

#include "wig-key-file-utils.h"

#include <glib/gstdio.h>

#define WIG_SESSION_MAX_CLOSED_WINDOWS 20
#define WIG_SESSION_SAVE_DELAY_SECONDS 5
#define WIG_SESSION_FORMAT_VERSION 1
#define WIG_SESSION_GROUP "Session"

struct _WigSession {
  GObject parent;

  char *path;
  GQueue *closed_windows; /* owned WigSessionWindow*, oldest first */
  GSList *restored_windows; /* owned WigSessionWindow*, from the last load */

  WigSessionCollectFunc collect_func;
  gpointer collect_data;
  guint save_timeout_id;
  gboolean quitting;
  gboolean restoring;
  gboolean restore_on_next_start;
};

G_DEFINE_FINAL_TYPE(WigSession, wig_session, G_TYPE_OBJECT)

static void wig_session_tab_free(WigSessionTab *tab)
{
  webkit_web_view_session_state_unref(tab->state);
  g_free(tab);
}

WigSessionWindow *wig_session_window_new(guint window_id)
{
  WigSessionWindow *self = g_new0(WigSessionWindow, 1);
  self->window_id = window_id;
  return self;
}

/* Takes ownership of @state. A view that never loaded anything has no state and
 * is not worth reopening, so it is dropped rather than kept as a blank tab. */
void wig_session_window_add_tab(WigSessionWindow *self, WebKitWebViewSessionState *state, gboolean was_focused,
                                gboolean pinned)
{
  g_assert(self != NULL);

  if (!state)
    return;

  WigSessionTab *tab = g_new0(WigSessionTab, 1);
  tab->state = state;
  tab->was_focused = was_focused;
  tab->pinned = pinned;
  self->tabs = g_slist_append(self->tabs, tab);
}

void wig_session_window_free(WigSessionWindow *self)
{
  if (!self)
    return;

  g_slist_free_full(self->tabs, (GDestroyNotify)wig_session_tab_free);
  g_free(self->monitor);
  g_free(self);
}

static void wig_session_window_list_free(GSList *windows)
{
  g_slist_free_full(windows, (GDestroyNotify)wig_session_window_free);
}

static char *wig_session_group_name(const char *prefix, guint index)
{
  return g_strdup_printf("%s %u", prefix, index);
}

static gboolean wig_session_tab_to_key_file(GKeyFile *key_file, const WigSessionTab *tab, const char *prefix,
                                            guint index)
{
  g_autoptr(GBytes) bytes = webkit_web_view_session_state_serialize(tab->state);
  if (!bytes)
    return FALSE;

  g_autofree char *group = wig_session_group_name(prefix, index);
  gsize size;
  const guint8 *data = g_bytes_get_data(bytes, &size);
  g_autofree char *state = g_base64_encode(data, size);
  g_key_file_set_boolean(key_file, group, "Focused", tab->was_focused);
  if (tab->pinned)
    g_key_file_set_boolean(key_file, group, "Pinned", TRUE);
  g_key_file_set_string(key_file, group, "State", state);
  return TRUE;
}

static gboolean wig_session_window_to_key_file(GKeyFile *key_file, const WigSessionWindow *window,
                                               const char *window_prefix, guint window_index, const char *tab_prefix,
                                               guint *tab_index)
{
  g_autofree char *group = wig_session_group_name(window_prefix, window_index);
  g_key_file_set_uint64(key_file, group, "Id", window->window_id);
  g_key_file_set_boolean(key_file, group, "Focused", window->focused);
  g_key_file_set_boolean(key_file, group, "Maximized", window->maximized);
  g_key_file_set_boolean(key_file, group, "Fullscreen", window->fullscreen);
  g_key_file_set_boolean(key_file, group, "Minimized", window->minimized);
  g_key_file_set_integer(key_file, group, "Width", window->width);
  g_key_file_set_integer(key_file, group, "Height", window->height);
  g_key_file_set_integer(key_file, group, "SidebarWidth", window->sidebar_width);
  if (window->fullscreen)
    g_key_file_set_string(key_file, group, "Monitor", window->monitor ? window->monitor : "");

  g_autoptr(GArray) tab_indexes = g_array_new(FALSE, FALSE, sizeof(int));
  for (const GSList *l = window->tabs; l; l = l->next) {
    if (!wig_session_tab_to_key_file(key_file, l->data, tab_prefix, *tab_index))
      continue;

    int index = (int)(*tab_index)++;
    g_array_append_val(tab_indexes, index);
  }
  if (!tab_indexes->len) {
    g_key_file_remove_group(key_file, group, NULL);
    return FALSE;
  }
  g_key_file_set_integer_list(key_file, group, "Tabs", &g_array_index(tab_indexes, int, 0), tab_indexes->len);
  return TRUE;
}

static WebKitWebViewSessionState *wig_session_tab_state_from_key_file(GKeyFile *key_file, const char *prefix, int index,
                                                                      gboolean *was_focused, gboolean *pinned)
{
  *was_focused = FALSE;
  *pinned = FALSE;

  g_autofree char *group = wig_session_group_name(prefix, (guint)index);
  g_autoptr(GError) error = NULL;
  g_autofree char *encoded_state = g_key_file_get_string(key_file, group, "State", &error);
  if (!encoded_state) {
    g_warning("session: discarding %s: %s", group, error->message);
    return NULL;
  }

  gsize size;
  g_autofree guint8 *data = g_base64_decode(encoded_state, &size);
  if (!size) {
    g_warning("session: discarding %s with invalid state encoding", group);
    return NULL;
  }

  g_autoptr(GBytes) bytes = g_bytes_new(data, size);
  WebKitWebViewSessionState *state = webkit_web_view_session_state_new(bytes);
  if (!state) {
    g_warning("session: discarding %s with unreadable state", group);
    return NULL;
  }

  *was_focused = wig_key_file_get_boolean(key_file, group, "Focused", FALSE);
  *pinned = wig_key_file_get_boolean(key_file, group, "Pinned", FALSE);
  return state;
}

static WigSessionWindow *wig_session_window_from_key_file(GKeyFile *key_file, const char *window_prefix,
                                                          guint window_index, const char *tab_prefix,
                                                          GHashTable *seen_tabs)
{
  g_autofree char *group = wig_session_group_name(window_prefix, window_index);
  g_autoptr(GError) error = NULL;
  guint64 window_id = g_key_file_get_uint64(key_file, group, "Id", &error);
  if (error || window_id > G_MAXUINT) {
    g_warning("session: discarding %s with invalid id: %s", group, error ? error->message : "out of range");
    return NULL;
  }

  gsize n_tabs;
  g_autofree int *tab_indexes = g_key_file_get_integer_list(key_file, group, "Tabs", &n_tabs, &error);
  if (!tab_indexes) {
    g_warning("session: discarding %s: %s", group, error ? error->message : "no tabs");
    return NULL;
  }

  WigSessionWindow *window = wig_session_window_new((guint)window_id);
  window->focused = wig_key_file_get_boolean(key_file, group, "Focused", FALSE);
  window->maximized = wig_key_file_get_boolean(key_file, group, "Maximized", FALSE);
  window->fullscreen = wig_key_file_get_boolean(key_file, group, "Fullscreen", FALSE);
  window->minimized = wig_key_file_get_boolean(key_file, group, "Minimized", FALSE);
  window->width = wig_key_file_get_integer(key_file, group, "Width", 0);
  window->height = wig_key_file_get_integer(key_file, group, "Height", 0);
  window->sidebar_width = wig_key_file_get_integer(key_file, group, "SidebarWidth", 0);
  window->monitor = g_key_file_get_string(key_file, group, "Monitor", NULL);
  if (window->monitor && !*window->monitor)
    g_clear_pointer(&window->monitor, g_free);

  for (gsize i = 0; i < n_tabs; i++) {
    gpointer tab_key = GINT_TO_POINTER(tab_indexes[i]);
    if (tab_indexes[i] < 0 || g_hash_table_contains(seen_tabs, tab_key)) {
      g_warning("session: discarding invalid tab reference %d from %s", tab_indexes[i], group);
      continue;
    }
    g_hash_table_add(seen_tabs, tab_key);

    gboolean was_focused;
    gboolean pinned;
    WebKitWebViewSessionState *state = wig_session_tab_state_from_key_file(key_file, tab_prefix, tab_indexes[i],
                                                                           &was_focused, &pinned);
    wig_session_window_add_tab(window, state, was_focused, pinned);
  }
  return window;
}

static GSList *wig_session_window_list_from_key_file(GKeyFile *key_file, const char *count_key,
                                                     const char *window_prefix, const char *tab_prefix,
                                                     gboolean unique_window_ids)
{
  int count = wig_key_file_get_integer(key_file, WIG_SESSION_GROUP, count_key, 0);
  if (count <= 0) {
    if (count < 0)
      g_warning("session: ignoring negative %s.%s count %d", WIG_SESSION_GROUP, count_key, count);
    return NULL;
  }

  /* Every window carries a group of its own, so the number of groups is an upper
   * bound on the count a sane file can claim. */
  gsize n_groups;
  g_auto(GStrv) groups = g_key_file_get_groups(key_file, &n_groups);
  if ((gsize)count > n_groups) {
    g_warning("session: %s.%s claims %d window(s) but only %" G_GSIZE_FORMAT " group(s) exist", WIG_SESSION_GROUP,
              count_key, count, n_groups);
    count = (int)n_groups;
  }

  GSList *windows = NULL;
  g_autoptr(GHashTable) seen_tabs = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_autoptr(GHashTable) seen_window_ids = unique_window_ids ? g_hash_table_new(g_direct_hash, g_direct_equal) : NULL;
  for (int i = 0; i < count; i++) {
    WigSessionWindow *window = wig_session_window_from_key_file(key_file, window_prefix, (guint)i, tab_prefix,
                                                                seen_tabs);
    if (!window || !window->tabs) {
      wig_session_window_free(window);
      continue;
    }

    if (seen_window_ids && !g_hash_table_add(seen_window_ids, GUINT_TO_POINTER(window->window_id))) {
      g_warning("session: discarding duplicate open window id %u", window->window_id);
      wig_session_window_free(window);
      continue;
    }

    windows = g_slist_prepend(windows, window);
  }
  return g_slist_reverse(windows);
}

static void wig_session_write(WigSession *self, GSList *open_windows)
{
  g_assert(open_windows != NULL);

  g_autofree char *dir = g_path_get_dirname(self->path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_warning("session: cannot create '%s': %s", dir, g_strerror(errno));
    return;
  }

  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_key_file_set_integer(key_file, WIG_SESSION_GROUP, "Version", WIG_SESSION_FORMAT_VERSION);
  if (self->restore_on_next_start)
    g_key_file_set_boolean(key_file, WIG_SESSION_GROUP, "RestoreOnNextStart", TRUE);

  guint n_open = 0;
  guint n_open_tabs = 0;
  for (GSList *l = open_windows; l; l = l->next) {
    if (wig_session_window_to_key_file(key_file, l->data, "Window", n_open, "Tab", &n_open_tabs))
      n_open++;
  }
  g_key_file_set_integer(key_file, WIG_SESSION_GROUP, "Windows", (int)n_open);

  guint n_closed = 0;
  guint n_closed_tabs = 0;
  for (GList *l = self->closed_windows->head; l; l = l->next) {
    if (wig_session_window_to_key_file(key_file, l->data, "ClosedWindow", n_closed, "ClosedTab", &n_closed_tabs))
      n_closed++;
  }
  g_key_file_set_integer(key_file, WIG_SESSION_GROUP, "ClosedWindows", (int)n_closed);

  g_autoptr(GError) error = NULL;
  if (!wig_key_file_save(key_file, self->path, &error)) {
    g_warning("session: cannot write '%s': %s", self->path, error->message);
    return;
  }

  g_debug("session: wrote %u window(s) and %u closed window(s) to '%s'", n_open, n_closed, self->path);
}

static void wig_session_dispose(GObject *object)
{
  WigSession *self = WIG_SESSION(object);

  g_clear_handle_id(&self->save_timeout_id, g_source_remove);

  if (self->closed_windows) {
    g_queue_free_full(self->closed_windows, (GDestroyNotify)wig_session_window_free);
    self->closed_windows = NULL;
  }

  g_clear_pointer(&self->restored_windows, wig_session_window_list_free);

  G_OBJECT_CLASS(wig_session_parent_class)->dispose(object);
}

static void wig_session_finalize(GObject *object)
{
  WigSession *self = WIG_SESSION(object);

  g_free(self->path);

  G_OBJECT_CLASS(wig_session_parent_class)->finalize(object);
}

static void wig_session_class_init(WigSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_session_dispose;
  object_class->finalize = wig_session_finalize;
}

static void wig_session_init(WigSession *self)
{
  self->closed_windows = g_queue_new();
}

WigSession *wig_session_new(const char *state_dir)
{
  g_return_val_if_fail(state_dir != NULL, NULL);

  WigSession *self = g_object_new(WIG_TYPE_SESSION, NULL);
  self->path = g_build_filename(state_dir, "session.ini", NULL);
  return self;
}

void wig_session_set_collect_func(WigSession *self, WigSessionCollectFunc func, gpointer user_data)
{
  g_return_if_fail(WIG_IS_SESSION(self));

  self->collect_func = func;
  self->collect_data = user_data;
}

void wig_session_load(WigSession *self)
{
  g_return_if_fail(WIG_IS_SESSION(self));

  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GError) error = NULL;
  if (!g_key_file_load_from_file(key_file, self->path, G_KEY_FILE_NONE, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      g_warning("session: cannot read '%s': %s", self->path, error->message);
    return;
  }

  int version = g_key_file_get_integer(key_file, WIG_SESSION_GROUP, "Version", &error);
  if (error) {
    g_warning("session: ignoring '%s' with no readable format version: %s", self->path, error->message);
    return;
  }
  if (version != WIG_SESSION_FORMAT_VERSION) {
    g_warning("session: ignoring '%s' written in format version %d", self->path, version);
    return;
  }

  self->restore_on_next_start = wig_key_file_get_boolean(key_file, WIG_SESSION_GROUP, "RestoreOnNextStart", FALSE);

  g_clear_pointer(&self->restored_windows, wig_session_window_list_free);
  self->restored_windows = wig_session_window_list_from_key_file(key_file, "Windows", "Window", "Tab", TRUE);

  g_queue_clear_full(self->closed_windows, (GDestroyNotify)wig_session_window_free);
  GSList *closed = wig_session_window_list_from_key_file(key_file, "ClosedWindows", "ClosedWindow", "ClosedTab", FALSE);
  for (GSList *l = closed; l; l = l->next)
    g_queue_push_tail(self->closed_windows, l->data);
  g_slist_free(closed);

  g_debug("session: loaded %d window(s) and %d closed window(s) from '%s'", g_slist_length(self->restored_windows),
          g_queue_get_length(self->closed_windows), self->path);
}

GSList *wig_session_take_restored_windows(WigSession *self)
{
  g_return_val_if_fail(WIG_IS_SESSION(self), NULL);

  return g_steal_pointer(&self->restored_windows);
}

/* Nothing may touch the session once the application is on its way out: the
 * windows are being torn down, and what they look like while that happens is
 * not what should come back on the next launch. */
void wig_session_set_quitting(WigSession *self)
{
  g_return_if_fail(WIG_IS_SESSION(self));

  g_debug("session: quitting, the saved state is now final");
  self->quitting = TRUE;
  g_clear_handle_id(&self->save_timeout_id, g_source_remove);
}

void wig_session_set_restore_on_next_start(WigSession *self)
{
  g_debug("session: the next start restores this session");
  self->restore_on_next_start = TRUE;
}

/* Reading it consumes it: the launch after the restart is an ordinary one. */
gboolean wig_session_take_restore_on_next_start(WigSession *self)
{
  gboolean restore = self->restore_on_next_start;

  self->restore_on_next_start = FALSE;
  return restore;
}

/* Windows and tabs appearing one at a time as a session is restored look exactly
 * like a user opening them, so the session has to be told to ignore itself
 * being put back together. */
void wig_session_set_restoring(WigSession *self, gboolean restoring)
{
  g_return_if_fail(WIG_IS_SESSION(self));

  if (self->restoring == restoring)
    return;

  g_debug("session: %s", restoring ? "restoring" : "restored");
  self->restoring = restoring;

  if (!restoring)
    wig_session_queue_save(self);
}

void wig_session_save(WigSession *self)
{
  g_return_if_fail(WIG_IS_SESSION(self));

  if (self->quitting || self->restoring)
    return;

  g_clear_handle_id(&self->save_timeout_id, g_source_remove);

  if (!self->collect_func)
    return;

  GSList *open_windows = self->collect_func(self->collect_data);
  if (!open_windows) {
    g_debug("session: no window holds anything worth restoring, skipping the save");
    return;
  }

  wig_session_write(self, open_windows);
  wig_session_window_list_free(open_windows);
}

static gboolean wig_session_on_save_timeout(gpointer user_data)
{
  WigSession *self = WIG_SESSION(user_data);

  self->save_timeout_id = 0;
  wig_session_save(self);

  return G_SOURCE_REMOVE;
}

void wig_session_queue_save(WigSession *self)
{
  g_return_if_fail(WIG_IS_SESSION(self));

  if (self->quitting || self->restoring || self->save_timeout_id)
    return;

  self->save_timeout_id = g_timeout_add_seconds(WIG_SESSION_SAVE_DELAY_SECONDS, wig_session_on_save_timeout, self);
}

void wig_session_push_closed_window(WigSession *self, WigSessionWindow *window)
{
  g_return_if_fail(WIG_IS_SESSION(self));
  g_return_if_fail(window != NULL);

  if (self->quitting || !window->tabs) {
    wig_session_window_free(window);
    return;
  }

  g_debug("session: pushing closed window %u with %d tab(s)", window->window_id, g_slist_length(window->tabs));
  g_queue_push_tail(self->closed_windows, window);

  if (g_queue_get_length(self->closed_windows) > WIG_SESSION_MAX_CLOSED_WINDOWS)
    wig_session_window_free(g_queue_pop_head(self->closed_windows));

  wig_session_queue_save(self);
}

WigSessionWindow *wig_session_pop_closed_window(WigSession *self)
{
  g_return_val_if_fail(WIG_IS_SESSION(self), NULL);

  if (self->quitting)
    return NULL;

  WigSessionWindow *window = g_queue_pop_tail(self->closed_windows);
  if (window) {
    g_debug("session: popping closed window %u with %d tab(s)", window->window_id, g_slist_length(window->tabs));
    wig_session_queue_save(self);
  }

  return window;
}
