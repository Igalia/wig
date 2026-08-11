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

#pragma once

#include <wpe/webkit.h>

G_BEGIN_DECLS

#define WIG_TYPE_SESSION (wig_session_get_type())
G_DECLARE_FINAL_TYPE(WigSession, wig_session, WIG, SESSION, GObject)

typedef struct {
  WebKitWebViewSessionState *state;
  gboolean was_focused;
  gboolean pinned;
} WigSessionTab;

typedef struct {
  guint window_id;
  gboolean focused;
  gboolean maximized;
  gboolean fullscreen;
  gboolean minimized;
  int width;
  int height;
  int sidebar_width;
  char *monitor; /* owned connector name, NULL when unknown */
  GSList *tabs; /* owned GSList of WigSessionTab* */
} WigSessionWindow;

WigSessionWindow *wig_session_window_new(guint window_id);
void wig_session_window_add_tab(WigSessionWindow *self, WebKitWebViewSessionState *state, gboolean was_focused,
                                gboolean pinned);
void wig_session_window_free(WigSessionWindow *self);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WigSessionWindow, wig_session_window_free)

/* Returns a newly allocated GSList of WigSessionWindow* describing every open
 * window, in the order they should be restored. */
typedef GSList *(*WigSessionCollectFunc)(gpointer user_data);

WigSession *wig_session_new(const char *state_dir);
void wig_session_set_collect_func(WigSession *self, WigSessionCollectFunc func, gpointer user_data);

void wig_session_load(WigSession *self);
GSList *wig_session_take_restored_windows(WigSession *self);
void wig_session_set_restoring(WigSession *self, gboolean restoring);

void wig_session_queue_save(WigSession *self);
void wig_session_save(WigSession *self);
void wig_session_set_quitting(WigSession *self);

void wig_session_set_restore_on_next_start(WigSession *self);
gboolean wig_session_take_restore_on_next_start(WigSession *self);

void wig_session_push_closed_window(WigSession *self, WigSessionWindow *window);
WigSessionWindow *wig_session_pop_closed_window(WigSession *self);

G_END_DECLS
