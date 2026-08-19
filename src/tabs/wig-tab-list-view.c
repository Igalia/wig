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

#include "wig-tab-list-view.h"

typedef struct {
  WigTabList *list;
  GtkBox *pinned_box;
  GtkWidget *separator;
  GtkBox *tab_box;
  GtkOrientation orientation;
  GSList *tab_widgets; /* WigTabWidget*, in list order: pinned tabs first */

  double drag_hot_x;
  double drag_hot_y;
  GtkWidget *drag_widget; /* owned while dragging */
  int drop_indicator;

  /* Index of the last tab that was Ctrl+clicked; used as the anchor for
   * Shift+click range selection.  -1 when no anchor has been set yet. */
  int last_selected_index;

  gboolean collapse_selection;
} WigTabListViewPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(WigTabListView, wig_tab_list_view, GTK_TYPE_WIDGET)

static void wig_tab_list_view_sync_placement(WigTabListView *self);

static void wig_tab_list_view_update_active(WigTabListView *self, GParamSpec *pspec, WigTabList *list)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  WigTab *active = wig_tab_list_get_active(priv->list);
  for (GSList *l = priv->tab_widgets; l; l = g_slist_next(l)) {
    WigTabWidget *widget = WIG_TAB_WIDGET(l->data);
    if (wig_tab_widget_get_tab(widget) == active)
      gtk_widget_add_css_class(GTK_WIDGET(widget), "active");
    else
      gtk_widget_remove_css_class(GTK_WIDGET(widget), "active");
  }
}

static void wig_tab_list_view_clear_selection(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  for (GSList *l = priv->tab_widgets; l; l = g_slist_next(l))
    wig_tab_set_selected(wig_tab_widget_get_tab(WIG_TAB_WIDGET(l->data)), FALSE);
  priv->last_selected_index = -1;
}

static void wig_tab_list_view_background_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                                 WigTabListView *self)
{
  /* This gesture covers the whole view, so it also sees presses that landed on a
   * tab.  Those belong to the tab's own handler — clearing here would drop the
   * selection just as a drag of it is starting. */
  for (GtkWidget *w = gtk_widget_pick(GTK_WIDGET(self), x, y, GTK_PICK_DEFAULT); w && w != GTK_WIDGET(self);
       w = gtk_widget_get_parent(w)) {
    if (WIG_IS_TAB_WIDGET(w))
      return;
  }

  GdkEvent *event = gtk_gesture_get_last_event(GTK_GESTURE(gesture), NULL);
  GdkModifierType state = event ? gdk_event_get_modifier_state(event) : 0;
  if (!(state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)))
    wig_tab_list_view_clear_selection(self);
}

static void wig_tab_list_view_tab_selected_changed(WigTab *tab, GParamSpec *pspec, WigTabWidget *tab_widget)
{
  if (wig_tab_get_selected(tab))
    gtk_widget_add_css_class(GTK_WIDGET(tab_widget), "selected");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(tab_widget), "selected");
}

static void wig_tab_list_view_close_requested(WigTabWidget *tab_widget, WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  wig_tab_list_close(priv->list, wig_tab_widget_get_tab(tab_widget));
}

static void wig_tab_list_view_tab_middle_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                                 WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  wig_tab_list_close(priv->list, wig_tab_widget_get_tab(WIG_TAB_WIDGET(widget)));
}

static void wig_tab_list_view_tab_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                          WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  WigTab *tab = wig_tab_widget_get_tab(WIG_TAB_WIDGET(widget));
  if (!tab)
    return;

  GdkEvent *event = gtk_gesture_get_last_event(GTK_GESTURE(gesture), NULL);
  GdkModifierType state = event ? gdk_event_get_modifier_state(event) : 0;
  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;

  if (ctrl) {
    wig_tab_set_selected(tab, !wig_tab_get_selected(tab));
    priv->last_selected_index = (int)wig_tab_list_index_of(priv->list, tab);
  } else if (shift && priv->last_selected_index >= 0) {
    int current_index = (int)wig_tab_list_index_of(priv->list, tab);
    int from = MIN(priv->last_selected_index, current_index);
    int to = MAX(priv->last_selected_index, current_index);
    for (int i = from; i <= to; i++) {
      WigTab *t = wig_tab_list_get_nth(priv->list, (guint)i);
      wig_tab_set_selected(t, !wig_tab_get_selected(t));
    }
  } else if (wig_tab_get_selected(tab)) {
    /* Pressing inside a selection has to leave it standing, because the press
     * may be the start of a drag that carries the whole selection.  A press that
     * turns out to be a plain click collapses it on release instead. */
    priv->collapse_selection = TRUE;
    wig_tab_list_set_active(priv->list, tab);
  } else {
    wig_tab_list_view_clear_selection(self);
    wig_tab_list_set_active(priv->list, tab);
  }
}

static void wig_tab_list_view_tab_released(GtkGestureClick *gesture, int n_press, double x, double y,
                                           WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  if (priv->collapse_selection)
    wig_tab_list_view_clear_selection(self);
  priv->collapse_selection = FALSE;
}

static void wig_tab_list_view_tab_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                                WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  WigTab *tab = wig_tab_widget_get_tab(WIG_TAB_WIDGET(widget));

  if (!wig_tab_get_selected(tab))
    wig_tab_list_view_clear_selection(self);

  wig_tab_widget_show_context_menu(WIG_TAB_WIDGET(widget), priv->list);
}

static void wig_tab_list_view_set_drop_indicator(WigTabListView *self, int index)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  if (priv->drop_indicator == index)
    return;
  priv->drop_indicator = index;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static GType wig_tab_id_get_type(void)
{
  static GType type = 0;
  if (g_once_init_enter(&type)) {
    GType t = g_pointer_type_register_static("WigTabId");
    g_once_init_leave(&type, t);
  }
  return type;
}

static GdkContentProvider *wig_tab_list_view_drag_prepare(GtkDragSource *source, double x, double y,
                                                          WigTabListView *self)
{
  /* Store the cursor-within-widget hotspot for use in drag_begin. */
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  priv->drag_hot_x = x;
  priv->drag_hot_y = y;

  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
  guint32 tab_id = wig_tab_get_id(wig_tab_widget_get_tab(WIG_TAB_WIDGET(widget)));
  GValue value = G_VALUE_INIT;
  g_value_init(&value, wig_tab_id_get_type());
  g_value_set_pointer(&value, GUINT_TO_POINTER(tab_id));
  return gdk_content_provider_new_for_value(&value);
}

static void wig_tab_list_view_drag_cancelled(GdkDrag *drag, GdkDragCancelReason reason, WigTabListView *self)
{
  if (reason != GDK_DRAG_CANCEL_NO_TARGET)
    return;

  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  if (!priv->list || !priv->drag_widget)
    return;
  if (wig_tab_list_get_n_tabs(priv->list) <= 1)
    return;

  WigTab *tab = wig_tab_widget_get_tab(WIG_TAB_WIDGET(priv->drag_widget));
  gtk_widget_activate_action(GTK_WIDGET(self), "tab.detach", "u", wig_tab_get_id(tab));
}

static void wig_tab_list_view_drag_begin(GtkDragSource *source, GdkDrag *drag, WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));

  /* GTK keeps the drag source alive until the drag finishes, so drag-end still
   * arrives after a cross-window drop has destroyed this window's tab widget. */
  g_set_object(&priv->drag_widget, widget);
  priv->collapse_selection = FALSE;
  gtk_widget_add_css_class(GTK_WIDGET(self), "tab-drag-active");
  g_signal_connect_object(drag, "cancel", G_CALLBACK(wig_tab_list_view_drag_cancelled), self, G_CONNECT_DEFAULT);

  /* Show the pinned area even when empty: it is the drop zone that pins. */
  gtk_widget_set_visible(GTK_WIDGET(priv->pinned_box), TRUE);
  gtk_widget_set_visible(priv->separator, TRUE);

  /* Take a static snapshot before adding .dragging so the icon is undimmed. */
  double w = gtk_widget_get_width(widget);
  double h = gtk_widget_get_height(widget);
  g_autoptr(GdkPaintable) live = gtk_widget_paintable_new(widget);
  g_autoptr(GtkSnapshot) snap = gtk_snapshot_new();
  gdk_paintable_snapshot(GDK_PAINTABLE(live), GDK_SNAPSHOT(snap), w, h);
  graphene_size_t size = GRAPHENE_SIZE_INIT((float)w, (float)h);
  g_autoptr(GdkPaintable) paintable = gtk_snapshot_to_paintable(snap, &size);
  gtk_drag_source_set_icon(source, paintable, (int)priv->drag_hot_x, (int)priv->drag_hot_y);

  gtk_widget_add_css_class(widget, "dragging");
}

static void wig_tab_list_view_drag_end(GtkDragSource *source, GdkDrag *drag, gboolean delete_data, WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);

  g_autoptr(GtkWidget) widget = g_steal_pointer(&priv->drag_widget);

  gtk_widget_remove_css_class(GTK_WIDGET(self), "tab-drag-active");
  if (widget)
    gtk_widget_remove_css_class(widget, "dragging");
  wig_tab_list_view_set_drop_indicator(self, -1);
  wig_tab_list_view_sync_placement(self);
}

/* Returns the index in tab_widgets that @widget corresponds to, or -1. */
static int wig_tab_list_view_widget_index(WigTabListView *self, GtkWidget *widget)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  int i = 0;
  for (GSList *l = priv->tab_widgets; l; l = g_slist_next(l), i++) {
    if (GTK_WIDGET(l->data) == widget)
      return i;
  }
  return -1;
}

/* The two boxes do not have to run the same way — the sidebar stacks its tabs
 * downwards but wraps the pinned ones across — so which axis a drop divides on
 * follows the box the tab under the cursor sits in. */
static GtkOrientation wig_tab_list_view_tab_orientation(WigTabListView *self, GtkWidget *tab_widget)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  WigTab *tab = wig_tab_widget_get_tab(WIG_TAB_WIDGET(tab_widget));

  /* Pinned tabs are favicon-sized and run across in both layouts; the rest
   * follow the view.  The boxes cannot be asked, because GtkBox forwards
   * "orientation" to its layout manager and both of ours replace it. */
  return tab && wig_tab_get_pinned(tab) ? GTK_ORIENTATION_HORIZONTAL : priv->orientation;
}

/* Compute the insertion index given the cursor position within a tab widget.
 * Returns the index before which the dragged tab should be inserted. */
static int wig_tab_list_view_compute_insert_index(WigTabListView *self, GtkWidget *drop_widget, double x, double y)
{
  int target_index = wig_tab_list_view_widget_index(self, drop_widget);
  if (target_index < 0)
    return -1;

  GtkOrientation orientation = wig_tab_list_view_tab_orientation(self, drop_widget);
  double pos = orientation == GTK_ORIENTATION_HORIZONTAL ? x : y;
  int size = orientation == GTK_ORIENTATION_HORIZONTAL ? gtk_widget_get_width(drop_widget)
                                                       : gtk_widget_get_height(drop_widget);

  /* Insert after the target when in the right/bottom half. */
  return (pos > size / 2.0) ? target_index + 1 : target_index;
}

/* Only accept drags originating from within this process (same-process drags
 * have a non-NULL GdkDrag; cross-app drags return NULL). */
static gboolean wig_tab_list_view_drop_accept(GtkDropTarget *target, GdkDrop *drop, WigTabListView *self)
{
  /* Accept any drop offering G_TYPE_UINT — the GtkDropTarget type filter
   * already rejects foreign content that doesn't provide this type. */
  return gdk_content_formats_contain_gtype(gdk_drop_get_formats(drop), wig_tab_id_get_type());
}

static GdkDragAction wig_tab_list_view_tab_drop_motion(GtkDropTarget *target, double x, double y, WigTabListView *self)
{
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
  int insert_index = wig_tab_list_view_compute_insert_index(self, widget, x, y);
  wig_tab_list_view_set_drop_indicator(self, insert_index);
  return GDK_ACTION_MOVE;
}

static void wig_tab_list_view_tab_drop_leave(GtkDropTarget *target, WigTabListView *self)
{
  wig_tab_list_view_set_drop_indicator(self, -1);
}

static gboolean wig_tab_list_view_tab_drop(GtkDropTarget *target, const GValue *value, double x, double y,
                                           WigTabListView *self)
{
  wig_tab_list_view_set_drop_indicator(self, -1);

  if (!G_VALUE_HOLDS(value, wig_tab_id_get_type()))
    return FALSE;

  guint32 tab_id = GPOINTER_TO_UINT(g_value_get_pointer(value));

  GtkWidget *drop_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
  int insert_index = wig_tab_list_view_compute_insert_index(self, drop_widget, x, y);
  if (insert_index < 0)
    return FALSE;

  /* Landing among the pinned tabs pins the dropped tab, and landing among the
   * regular ones unpins it. */
  gboolean pinned = wig_tab_get_pinned(wig_tab_widget_get_tab(WIG_TAB_WIDGET(drop_widget)));

  /* tab.move-to handles both same-window reorder and cross-window moves. */
  gtk_widget_activate_action(GTK_WIDGET(self), "tab.move-to", "(uub)", tab_id, (guint32)insert_index, pinned);

  return TRUE;
}

/* Drop onto the pinned area — pin the tab and put it at the end of the block. */
static gboolean wig_tab_list_view_pinned_box_drop(GtkDropTarget *target, const GValue *value, double x, double y,
                                                  WigTabListView *self)
{
  wig_tab_list_view_set_drop_indicator(self, -1);

  if (!G_VALUE_HOLDS(value, wig_tab_id_get_type()))
    return FALSE;

  guint32 tab_id = GPOINTER_TO_UINT(g_value_get_pointer(value));
  gtk_widget_activate_action(GTK_WIDGET(self), "tab.move-to", "(uub)", tab_id, G_MAXUINT32, TRUE);

  return TRUE;
}

/* Drop onto the tab box past the last tab — append. */
static gboolean wig_tab_list_view_box_drop(GtkDropTarget *target, const GValue *value, double x, double y,
                                           WigTabListView *self)
{
  wig_tab_list_view_set_drop_indicator(self, -1);

  if (!G_VALUE_HOLDS(value, wig_tab_id_get_type()))
    return FALSE;

  guint32 tab_id = GPOINTER_TO_UINT(g_value_get_pointer(value));

  /* Append: pass G_MAXUINT32 as insert_index; tab.move-to clamps to n.  Past the
   * last tab is regular territory, so the tab is unpinned if it was pinned. */
  gtk_widget_activate_action(GTK_WIDGET(self), "tab.move-to", "(uub)", tab_id, G_MAXUINT32, FALSE);

  return TRUE;
}

/* Put every tab widget in the box its tab belongs to, in list order. The list
 * keeps pinned tabs at the front, so walking it in order fills the pinned box
 * and then the regular one, whatever it was that changed. */
static void wig_tab_list_view_sync_placement(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);

  /* Moving the last tab out takes the window with it, and the drag still ends
   * on this view afterwards. */
  if (!priv->tab_box)
    return;

  GtkWidget *prev_pinned = NULL;
  GtkWidget *prev_regular = NULL;
  gboolean any_pinned = FALSE;

  for (GSList *l = priv->tab_widgets; l; l = g_slist_next(l)) {
    GtkWidget *widget = GTK_WIDGET(l->data);
    gboolean pinned = wig_tab_get_pinned(wig_tab_widget_get_tab(WIG_TAB_WIDGET(widget)));
    GtkBox *box = pinned ? priv->pinned_box : priv->tab_box;
    GtkWidget **prev = pinned ? &prev_pinned : &prev_regular;
    GtkWidget *parent = gtk_widget_get_parent(widget);

    if (parent == GTK_WIDGET(box)) {
      gtk_box_reorder_child_after(box, widget, *prev);
    } else {
      /* The box it is leaving holds the only reference to it. */
      g_autoptr(GtkWidget) held = parent ? g_object_ref(widget) : NULL;
      if (parent)
        gtk_box_remove(GTK_BOX(parent), widget);
      gtk_box_insert_child_after(box, widget, *prev);
    }

    *prev = widget;
    any_pinned |= pinned;
  }

  gtk_widget_set_visible(GTK_WIDGET(priv->pinned_box), any_pinned);
  gtk_widget_set_visible(priv->separator, any_pinned);
}

static void wig_tab_list_view_tab_pinned_changed(WigTab *tab, GParamSpec *pspec, WigTabListView *self)
{
  wig_tab_list_view_sync_placement(self);
}

static void wig_tab_list_view_tab_added(WigTabList *list, WigTab *tab, guint position, WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);

  GtkWidget *widget = wig_tab_widget_new(tab);
  WigTabWidget *tab_widget = WIG_TAB_WIDGET(widget);

  g_signal_connect_object(tab_widget, "close-requested", G_CALLBACK(wig_tab_list_view_close_requested), self,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(tab, "notify::selected", G_CALLBACK(wig_tab_list_view_tab_selected_changed), tab_widget,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(tab, "notify::pinned", G_CALLBACK(wig_tab_list_view_tab_pinned_changed), self,
                          G_CONNECT_DEFAULT);

  GtkGestureClick *gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  g_signal_connect_object(gesture, "pressed", G_CALLBACK(wig_tab_list_view_tab_pressed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(gesture, "released", G_CALLBACK(wig_tab_list_view_tab_released), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));

  GtkGestureClick *right_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_gesture), GDK_BUTTON_SECONDARY);
  g_signal_connect_object(right_gesture, "pressed", G_CALLBACK(wig_tab_list_view_tab_right_pressed), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(right_gesture));

  GtkGestureClick *middle_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle_gesture), GDK_BUTTON_MIDDLE);
  g_signal_connect_object(middle_gesture, "pressed", G_CALLBACK(wig_tab_list_view_tab_middle_pressed), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(middle_gesture));

  GtkDragSource *drag_source = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
  g_signal_connect_object(drag_source, "prepare", G_CALLBACK(wig_tab_list_view_drag_prepare), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(drag_source, "drag-begin", G_CALLBACK(wig_tab_list_view_drag_begin), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(drag_source, "drag-end", G_CALLBACK(wig_tab_list_view_drag_end), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag_source));

  GtkDropTarget *drop_target = gtk_drop_target_new(wig_tab_id_get_type(), GDK_ACTION_MOVE);
  g_signal_connect_object(drop_target, "accept", G_CALLBACK(wig_tab_list_view_drop_accept), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(drop_target, "motion", G_CALLBACK(wig_tab_list_view_tab_drop_motion), self,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(drop_target, "leave", G_CALLBACK(wig_tab_list_view_tab_drop_leave), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(drop_target, "drop", G_CALLBACK(wig_tab_list_view_tab_drop), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drop_target));

  priv->tab_widgets = g_slist_insert(priv->tab_widgets, tab_widget, (gint)position);
  wig_tab_list_view_sync_placement(self);

  if (wig_tab_widget_get_tab(tab_widget) == wig_tab_list_get_active(priv->list))
    gtk_widget_add_css_class(widget, "active");

  /* A tab dragged in from another window keeps its selection, and the fresh
   * widget has to show it without waiting for the property to change again. */
  wig_tab_list_view_tab_selected_changed(tab, NULL, tab_widget);
  priv->last_selected_index = -1;

  WigTabListViewClass *klass = WIG_TAB_LIST_VIEW_GET_CLASS(self);
  if (klass->tab_widget_added)
    klass->tab_widget_added(self, tab_widget, position);
}

static void wig_tab_list_view_tab_moved(WigTabList *list, WigTab *tab, guint old_index, guint new_index,
                                        WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);

  GSList *link = g_slist_nth(priv->tab_widgets, old_index);
  GtkWidget *widget = GTK_WIDGET(link->data);

  priv->tab_widgets = g_slist_delete_link(priv->tab_widgets, link);
  priv->tab_widgets = g_slist_insert(priv->tab_widgets, widget, (gint)new_index);
  wig_tab_list_view_sync_placement(self);
  priv->last_selected_index = -1;
}

static void wig_tab_list_view_tab_removed(WigTabList *list, WigTab *tab, guint position, WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  GSList *link = g_slist_nth(priv->tab_widgets, position);
  WigTabWidget *tab_widget = WIG_TAB_WIDGET(link->data);
  priv->tab_widgets = g_slist_delete_link(priv->tab_widgets, link);
  gtk_box_remove(GTK_BOX(gtk_widget_get_parent(GTK_WIDGET(tab_widget))), GTK_WIDGET(tab_widget));
  wig_tab_list_view_sync_placement(self);
  priv->last_selected_index = -1;
}

/* ---------- drop line rendering ------------------------------------------ */

static void wig_tab_list_view_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
  WigTabListView *self = WIG_TAB_LIST_VIEW(widget);
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);

  GTK_WIDGET_CLASS(wig_tab_list_view_parent_class)->snapshot(widget, snapshot);

  if (priv->drop_indicator < 0)
    return;

  guint n = wig_tab_list_get_n_tabs(priv->list);
  if (n == 0)
    return;

  /* Determine where to draw the line: at the left/top edge of the widget at
   * drop_indicator, or at the right/bottom edge of the last widget. */
  GtkWidget *ref_widget = NULL;
  gboolean use_end = FALSE;
  if ((guint)priv->drop_indicator >= n) {
    ref_widget = GTK_WIDGET(g_slist_nth_data(priv->tab_widgets, n - 1));
    use_end = TRUE;
  } else {
    ref_widget = GTK_WIDGET(g_slist_nth_data(priv->tab_widgets, (guint)priv->drop_indicator));
  }

  if (!ref_widget)
    return;

  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds(ref_widget, widget, &bounds))
    return;

  GdkRGBA color;
  gtk_widget_get_color(widget, &color);

  const float LINE_HALF = 1.5f;

  if (wig_tab_list_view_tab_orientation(self, ref_widget) == GTK_ORIENTATION_HORIZONTAL) {
    float x = use_end ? bounds.origin.x + bounds.size.width : bounds.origin.x;
    graphene_rect_t line_rect = GRAPHENE_RECT_INIT(x - LINE_HALF, bounds.origin.y, LINE_HALF * 2.0f,
                                                   bounds.size.height);
    gtk_snapshot_append_color(snapshot, &color, &line_rect);
  } else {
    float y = use_end ? bounds.origin.y + bounds.size.height : bounds.origin.y;
    graphene_rect_t line_rect = GRAPHENE_RECT_INIT(bounds.origin.x, y - LINE_HALF, bounds.size.width, LINE_HALF * 2.0f);
    gtk_snapshot_append_color(snapshot, &color, &line_rect);
  }
}

/* ---------- boilerplate -------------------------------------------------- */

static void wig_tab_list_view_dispose(GObject *object)
{
  WigTabListView *self = WIG_TAB_LIST_VIEW(object);
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  g_clear_object(&priv->drag_widget);
  g_clear_pointer(&priv->tab_widgets, g_slist_free);
  g_clear_object(&priv->list);
  priv->pinned_box = NULL;
  priv->separator = NULL;
  priv->tab_box = NULL;
  G_OBJECT_CLASS(wig_tab_list_view_parent_class)->dispose(object);
}

static void wig_tab_list_view_init(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  priv->drop_indicator = -1;
  priv->last_selected_index = -1;
}

static void wig_tab_list_view_class_init(WigTabListViewClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_list_view_dispose;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  widget_class->snapshot = wig_tab_list_view_snapshot;
}

void wig_tab_list_view_setup(WigTabListView *self, WigTabList *list, GtkBox *pinned_box, GtkWidget *separator,
                             GtkBox *tab_box, GtkOrientation orientation)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);

  priv->list = g_object_ref(list);
  priv->pinned_box = pinned_box;
  priv->separator = separator;
  priv->tab_box = tab_box;
  priv->orientation = orientation;

  gtk_widget_set_visible(GTK_WIDGET(pinned_box), FALSE);
  gtk_widget_set_visible(separator, FALSE);

  g_signal_connect_object(list, "tab-added", G_CALLBACK(wig_tab_list_view_tab_added), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(list, "tab-removed", G_CALLBACK(wig_tab_list_view_tab_removed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(list, "tab-moved", G_CALLBACK(wig_tab_list_view_tab_moved), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(list, "notify::active-tab", G_CALLBACK(wig_tab_list_view_update_active), self,
                          G_CONNECT_SWAPPED);

  GtkGestureClick *bg_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  g_signal_connect_object(bg_gesture, "pressed", G_CALLBACK(wig_tab_list_view_background_pressed), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(bg_gesture));

  GtkDropTarget *box_drop_target = gtk_drop_target_new(wig_tab_id_get_type(), GDK_ACTION_MOVE);
  g_signal_connect_object(box_drop_target, "accept", G_CALLBACK(wig_tab_list_view_drop_accept), self,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(box_drop_target, "drop", G_CALLBACK(wig_tab_list_view_box_drop), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(GTK_WIDGET(tab_box), GTK_EVENT_CONTROLLER(box_drop_target));

  GtkDropTarget *pinned_drop_target = gtk_drop_target_new(wig_tab_id_get_type(), GDK_ACTION_MOVE);
  g_signal_connect_object(pinned_drop_target, "accept", G_CALLBACK(wig_tab_list_view_drop_accept), self,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(pinned_drop_target, "drop", G_CALLBACK(wig_tab_list_view_pinned_box_drop), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(GTK_WIDGET(pinned_box), GTK_EVENT_CONTROLLER(pinned_drop_target));

  guint n = wig_tab_list_get_n_tabs(list);
  for (guint i = 0; i < n; i++)
    wig_tab_list_view_tab_added(list, wig_tab_list_get_nth(list, i), i, self);
}

WigTabList *wig_tab_list_view_get_list(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  return priv->list;
}

GtkBox *wig_tab_list_view_get_tab_box(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  return priv->tab_box;
}

GSList *wig_tab_list_view_get_tab_widgets(WigTabListView *self)
{
  WigTabListViewPrivate *priv = wig_tab_list_view_get_instance_private(self);
  return priv->tab_widgets;
}
