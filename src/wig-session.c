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

#define WIG_SESSION_MAX_CLOSED_GROUPS 20

struct _WigSession {
  GObject parent;

  GQueue *closed_groups; /* owned WigClosedGroup*, oldest first */
};

G_DEFINE_FINAL_TYPE(WigSession, wig_session, G_TYPE_OBJECT)

static void wig_closed_tab_free(WigClosedTab *tab)
{
  webkit_web_view_session_state_unref(tab->state);
  g_free(tab);
}

WigClosedGroup *wig_closed_group_new(guint window_id)
{
  WigClosedGroup *group = g_new0(WigClosedGroup, 1);
  group->window_id = window_id;
  return group;
}

/* Takes ownership of @state. A view that never loaded anything has no state and
 * is not worth reopening, so it is dropped rather than restored as a blank tab. */
void wig_closed_group_add_tab(WigClosedGroup *group, WebKitWebViewSessionState *state, gboolean was_focused)
{
  g_assert(group != NULL);

  if (!state)
    return;

  WigClosedTab *tab = g_new0(WigClosedTab, 1);
  tab->state = state;
  tab->was_focused = was_focused;
  group->tabs = g_slist_append(group->tabs, tab);
}

void wig_closed_group_free(WigClosedGroup *group)
{
  if (!group)
    return;

  g_slist_free_full(group->tabs, (GDestroyNotify)wig_closed_tab_free);
  g_free(group);
}

static void wig_session_dispose(GObject *object)
{
  WigSession *self = WIG_SESSION(object);

  if (self->closed_groups) {
    g_queue_free_full(self->closed_groups, (GDestroyNotify)wig_closed_group_free);
    self->closed_groups = NULL;
  }

  G_OBJECT_CLASS(wig_session_parent_class)->dispose(object);
}

static void wig_session_class_init(WigSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_session_dispose;
}

static void wig_session_init(WigSession *self)
{
  self->closed_groups = g_queue_new();
}

WigSession *wig_session_new(void)
{
  return g_object_new(WIG_TYPE_SESSION, NULL);
}

void wig_session_push_closed_group(WigSession *self, WigClosedGroup *group)
{
  g_return_if_fail(WIG_IS_SESSION(self));
  g_return_if_fail(group != NULL);

  if (!group->tabs) {
    wig_closed_group_free(group);
    return;
  }

  g_debug("session: pushing closed group of size %d for window %d", g_slist_length(group->tabs), group->window_id);
  g_queue_push_tail(self->closed_groups, group);

  if (g_queue_get_length(self->closed_groups) > WIG_SESSION_MAX_CLOSED_GROUPS)
    wig_closed_group_free(g_queue_pop_head(self->closed_groups));
}

WigClosedGroup *wig_session_pop_closed_group(WigSession *self)
{
  g_return_val_if_fail(WIG_IS_SESSION(self), NULL);

  WigClosedGroup *group = g_queue_pop_tail(self->closed_groups);
  if (group)
    g_debug("session: popping closed group of size %d for window %d", g_slist_length(group->tabs), group->window_id);
  return group;
}
