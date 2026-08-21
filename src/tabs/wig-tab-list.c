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

static void wig_tab_list_reorder(WigTabList *, WigTab *, guint);
static guint wig_tab_list_clamp_to_block(WigTabList *, WigTab *, guint);
static WigTab *wig_tab_list_successor(WigTabList *, guint);

static void on_action_reload(GSimpleAction *, GVariant *, gpointer);
static void on_action_mute(GSimpleAction *, GVariant *, gpointer);
static void on_action_duplicate(GSimpleAction *, GVariant *, gpointer);
static void on_action_copy_link(GSimpleAction *, GVariant *, gpointer);
static void on_action_pin(GSimpleAction *, GVariant *, gpointer);
static void on_action_move_to_new_window(GSimpleAction *, GVariant *, gpointer);
static void on_action_unload(GSimpleAction *, GVariant *, gpointer);
static void on_action_close(GSimpleAction *, GVariant *, gpointer);
static void on_action_close_to_left(GSimpleAction *, GVariant *, gpointer);
static void on_action_close_to_right(GSimpleAction *, GVariant *, gpointer);
static void on_action_close_others(GSimpleAction *, GVariant *, gpointer);

struct _WigTabList {
  GObject parent;

  GPtrArray *tabs; /* type WigTab*, owned */
  WigTab *active; /* owned, nullable */
  GSimpleActionGroup *actions;
};

G_DEFINE_FINAL_TYPE(WigTabList, wig_tab_list, G_TYPE_OBJECT)

typedef enum {
  PROP_ACTIVE_TAB = 1,
} WigTabListProps;

static GParamSpec *props[PROP_ACTIVE_TAB + 1];

enum {
  SIGNAL_TAB_ADDED,
  SIGNAL_TAB_REMOVED,
  SIGNAL_TAB_MOVED,
  SIGNAL_CLOSE_TAB,
  SIGNAL_CREATE_TAB,
  SIGNAL_RELOAD_TAB,
  SIGNAL_MUTE_TAB,
  SIGNAL_DUPLICATE_TAB,
  SIGNAL_COPY_LINK_TAB,
  SIGNAL_DETACH_TAB,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void wig_tab_list_dispose(GObject *object)
{
  WigTabList *self = WIG_TAB_LIST(object);
  g_clear_object(&self->active);
  g_clear_object(&self->actions);
  g_clear_pointer(&self->tabs, g_ptr_array_unref);
  G_OBJECT_CLASS(wig_tab_list_parent_class)->dispose(object);
}

static void wig_tab_list_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigTabList *self = WIG_TAB_LIST(object);
  switch ((WigTabListProps)prop_id) {
  case PROP_ACTIVE_TAB:
    g_value_set_object(value, self->active);
    break;
  }
}

static void wig_tab_list_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigTabList *self = WIG_TAB_LIST(object);
  switch ((WigTabListProps)prop_id) {
  case PROP_ACTIVE_TAB:
    wig_tab_list_set_active(self, g_value_get_object(value));
    break;
  }
}

static void wig_tab_list_init(WigTabList *self)
{
  self->tabs = g_ptr_array_new_with_free_func(g_object_unref);

  static const GActionEntry entries[] = {
    { "reload", on_action_reload, "u", NULL, NULL },
    { "mute", on_action_mute, "u", NULL, NULL },
    { "duplicate", on_action_duplicate, "u", NULL, NULL },
    { "copy-link", on_action_copy_link, "u", NULL, NULL },
    { "pin", on_action_pin, "u", NULL, NULL },
    { "move-to-new-window", on_action_move_to_new_window, "u", NULL, NULL },
    { "unload", on_action_unload, "u", NULL, NULL },
    { "close", on_action_close, "u", NULL, NULL },
    { "close-to-left", on_action_close_to_left, "u", NULL, NULL },
    { "close-to-right", on_action_close_to_right, "u", NULL, NULL },
    { "close-others", on_action_close_others, "u", NULL, NULL },
  };
  self->actions = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(self->actions), entries, G_N_ELEMENTS(entries), self);
}

static void wig_tab_list_class_init(WigTabListClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_list_dispose;
  gobject_class->get_property = wig_tab_list_get_property;
  gobject_class->set_property = wig_tab_list_set_property;

  props[PROP_ACTIVE_TAB] = g_param_spec_object("active-tab", NULL, NULL, WIG_TYPE_TAB,
                                               G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(gobject_class, G_N_ELEMENTS(props), props);

  /* tab-added / tab-removed: the tab pointer is valid for the duration of the
   * signal — the list holds a strong ref until after all handlers return. */
  signals[SIGNAL_TAB_ADDED] = g_signal_new("tab-added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 2, WIG_TYPE_TAB, G_TYPE_UINT);
  signals[SIGNAL_TAB_REMOVED] = g_signal_new("tab-removed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                             NULL, G_TYPE_NONE, 2, WIG_TYPE_TAB, G_TYPE_UINT);
  /* tab-moved(tab, old_index, new_index) */
  signals[SIGNAL_TAB_MOVED] = g_signal_new("tab-moved", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 3, WIG_TYPE_TAB, G_TYPE_UINT, G_TYPE_UINT);

  /* Returns gboolean: TRUE means the caller handled close (e.g. window destroy);
   * FALSE means the list should remove the tab itself. */
  signals[SIGNAL_CLOSE_TAB] = g_signal_new("close-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                           g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 1,
                                           WIG_TYPE_TAB);

  signals[SIGNAL_CREATE_TAB] = g_signal_new("create-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                            NULL, WIG_TYPE_TAB, 0);

  signals[SIGNAL_RELOAD_TAB] = g_signal_new("reload-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                            NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_MUTE_TAB] = g_signal_new("mute-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_DUPLICATE_TAB] = g_signal_new("duplicate-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                               NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_COPY_LINK_TAB] = g_signal_new("copy-link-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                               NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_DETACH_TAB] = g_signal_new("detach-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                            NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

WigTabList *wig_tab_list_new(void)
{
  return WIG_TAB_LIST(g_object_new(WIG_TYPE_TAB_LIST, NULL));
}

WigTab *wig_tab_list_insert(WigTabList *self, WebKitWebView *web_view, guint index)
{
  g_autoptr(WigTab) tab = wig_tab_new(web_view);
  gboolean first = self->tabs->len == 0;

  index = CLAMP(index, wig_tab_list_get_n_pinned(self), self->tabs->len);
  g_ptr_array_insert(self->tabs, (gint)index, g_object_ref(tab));
  g_signal_emit(self, signals[SIGNAL_TAB_ADDED], 0, tab, index);
  if (first)
    wig_tab_list_set_active(self, tab);
  return tab;
}

WigTab *wig_tab_list_append(WigTabList *self, WebKitWebView *web_view)
{
  return wig_tab_list_insert(self, web_view, self->tabs->len);
}

static WigTab *wig_tab_list_successor(WigTabList *self, guint pos)
{
  WigTab *leaving = g_ptr_array_index(self->tabs, pos);
  WigTab *successor = NULL;
  gint64 last_active = 0;

  for (guint i = 0; i < self->tabs->len; i++) {
    WigTab *tab = g_ptr_array_index(self->tabs, i);

    if (tab == leaving || wig_tab_get_last_active(tab) <= last_active)
      continue;

    successor = tab;
    last_active = wig_tab_get_last_active(tab);
  }

  if (successor)
    return successor;

  if (self->tabs->len < 2)
    return NULL;

  return g_ptr_array_index(self->tabs, pos > 0 ? pos - 1 : pos + 1);
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

  if (self->active == tab)
    wig_tab_list_set_active(self, wig_tab_list_successor(self, pos));

  g_autoptr(WigTab) alive_tab = g_object_ref(tab);
  g_ptr_array_remove_index(self->tabs, pos);
  g_signal_emit(self, signals[SIGNAL_TAB_REMOVED], 0, alive_tab, pos);
}

/* Remove a tab from the list without going through the close-tab signal.
 * Callers are responsible for handling the web view lifecycle (e.g. re-attaching
 * it to a new list with wig_tab_list_attach). */
WigTab *wig_tab_list_detach(WigTabList *self, WigTab *tab)
{
  guint pos = wig_tab_list_index_of(self, tab);
  if (pos == GTK_INVALID_LIST_POSITION)
    return NULL;

  if (self->active == tab)
    wig_tab_list_set_active(self, wig_tab_list_successor(self, pos));

  g_autoptr(WigTab) alive_tab = g_object_ref(tab);
  g_ptr_array_remove_index(self->tabs, pos);
  g_signal_emit(self, signals[SIGNAL_TAB_REMOVED], 0, alive_tab, pos);
  return g_steal_pointer(&alive_tab);
}

/* Add an existing WigTab to this list (e.g. after detaching from another list).
 * The tab is appended. */
void wig_tab_list_attach(WigTabList *self, WigTab *tab)
{
  g_ptr_array_add(self->tabs, g_object_ref(tab));
  guint pos = self->tabs->len - 1;
  g_signal_emit(self, signals[SIGNAL_TAB_ADDED], 0, tab, pos);

  /* Which tab to look at is the caller's to say — attaching the first one is not
   * a reason to build a view for it, when a restored window is about to name a
   * different tab as the one to show. */

  /* A tab keeps its pin when it moves to another window, so it has to land in
   * that window's pinned block rather than at the end. */
  wig_tab_list_reorder(self, tab, wig_tab_list_clamp_to_block(self, tab, pos));
}

/* Move @tab to @final_index, the index it ends up at once it has been taken out
 * of the array. */
static void wig_tab_list_reorder(WigTabList *self, WigTab *tab, guint final_index)
{
  guint old_index = wig_tab_list_index_of(self, tab);
  if (old_index == GTK_INVALID_LIST_POSITION || old_index == final_index)
    return;

  g_autoptr(WigTab) alive_tab = g_object_ref(tab);
  g_ptr_array_remove_index(self->tabs, old_index);
  g_ptr_array_insert(self->tabs, (gint)final_index, g_steal_pointer(&alive_tab));
  g_signal_emit(self, signals[SIGNAL_TAB_MOVED], 0, tab, old_index, final_index);
}

/* Pinned tabs occupy the front of the list, so a tab can only be reordered
 * within its own block however far a drop or a move asks it to travel. */
static guint wig_tab_list_clamp_to_block(WigTabList *self, WigTab *tab, guint index)
{
  guint n_pinned = wig_tab_list_get_n_pinned(self);
  if (wig_tab_get_pinned(tab))
    return MIN(index, n_pinned - 1);
  return CLAMP(index, n_pinned, self->tabs->len - 1);
}

void wig_tab_list_move(WigTabList *self, WigTab *tab, guint new_index)
{
  guint old_index = wig_tab_list_index_of(self, tab);
  if (old_index == GTK_INVALID_LIST_POSITION)
    return;

  /* @new_index counts the tab itself, so dropping past its own position lands
   * one slot earlier once it is taken out. */
  guint final_index = new_index > old_index ? new_index - 1 : new_index;
  final_index = MIN(final_index, self->tabs->len - 1);
  wig_tab_list_reorder(self, tab, wig_tab_list_clamp_to_block(self, tab, final_index));
}

/* Move @tabs to @new_index as one run, keeping the order they have here.  Each
 * one lands directly after the one before it, so the drop index only has to be
 * right for the first. */
void wig_tab_list_move_many(WigTabList *self, GPtrArray *tabs, guint new_index)
{
  for (guint i = 0; i < tabs->len; i++) {
    WigTab *tab = g_ptr_array_index(tabs, i);
    if (wig_tab_list_index_of(self, tab) == GTK_INVALID_LIST_POSITION)
      continue;

    wig_tab_list_move(self, tab, new_index);
    new_index = wig_tab_list_index_of(self, tab) + 1;
  }
}

void wig_tab_list_set_pinned(WigTabList *self, WigTab *tab, gboolean pinned)
{
  if (wig_tab_list_index_of(self, tab) == GTK_INVALID_LIST_POSITION)
    return;
  if (wig_tab_get_pinned(tab) == pinned)
    return;

  wig_tab_set_pinned(tab, pinned);

  /* Join the end of the pinned block, or the start of the unpinned one. */
  guint n_pinned = wig_tab_list_get_n_pinned(self);
  wig_tab_list_reorder(self, tab, pinned ? n_pinned - 1 : n_pinned);
}

guint wig_tab_list_get_n_tabs(WigTabList *self)
{
  return self->tabs->len;
}

guint wig_tab_list_get_n_pinned(WigTabList *self)
{
  guint n = 0;
  for (guint i = 0; i < self->tabs->len; i++) {
    if (wig_tab_get_pinned(g_ptr_array_index(self->tabs, i)))
      n++;
  }
  return n;
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

WigTab *wig_tab_list_get_by_id(WigTabList *self, guint id)
{
  for (guint i = 0; i < self->tabs->len; i++) {
    WigTab *tab = g_ptr_array_index(self->tabs, i);
    if (wig_tab_get_id(tab) == id)
      return tab;
  }
  return NULL;
}

void wig_tab_list_close_many(WigTabList *self, GPtrArray *tabs)
{
  g_autoptr(GPtrArray) alive_tabs = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < tabs->len; i++)
    g_ptr_array_add(alive_tabs, g_object_ref(g_ptr_array_index(tabs, i)));
  for (guint i = 0; i < alive_tabs->len; i++)
    wig_tab_list_close(self, g_ptr_array_index(alive_tabs, i));
}

WigTab *wig_tab_list_get_active(WigTabList *self)
{
  return self->active;
}

void wig_tab_list_set_active(WigTabList *self, WigTab *tab)
{
  if (self->active == tab)
    return;

  if (self->active)
    wig_tab_set_active(self->active, FALSE);

  g_set_object(&self->active, tab);

  if (tab)
    wig_tab_set_active(tab, TRUE);

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ACTIVE_TAB]);
}

/* Returns: (transfer full): the selected tabs in list order.  They are reffed so
 * that callers can move or detach them while walking the result. */
GPtrArray *wig_tab_list_get_selected(WigTabList *self)
{
  GPtrArray *selected = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < self->tabs->len; i++) {
    WigTab *t = g_ptr_array_index(self->tabs, i);
    if (wig_tab_get_selected(t))
      g_ptr_array_add(selected, g_object_ref(t));
  }
  return selected;
}

static void on_action_reload(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;
  if (wig_tab_get_selected(tab)) {
    g_autoptr(GPtrArray) selected = wig_tab_list_get_selected(self);
    for (guint i = 0; i < selected->len; i++)
      g_signal_emit(self, signals[SIGNAL_RELOAD_TAB], 0, wig_tab_get_id(g_ptr_array_index(selected, i)));
  } else {
    g_signal_emit(self, signals[SIGNAL_RELOAD_TAB], 0, g_variant_get_uint32(parameter));
  }
}

static void on_action_mute(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;
  if (wig_tab_get_selected(tab)) {
    g_autoptr(GPtrArray) selected = wig_tab_list_get_selected(self);
    for (guint i = 0; i < selected->len; i++)
      g_signal_emit(self, signals[SIGNAL_MUTE_TAB], 0, wig_tab_get_id(g_ptr_array_index(selected, i)));
  } else {
    g_signal_emit(self, signals[SIGNAL_MUTE_TAB], 0, g_variant_get_uint32(parameter));
  }
}

static void on_action_duplicate(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;
  if (wig_tab_get_selected(tab)) {
    g_autoptr(GPtrArray) selected = wig_tab_list_get_selected(self);
    for (guint i = 0; i < selected->len; i++)
      g_signal_emit(self, signals[SIGNAL_DUPLICATE_TAB], 0, wig_tab_get_id(g_ptr_array_index(selected, i)));
  } else {
    g_signal_emit(self, signals[SIGNAL_DUPLICATE_TAB], 0, g_variant_get_uint32(parameter));
  }
}

static void on_action_copy_link(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;
  if (wig_tab_get_selected(tab)) {
    g_autoptr(GPtrArray) selected = wig_tab_list_get_selected(self);
    for (guint i = 0; i < selected->len; i++)
      g_signal_emit(self, signals[SIGNAL_COPY_LINK_TAB], 0, wig_tab_get_id(g_ptr_array_index(selected, i)));
  } else {
    g_signal_emit(self, signals[SIGNAL_COPY_LINK_TAB], 0, g_variant_get_uint32(parameter));
  }
}

static void on_action_pin(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;

  /* The clicked tab decides the direction for a mixed selection. */
  gboolean pinned = !wig_tab_get_pinned(tab);
  if (wig_tab_get_selected(tab)) {
    g_autoptr(GPtrArray) selected = wig_tab_list_get_selected(self);
    for (guint i = 0; i < selected->len; i++)
      wig_tab_list_set_pinned(self, g_ptr_array_index(selected, i), pinned);
  } else {
    wig_tab_list_set_pinned(self, tab, pinned);
  }
}

/* The tab being looked at cannot simply go blank, so it hands over to the
 * nearest one that is staying, searching outwards from where it sits. */
static WigTab *wig_tab_list_nearest_outside(WigTabList *self, guint pos, GPtrArray *excluded)
{
  for (guint distance = 1; distance <= self->tabs->len; distance++) {
    if (pos >= distance) {
      WigTab *tab = g_ptr_array_index(self->tabs, pos - distance);
      if (!g_ptr_array_find(excluded, tab, NULL))
        return tab;
    }
    if (pos + distance < self->tabs->len) {
      WigTab *tab = g_ptr_array_index(self->tabs, pos + distance);
      if (!g_ptr_array_find(excluded, tab, NULL))
        return tab;
    }
  }
  return NULL;
}

void wig_tab_list_discard_many(WigTabList *self, GPtrArray *tabs)
{
  if (self->active && g_ptr_array_find(tabs, self->active, NULL)) {
    WigTab *survivor = wig_tab_list_nearest_outside(self, wig_tab_list_index_of(self, self->active), tabs);
    if (survivor)
      wig_tab_list_set_active(self, survivor);
  }

  for (guint i = 0; i < tabs->len; i++) {
    WigTab *tab = g_ptr_array_index(tabs, i);

    /* Every tab was asked to go, so the one on screen stays loaded. */
    if (tab == self->active)
      continue;

    wig_tab_discard(tab);
  }
}

void wig_tab_list_discard_unused(WigTabList *self, guint wait_seconds)
{
  g_autoptr(GPtrArray) unused = g_ptr_array_new();

  for (guint i = 0; i < self->tabs->len; i++) {
    WigTab *tab = g_ptr_array_index(self->tabs, i);

    if (wig_tab_get_pinned(tab) || wig_tab_get_discarded(tab))
      continue;

    guint unused_seconds = wig_tab_get_unused_seconds(tab);
    if (unused_seconds < wait_seconds)
      continue;

    g_debug("tab %u: unused for %u seconds", wig_tab_get_id(tab), unused_seconds);
    g_ptr_array_add(unused, tab);
  }

  if (unused->len > 0)
    wig_tab_list_discard_many(self, unused);
}

static void on_action_unload(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;

  if (wig_tab_get_selected(tab)) {
    g_autoptr(GPtrArray) selected = wig_tab_list_get_selected(self);
    wig_tab_list_discard_many(self, selected);
    return;
  }

  g_autoptr(GPtrArray) one = g_ptr_array_new();
  g_ptr_array_add(one, tab);
  wig_tab_list_discard_many(self, one);
}

/* The window takes the whole selection along, so this fires once however many
 * tabs are going. */
static void on_action_move_to_new_window(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  g_signal_emit(self, signals[SIGNAL_DETACH_TAB], 0, g_variant_get_uint32(parameter));
}

static void on_action_close(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;
  if (wig_tab_get_selected(tab)) {
    g_autoptr(GPtrArray) selected = wig_tab_list_get_selected(self);
    wig_tab_list_close_many(self, selected);
  } else {
    wig_tab_list_close(self, tab);
  }
}

static void on_action_close_to_left(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;
  guint pos = wig_tab_list_index_of(self, tab);
  g_autoptr(GPtrArray) tabs = g_ptr_array_new();
  for (guint i = 0; i < pos; i++) {
    WigTab *t = wig_tab_list_get_nth(self, i);
    if (!wig_tab_get_pinned(t))
      g_ptr_array_add(tabs, t);
  }
  wig_tab_list_close_many(self, tabs);
}

static void on_action_close_to_right(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;
  guint pos = wig_tab_list_index_of(self, tab);
  guint n = wig_tab_list_get_n_tabs(self);
  g_autoptr(GPtrArray) tabs = g_ptr_array_new();
  for (guint i = pos + 1; i < n; i++) {
    WigTab *t = wig_tab_list_get_nth(self, i);
    if (!wig_tab_get_pinned(t))
      g_ptr_array_add(tabs, t);
  }
  wig_tab_list_close_many(self, tabs);
}

static void on_action_close_others(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigTabList *self = WIG_TAB_LIST(user_data);
  WigTab *tab = wig_tab_list_get_by_id(self, g_variant_get_uint32(parameter));
  if (!tab)
    return;
  guint n = wig_tab_list_get_n_tabs(self);
  g_autoptr(GPtrArray) tabs = g_ptr_array_new();
  for (guint i = 0; i < n; i++) {
    WigTab *t = wig_tab_list_get_nth(self, i);
    if (t != tab && !wig_tab_get_pinned(t))
      g_ptr_array_add(tabs, t);
  }
  wig_tab_list_close_many(self, tabs);
}

GSimpleActionGroup *wig_tab_list_get_action_group(WigTabList *self)
{
  return self->actions;
}
