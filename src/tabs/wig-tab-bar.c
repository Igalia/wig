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

#include "wig-tab-bar.h"

#include "wig-tab-strip-layout.h"
#include "wig-tab-widget.h"

#define DEFAULT_SCROLL_STEP 100

struct _WigTabBar {
  WigTabListView parent;

  GtkWidget *pinned_box;
  GtkWidget *separator;
  GtkWidget *scrolled_window;
  GtkWidget *new_tab_button;
  GtkWidget *scroll_left_button;
  GtkWidget *scroll_right_button;

  guint relayout_idle_id;
  guint settle_idle_id;
  int scroll_to_pos;
};

G_DEFINE_FINAL_TYPE(WigTabBar, wig_tab_bar, WIG_TYPE_TAB_LIST_VIEW)

static void wig_tab_bar_queue_relayout(WigTabBar *self, int scroll_to_pos);
static void wig_tab_bar_try_scroll(WigTabBar *self);
static int wig_tab_bar_child_width(GtkWidget *child);

static void wig_tab_bar_new_tab_clicked(GtkButton *button, WigTabBar *self)
{
  WigTab *tab = NULL;
  g_signal_emit_by_name(wig_tab_list_view_get_list(WIG_TAB_LIST_VIEW(self)), "create-tab", &tab);
}

/* Scroll the strip by @delta pixels, clamped to the scrollable range. */
static void wig_tab_bar_scroll_by(WigTabBar *self, double delta)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  double lower = gtk_adjustment_get_lower(adj);
  double upper = gtk_adjustment_get_upper(adj);
  double page = gtk_adjustment_get_page_size(adj);
  gtk_adjustment_set_value(adj, CLAMP(gtk_adjustment_get_value(adj) + delta, lower, MAX(lower, upper - page)));
}

/* One "step" of scrolling, used by the keyboard actions and the </> buttons. */
static double wig_tab_bar_scroll_step(WigTabBar *self)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  double step = gtk_adjustment_get_step_increment(adj);
  return (step > 0.0 ? step : DEFAULT_SCROLL_STEP) * 3;
}

static void wig_tab_bar_scroll_left(GtkWidget *widget, const char *action_name, GVariant *parameter)
{
  WigTabBar *self = WIG_TAB_BAR(widget);
  wig_tab_bar_scroll_by(self, -wig_tab_bar_scroll_step(self));
}

static void wig_tab_bar_scroll_right(GtkWidget *widget, const char *action_name, GVariant *parameter)
{
  WigTabBar *self = WIG_TAB_BAR(widget);
  wig_tab_bar_scroll_by(self, wig_tab_bar_scroll_step(self));
}

static void wig_tab_bar_scroll_left_clicked(GtkButton *button, WigTabBar *self)
{
  wig_tab_bar_scroll_by(self, -wig_tab_bar_scroll_step(self));
}

static void wig_tab_bar_scroll_right_clicked(GtkButton *button, WigTabBar *self)
{
  wig_tab_bar_scroll_by(self, wig_tab_bar_scroll_step(self));
}

/* The strip is scrollable when the tabs cannot fit even at their minimum width.
 * This is measured against the width available *without* the </> buttons, so
 * that showing the buttons (which themselves consume width) cannot be what tips
 * the strip into overflow and keep them stuck on. */
static gboolean wig_tab_bar_is_scrollable(WigTabBar *self)
{
  int width = gtk_widget_get_width(GTK_WIDGET(self)) - wig_tab_bar_child_width(self->new_tab_button)
      - wig_tab_bar_child_width(self->pinned_box) - wig_tab_bar_child_width(self->separator);
  WigTabList *list = wig_tab_list_view_get_list(WIG_TAB_LIST_VIEW(self));
  guint n = wig_tab_list_get_n_tabs(list) - wig_tab_list_get_n_pinned(list);
  GtkWidget *tab_box = GTK_WIDGET(wig_tab_list_view_get_tab_box(WIG_TAB_LIST_VIEW(self)));
  return (int)n * wig_tab_strip_layout_child_min_width(tab_box) > width;
}

/* Desensitise each button when scrolled all the way to that end.  Safe to call
 * during layout: it only toggles sensitivity, which does not queue a resize. */
static void wig_tab_bar_update_scroll_sensitivity(WigTabBar *self)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  double lower = gtk_adjustment_get_lower(adj);
  double upper = gtk_adjustment_get_upper(adj);
  double page = gtk_adjustment_get_page_size(adj);
  double value = gtk_adjustment_get_value(adj);
  gtk_widget_set_sensitive(self->scroll_left_button, value > lower + 0.5);
  gtk_widget_set_sensitive(self->scroll_right_button, value < upper - page - 0.5);
}

/* Show the </> buttons only while the strip is scrollable.  Toggling visibility
 * queues a resize, so this must run outside a layout pass (from the idle). */
static void wig_tab_bar_update_scroll_visibility(WigTabBar *self)
{
  gboolean scrollable = wig_tab_bar_is_scrollable(self);
  gtk_widget_set_visible(self->scroll_left_button, scrollable);
  gtk_widget_set_visible(self->scroll_right_button, scrollable);
}

/* GtkScrolledWindow only redirects a vertical wheel to horizontal scrolling
 * while Shift is held, so a plain wheel does nothing over a horizontal-only
 * list.  Translate vertical wheel deltas into horizontal scrolling ourselves;
 * horizontal deltas (e.g. a touchpad) fall through to the default handler. */
static gboolean wig_tab_bar_scroll(GtkEventControllerScroll *controller, double dx, double dy, WigTabBar *self)
{
  if (dy == 0.0)
    return GDK_EVENT_PROPAGATE;

  GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  double step = gtk_adjustment_get_step_increment(adj);
  if (step <= 0.0)
    step = DEFAULT_SCROLL_STEP;
  double upper = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
  gtk_adjustment_set_value(adj,
                           CLAMP(gtk_adjustment_get_value(adj) + dy * step * 3, gtk_adjustment_get_lower(adj), upper));
  return GDK_EVENT_STOP;
}

/* Natural width a child contributes to the bar, or 0 when it is hidden. */
static int wig_tab_bar_child_width(GtkWidget *child)
{
  int width = 0;
  if (gtk_widget_get_visible(child))
    gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &width, NULL, NULL);
  return width;
}

/* Returns TRUE once the tab is fully in view.  May return FALSE while the layout
 * has not settled (the tab has no real allocation yet); the caller keeps the
 * request pending and the next layout pass, with up-to-date geometry, retries.
 *
 * Uses the tab widget's actual allocated bounds to account for CSS. */
static gboolean wig_tab_bar_scroll_to_index(WigTabBar *self, int index)
{
  WigTabList *list = wig_tab_list_view_get_list(WIG_TAB_LIST_VIEW(self));
  guint n = wig_tab_list_get_n_tabs(list);
  if (index < 0 || (guint)index >= n)
    return TRUE;

  /* Pinned tabs are outside the strip and always in view. */
  if (wig_tab_get_pinned(wig_tab_list_get_nth(list, (guint)index)))
    return TRUE;

  GtkWidget *target = GTK_WIDGET(
      g_slist_nth_data(wig_tab_list_view_get_tab_widgets(WIG_TAB_LIST_VIEW(self)), (guint)index));
  if (!target || gtk_widget_get_width(target) <= 0)
    return FALSE; /* not allocated yet — retry once layout settles */

  GtkWidget *tab_box = GTK_WIDGET(wig_tab_list_view_get_tab_box(WIG_TAB_LIST_VIEW(self)));
  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds(target, tab_box, &bounds))
    return FALSE;

  GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  /* page_size is already the viewport width; the </> buttons live outside the
   * scrolled window, so they must NOT be subtracted from it. */
  double page = gtk_adjustment_get_page_size(adj);
  double value = gtk_adjustment_get_value(adj);
  double lower = gtk_adjustment_get_lower(adj);
  double upper = gtk_adjustment_get_upper(adj);

  /* compute_bounds returns the tab's border box, excluding its CSS margin, so
   * aligning to it leaves the tab's margin (10px on the last tab) showing as a
   * gap at the viewport edge.  Take the slot edges from the neighbouring tabs
   * instead — the midpoint of the inter-tab gap — and from the content bounds
   * (lower/upper) at the ends, so the margin is included on both sides. */
  GtkWidget *prev = gtk_widget_get_prev_sibling(target);
  GtkWidget *next = gtk_widget_get_next_sibling(target);
  graphene_rect_t nb;
  double left = lower;
  if (prev && gtk_widget_compute_bounds(prev, tab_box, &nb))
    left = (nb.origin.x + nb.size.width + bounds.origin.x) / 2.0;
  double right = upper;
  if (next && gtk_widget_compute_bounds(next, tab_box, &nb))
    right = (bounds.origin.x + bounds.size.width + nb.origin.x) / 2.0;

  double x = left;
  double width = right - left;

  double desired = value;
  if (x < value)
    desired = x;
  else if (x + width > value + page)
    desired = x + width - page;

  double clamped = CLAMP(desired, lower, MAX(lower, upper - page));
  gtk_adjustment_set_value(adj, clamped);
  return clamped >= desired - 0.5;
}

static void wig_tab_bar_settle_idle(gpointer data)
{
  WigTabBar *self = WIG_TAB_BAR(data);
  self->settle_idle_id = 0;

  wig_tab_bar_try_scroll(self);
  wig_tab_bar_update_scroll_sensitivity(self);
}

/* First of two idle passes.  Toggling the </> buttons' visibility here queues a
 * resize, so the strip geometry is stale by the time this returns; the actual
 * scroll is therefore deferred to wig_tab_bar_settle_idle, which runs on the
 * next iteration once that resize has been laid out and the geometry is final. */
static void wig_tab_bar_relayout_idle(gpointer data)
{
  WigTabBar *self = WIG_TAB_BAR(data);
  self->relayout_idle_id = 0;

  wig_tab_bar_update_scroll_visibility(self);
  if (self->settle_idle_id == 0)
    self->settle_idle_id = g_idle_add_once(wig_tab_bar_settle_idle, self);
}

/* Apply width distribution (and any pending scroll) from an idle.  Calling
 * gtk_widget_set_size_request() during size_allocate() leaves the queued resize
 * unprocessed until the next unrelated relayout, which is why the tabs used to
 * stay at their minimum width until a tab was added; deferring avoids that.
 * scroll_to_pos of -1 leaves any already-pending scroll target untouched. */
static void wig_tab_bar_queue_relayout(WigTabBar *self, int scroll_to_pos)
{
  if (scroll_to_pos >= 0)
    self->scroll_to_pos = scroll_to_pos;
  if (self->relayout_idle_id == 0)
    self->relayout_idle_id = g_idle_add_once(wig_tab_bar_relayout_idle, self);
}

static void wig_tab_bar_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
  WigTabBar *self = WIG_TAB_BAR(widget);
  GTK_WIDGET_CLASS(wig_tab_bar_parent_class)->size_allocate(widget, width, height, baseline);
  wig_tab_bar_queue_relayout(self, -1);
}

/* Attempt any pending scroll, clearing it once the tab is fully in view. */
static void wig_tab_bar_try_scroll(WigTabBar *self)
{
  if (self->scroll_to_pos >= 0 && wig_tab_bar_scroll_to_index(self, self->scroll_to_pos))
    self->scroll_to_pos = -1;
}

/* The hadjustment emits "changed" when the viewport or content width updates:
 * after the tab box re-lays-out with new widths, and on every window resize (the
 * page_size changes).  That is the moment a scroll to a just-added tab can
 * succeed, since the adjustment's upper bound now includes it, and also when
 * scrollability may have toggled.  This is the bar's reliable resize signal —
 * the bar's own size_allocate does not fire on resize — so the relayout idle is
 * scheduled here to refresh the </> button visibility.  Visibility is NOT
 * toggled inline: doing so during a layout pass makes GTK allocate without
 * measuring, so it is deferred to the idle. */
static void wig_tab_bar_hadjustment_changed(WigTabBar *self, GtkAdjustment *adj)
{
  wig_tab_bar_update_scroll_sensitivity(self);
  wig_tab_bar_queue_relayout(self, -1);
}

/* "value-changed" fires as the strip scrolls, toggling the </> sensitivity. */
static void wig_tab_bar_hadjustment_value_changed(WigTabBar *self, GtkAdjustment *adj)
{
  wig_tab_bar_update_scroll_sensitivity(self);
}

static void wig_tab_bar_tab_widget_added(WigTabListView *view, WigTabWidget *tab_widget, guint position)
{
  WigTabBar *self = WIG_TAB_BAR(view);
  wig_tab_bar_queue_relayout(self, (int)position);
}

static void wig_tab_bar_active_tab_changed(WigTabBar *self, GParamSpec *pspec, WigTabList *list)
{
  WigTab *active = wig_tab_list_get_active(list);
  if (active)
    wig_tab_bar_queue_relayout(self, (int)wig_tab_list_index_of(list, active));
}

static void wig_tab_bar_dispose(GObject *object)
{
  WigTabBar *self = WIG_TAB_BAR(object);
  g_clear_handle_id(&self->relayout_idle_id, g_source_remove);
  g_clear_handle_id(&self->settle_idle_id, g_source_remove);
  g_clear_pointer(&self->pinned_box, gtk_widget_unparent);
  g_clear_pointer(&self->separator, gtk_widget_unparent);
  g_clear_pointer(&self->scroll_left_button, gtk_widget_unparent);
  g_clear_pointer(&self->new_tab_button, gtk_widget_unparent);
  g_clear_pointer(&self->scroll_right_button, gtk_widget_unparent);
  g_clear_pointer(&self->scrolled_window, gtk_widget_unparent);
  G_OBJECT_CLASS(wig_tab_bar_parent_class)->dispose(object);
}

static void wig_tab_bar_init(WigTabBar *self)
{
  self->scroll_to_pos = -1;
}

static void wig_tab_bar_class_init(WigTabBarClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_bar_dispose;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  widget_class->size_allocate = wig_tab_bar_size_allocate;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-tab-bar");

  gtk_widget_class_install_action(widget_class, "tab.scroll-left", NULL, wig_tab_bar_scroll_left);
  gtk_widget_class_install_action(widget_class, "tab.scroll-right", NULL, wig_tab_bar_scroll_right);

  gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Left, GDK_CONTROL_MASK, "tab.scroll-left", NULL);
  gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Right, GDK_CONTROL_MASK, "tab.scroll-right", NULL);

  WigTabListViewClass *view_class = WIG_TAB_LIST_VIEW_CLASS(klass);
  view_class->tab_widget_added = wig_tab_bar_tab_widget_added;
}

GtkWidget *wig_tab_bar_new(WigTabList *list)
{
  g_return_val_if_fail(WIG_IS_TAB_LIST(list), NULL);

  WigTabBar *self = WIG_TAB_BAR(g_object_new(WIG_TYPE_TAB_BAR, NULL));
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);

  /* Pinned tabs sit left of the scrollable strip, always in view. */
  self->pinned_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->pinned_box, "pinned-tabs");
  gtk_widget_set_parent(self->pinned_box, GTK_WIDGET(self));

  self->separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(self->separator, "pinned-separator");
  gtk_widget_set_parent(self->separator, GTK_WIDGET(self));

  /* The "<" scroll button, shown only while scrollable. */
  self->scroll_left_button = gtk_button_new_from_icon_name("pan-start-symbolic");
  gtk_widget_add_css_class(self->scroll_left_button, "flat");
  gtk_widget_set_visible(self->scroll_left_button, FALSE);
  g_signal_connect_object(self->scroll_left_button, "clicked", G_CALLBACK(wig_tab_bar_scroll_left_clicked), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_set_parent(self->scroll_left_button, GTK_WIDGET(self));

  GtkBox *tab_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_set_layout_manager(GTK_WIDGET(tab_box), wig_tab_strip_layout_new());

  /* GTK_POLICY_EXTERNAL: no scrollbar widget is shown, but the adjustment still
   * scrolls (driven by wig_tab_bar_scroll() and the scroll-left/right actions). */
  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(self->scrolled_window), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window), GTK_WIDGET(tab_box));
  gtk_widget_set_parent(self->scrolled_window, GTK_WIDGET(self));

  /* Translate vertical wheel scrolling into horizontal scrolling of the strip. */
  GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
  g_signal_connect(scroll, "scroll", G_CALLBACK(wig_tab_bar_scroll), self);
  gtk_widget_add_controller(GTK_WIDGET(self), scroll);

  /* Complete a pending scroll-to-tab once the content width updates, and keep
   * the </> buttons' visibility/sensitivity in sync as content and value change. */
  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  g_signal_connect_object(hadj, "changed", G_CALLBACK(wig_tab_bar_hadjustment_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(hadj, "value-changed", G_CALLBACK(wig_tab_bar_hadjustment_value_changed), self,
                          G_CONNECT_SWAPPED);

  /* The ">" scroll button sits just past the strip, before the "+" button. */
  self->scroll_right_button = gtk_button_new_from_icon_name("pan-end-symbolic");
  gtk_widget_add_css_class(self->scroll_right_button, "flat");
  gtk_widget_set_visible(self->scroll_right_button, FALSE);
  g_signal_connect_object(self->scroll_right_button, "clicked", G_CALLBACK(wig_tab_bar_scroll_right_clicked), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_set_parent(self->scroll_right_button, GTK_WIDGET(self));

  self->new_tab_button = gtk_button_new_from_icon_name("tab-new-symbolic");
  gtk_widget_add_css_class(self->new_tab_button, "flat");
  g_signal_connect_object(self->new_tab_button, "clicked", G_CALLBACK(wig_tab_bar_new_tab_clicked), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_set_parent(self->new_tab_button, GTK_WIDGET(self));

  g_signal_connect_object(list, "notify::active-tab", G_CALLBACK(wig_tab_bar_active_tab_changed), self,
                          G_CONNECT_SWAPPED);

  wig_tab_list_view_setup(WIG_TAB_LIST_VIEW(self), list, GTK_BOX(self->pinned_box), self->separator, tab_box);

  return GTK_WIDGET(self);
}
