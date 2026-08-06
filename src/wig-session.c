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

#include <glib/gstdio.h>

#define WIG_SESSION_MAX_CLOSED_WINDOWS 20
#define WIG_SESSION_SAVE_DELAY_SECONDS 5
#define WIG_SESSION_FORMAT_VERSION 1

/* (version, open windows, closed windows), a window being (id, (tab state, was focused)*). */
#define WIG_SESSION_FORMAT "(ua(ua(ayb))a(ua(ayb)))"
#define WIG_SESSION_WINDOW_FORMAT "(u@a(ayb))"

struct _WigSession {
  GObject parent;

  char *path;
  GQueue *closed_windows; /* owned WigSessionWindow*, oldest first */
  GSList *restored_windows; /* owned WigSessionWindow*, from the last load */
  GVariant *open_variant; /* a(ua(ayb)), the open windows as last written */

  WigSessionCollectFunc collect_func;
  gpointer collect_data;
  guint save_timeout_id;
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
void wig_session_window_add_tab(WigSessionWindow *self, WebKitWebViewSessionState *state, gboolean was_focused)
{
  g_assert(self != NULL);

  if (!state)
    return;

  WigSessionTab *tab = g_new0(WigSessionTab, 1);
  tab->state = state;
  tab->was_focused = was_focused;
  self->tabs = g_slist_append(self->tabs, tab);
}

void wig_session_window_free(WigSessionWindow *self)
{
  if (!self)
    return;

  g_slist_free_full(self->tabs, (GDestroyNotify)wig_session_tab_free);
  g_free(self);
}

static void wig_session_window_list_free(GSList *windows)
{
  g_slist_free_full(windows, (GDestroyNotify)wig_session_window_free);
}

static GVariant *wig_session_window_to_variant(const WigSessionWindow *window)
{
  GVariantBuilder tabs;
  g_variant_builder_init(&tabs, G_VARIANT_TYPE("a(ayb)"));

  for (const GSList *l = window->tabs; l; l = l->next) {
    const WigSessionTab *tab = l->data;
    g_autoptr(GBytes) bytes = webkit_web_view_session_state_serialize(tab->state);
    if (!bytes)
      continue;

    gsize size;
    const guint8 *data = g_bytes_get_data(bytes, &size);
    g_variant_builder_add(&tabs, "(@ayb)", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, size, 1),
                          tab->was_focused);
  }

  return g_variant_new(WIG_SESSION_WINDOW_FORMAT, window->window_id, g_variant_builder_end(&tabs));
}

static WigSessionWindow *wig_session_window_from_variant(GVariant *variant)
{
  guint window_id;
  g_autoptr(GVariant) tabs = NULL;
  g_variant_get(variant, WIG_SESSION_WINDOW_FORMAT, &window_id, &tabs);

  WigSessionWindow *window = wig_session_window_new(window_id);

  GVariantIter iter;
  GVariant *blob;
  gboolean was_focused;
  g_variant_iter_init(&iter, tabs);
  while (g_variant_iter_loop(&iter, "(@ayb)", &blob, &was_focused)) {
    gsize size;
    const guint8 *data = g_variant_get_fixed_array(blob, &size, 1);
    if (!size)
      continue;

    g_autoptr(GBytes) bytes = g_bytes_new(data, size);
    WebKitWebViewSessionState *state = webkit_web_view_session_state_new(bytes);
    if (!state) {
      g_warning("session: discarding a tab with unreadable state");
      continue;
    }

    wig_session_window_add_tab(window, state, was_focused);
  }

  return window;
}

static GSList *wig_session_window_list_from_variant(GVariant *variant)
{
  GSList *windows = NULL;
  GVariantIter iter;
  GVariant *child;

  g_variant_iter_init(&iter, variant);
  while (g_variant_iter_loop(&iter, "@(ua(ayb))", &child)) {
    WigSessionWindow *window = wig_session_window_from_variant(child);
    if (window->tabs)
      windows = g_slist_prepend(windows, window);
    else
      wig_session_window_free(window);
  }

  return g_slist_reverse(windows);
}

static GVariant *wig_session_window_list_to_variant(GSList *windows)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ua(ayb))"));

  for (GSList *l = windows; l; l = l->next)
    g_variant_builder_add_value(&builder, wig_session_window_to_variant(l->data));

  return g_variant_builder_end(&builder);
}

static void wig_session_write(WigSession *self, GVariant *open_windows)
{
  g_autofree char *dir = g_path_get_dirname(self->path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_warning("session: cannot create '%s': %s", dir, g_strerror(errno));
    return;
  }

  GVariantBuilder closed_builder;
  g_variant_builder_init(&closed_builder, G_VARIANT_TYPE("a(ua(ayb))"));
  for (GList *l = self->closed_windows->head; l; l = l->next)
    g_variant_builder_add_value(&closed_builder, wig_session_window_to_variant(l->data));

  g_autoptr(GVariant) variant = g_variant_ref_sink(g_variant_new(
      "(u@a(ua(ayb))@a(ua(ayb)))", WIG_SESSION_FORMAT_VERSION, open_windows, g_variant_builder_end(&closed_builder)));

  g_autoptr(GError) error = NULL;
  if (!g_file_set_contents_full(self->path, g_variant_get_data(variant), (gssize)g_variant_get_size(variant),
                                G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
    g_warning("session: cannot write '%s': %s", self->path, error->message);
    return;
  }

  g_debug("session: wrote %" G_GSIZE_FORMAT " window(s) and %d closed window(s) to '%s'",
          g_variant_n_children(open_windows), g_queue_get_length(self->closed_windows), self->path);
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
  g_clear_pointer(&self->open_variant, g_variant_unref);

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
  self->path = g_build_filename(state_dir, "session.gvariant", NULL);
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

  g_autofree char *contents = NULL;
  gsize length;
  g_autoptr(GError) error = NULL;
  if (!g_file_get_contents(self->path, &contents, &length, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      g_warning("session: cannot read '%s': %s", self->path, error->message);
    return;
  }

  g_autoptr(GBytes) bytes = g_bytes_new_take(g_steal_pointer(&contents), length);
  g_autoptr(GVariant) variant = g_variant_new_from_bytes(G_VARIANT_TYPE(WIG_SESSION_FORMAT), bytes, FALSE);
  if (!g_variant_is_normal_form(variant)) {
    g_warning("session: '%s' is corrupt, ignoring it", self->path);
    return;
  }

  guint version;
  g_autoptr(GVariant) open_windows = NULL;
  g_autoptr(GVariant) closed_windows = NULL;
  g_variant_get(variant, "(u@a(ua(ayb))@a(ua(ayb)))", &version, &open_windows, &closed_windows);

  if (version != WIG_SESSION_FORMAT_VERSION) {
    g_message("session: ignoring '%s' written in format version %u", self->path, version);
    return;
  }

  g_clear_pointer(&self->restored_windows, wig_session_window_list_free);
  self->restored_windows = wig_session_window_list_from_variant(open_windows);

  g_clear_pointer(&self->open_variant, g_variant_unref);
  self->open_variant = g_variant_ref(open_windows);

  GSList *closed = wig_session_window_list_from_variant(closed_windows);
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

void wig_session_save(WigSession *self)
{
  g_return_if_fail(WIG_IS_SESSION(self));

  g_clear_handle_id(&self->save_timeout_id, g_source_remove);

  if (!self->collect_func)
    return;

  GSList *open_windows = self->collect_func(self->collect_data);

  /* Windows only ever all disappear as the application is going away, so the
   * windows to restore are the ones from just before that, not none at all.
   * The closed tab history is still worth writing out. */
  if (!open_windows) {
    if (!self->open_variant) {
      g_debug("session: nothing open and nothing saved before, skipping the save");
      return;
    }

    g_debug("session: nothing open, keeping the windows already saved");
    wig_session_write(self, self->open_variant);
    return;
  }

  g_clear_pointer(&self->open_variant, g_variant_unref);
  self->open_variant = g_variant_ref_sink(wig_session_window_list_to_variant(open_windows));
  wig_session_window_list_free(open_windows);

  wig_session_write(self, self->open_variant);
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

  if (self->save_timeout_id)
    return;

  self->save_timeout_id = g_timeout_add_seconds(WIG_SESSION_SAVE_DELAY_SECONDS, wig_session_on_save_timeout, self);
}

void wig_session_push_closed_window(WigSession *self, WigSessionWindow *window)
{
  g_return_if_fail(WIG_IS_SESSION(self));
  g_return_if_fail(window != NULL);

  if (!window->tabs) {
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

  WigSessionWindow *window = g_queue_pop_tail(self->closed_windows);
  if (window) {
    g_debug("session: popping closed window %u with %d tab(s)", window->window_id, g_slist_length(window->tabs));
    wig_session_queue_save(self);
  }

  return window;
}
