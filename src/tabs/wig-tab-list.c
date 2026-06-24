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

#include "wig-tab-list.h"

struct _WigTabList {
  GObject parent;

  GPtrArray *tabs; /* type WigTab*, owned */
  WigTab *active; /* owned, nullable */
};

G_DEFINE_FINAL_TYPE(WigTabList, wig_tab_list, G_TYPE_OBJECT)

enum { PROP_0, PROP_ACTIVE_TAB, N_PROPS };
static GParamSpec *props[N_PROPS];

enum { SIGNAL_TAB_ADDED, SIGNAL_TAB_REMOVED, SIGNAL_CLOSE_TAB, SIGNAL_CREATE_TAB, N_SIGNALS };
static guint signals[N_SIGNALS];

static void wig_tab_list_dispose(GObject *object)
{
  WigTabList *self = WIG_TAB_LIST(object);
  g_clear_object(&self->active);
  g_clear_pointer(&self->tabs, g_ptr_array_unref);
  G_OBJECT_CLASS(wig_tab_list_parent_class)->dispose(object);
}

static void wig_tab_list_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigTabList *self = WIG_TAB_LIST(object);
  switch (prop_id) {
  case PROP_ACTIVE_TAB:
    g_value_set_object(value, self->active);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_tab_list_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigTabList *self = WIG_TAB_LIST(object);
  switch (prop_id) {
  case PROP_ACTIVE_TAB:
    wig_tab_list_set_active(self, g_value_get_object(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_tab_list_init(WigTabList *self)
{
  self->tabs = g_ptr_array_new_with_free_func(g_object_unref);
}

static void wig_tab_list_class_init(WigTabListClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_list_dispose;
  gobject_class->get_property = wig_tab_list_get_property;
  gobject_class->set_property = wig_tab_list_set_property;

  props[PROP_ACTIVE_TAB] = g_param_spec_object("active-tab", NULL, NULL, WIG_TYPE_TAB,
                                               G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(gobject_class, N_PROPS, props);

  /* tab-added / tab-removed: the tab pointer is valid for the duration of the
   * signal — the list holds a strong ref until after all handlers return. */
  signals[SIGNAL_TAB_ADDED] = g_signal_new("tab-added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 2, WIG_TYPE_TAB, G_TYPE_UINT);
  signals[SIGNAL_TAB_REMOVED] = g_signal_new("tab-removed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                             NULL, G_TYPE_NONE, 2, WIG_TYPE_TAB, G_TYPE_UINT);

  /* Returns gboolean: TRUE means the caller handled close (e.g. window destroy);
   * FALSE means the list should remove the tab itself. */
  signals[SIGNAL_CLOSE_TAB] = g_signal_new("close-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                           g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 1,
                                           WIG_TYPE_TAB);

  signals[SIGNAL_CREATE_TAB] = g_signal_new("create-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                            NULL, WIG_TYPE_TAB, 0);
}

WigTabList *wig_tab_list_new(void)
{
  return WIG_TAB_LIST(g_object_new(WIG_TYPE_TAB_LIST, NULL));
}

WigTab *wig_tab_list_append(WigTabList *self, WebKitWebView *web_view)
{
  g_autoptr(WigTab) tab = wig_tab_new(web_view);
  gboolean first = self->tabs->len == 0;
  g_ptr_array_add(self->tabs, g_object_ref(tab));
  guint pos = self->tabs->len - 1;
  g_signal_emit(self, signals[SIGNAL_TAB_ADDED], 0, tab, pos);
  if (first)
    wig_tab_list_set_active(self, tab);
  return tab;
}

void wig_tab_list_close(WigTabList *self, WigTab *tab)
{
  guint pos = wig_tab_list_index_of(self, tab);
  if (pos == GTK_INVALID_LIST_POSITION)
    return;

  gboolean handled = FALSE;
  g_signal_emit(self, signals[SIGNAL_CLOSE_TAB], 0, tab, &handled);
  if (handled)
    return;

  /* If the active tab is being closed, move active to the nearest neighbour. */
  if (self->active == tab) {
    WigTab *new_active = NULL;
    if (self->tabs->len > 1)
      new_active = g_ptr_array_index(self->tabs, pos > 0 ? pos - 1 : pos + 1);
    g_set_object(&self->active, new_active);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ACTIVE_TAB]);
  }

  g_autoptr(WigTab) alive_tab = g_object_ref(tab);
  g_ptr_array_remove_index(self->tabs, pos);
  g_signal_emit(self, signals[SIGNAL_TAB_REMOVED], 0, alive_tab, pos);
}

void wig_tab_list_move(WigTabList *self, WigTab *tab, guint new_index)
{
  guint old_index = wig_tab_list_index_of(self, tab);
  if (old_index == GTK_INVALID_LIST_POSITION || old_index == new_index)
    return;

  g_autoptr(WigTab) alive_tab = g_object_ref(tab);
  g_ptr_array_remove_index(self->tabs, old_index);
  if (old_index < new_index)
    new_index--;
  g_ptr_array_insert(self->tabs, (gint)new_index, g_steal_pointer(&alive_tab));
}

guint wig_tab_list_get_n_tabs(WigTabList *self)
{
  return self->tabs->len;
}

WigTab *wig_tab_list_get_nth(WigTabList *self, guint i)
{
  g_return_val_if_fail(i < self->tabs->len, NULL);
  return g_ptr_array_index(self->tabs, i);
}

guint wig_tab_list_index_of(WigTabList *self, WigTab *tab)
{
  for (guint i = 0; i < self->tabs->len; i++) {
    if (g_ptr_array_index(self->tabs, i) == tab)
      return i;
  }
  return GTK_INVALID_LIST_POSITION;
}

WigTab *wig_tab_list_get_active(WigTabList *self)
{
  return self->active;
}

void wig_tab_list_set_active(WigTabList *self, WigTab *tab)
{
  if (self->active == tab)
    return;
  g_set_object(&self->active, tab);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ACTIVE_TAB]);
}
