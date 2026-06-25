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

#include "wig-tab-widget.h"

/* A fully collapsed tab is just the favicon plus the tab's horizontal padding
 * (see the wig-tab padding in the CSS below). */
#define MIN_TAB_WIDTH (WIG_TAB_FAVICON_SIZE + 20)
#define MAX_TAB_WIDTH 240

struct _WigTabBar {
  GtkWidget parent;

  WigTabList *list;
  GtkWidget *scrolled_window;
  GtkWidget *tab_box;
  GtkWidget *new_tab_button;
  GtkWidget *scroll_left_button;
  GtkWidget *scroll_right_button;

  GSList *tab_widgets;
  int tab_width;

  /* Width distribution is applied from an idle rather than directly, because
   * queueing a resize from within size_allocate() is unreliable.  A pending
   * scroll target (or -1) is carried along so we scroll once widths settle. */
  guint relayout_idle_id;
  int scroll_to_pos;
};

G_DEFINE_FINAL_TYPE(WigTabBar, wig_tab_bar, GTK_TYPE_WIDGET)

enum { SIGNAL_CLOSE_TAB, N_SIGNALS };
static guint signals[N_SIGNALS];

static void wig_tab_bar_queue_relayout(WigTabBar *self, int scroll_to_pos);
static void wig_tab_bar_try_scroll(WigTabBar *self);
static int wig_tab_bar_child_width(GtkWidget *child);
static int wig_tab_bar_available_width(WigTabBar *self);

static void wig_tab_bar_update_active(WigTabBar *self, GParamSpec *pspec, WigTabList *list)
{
  WigTab *active = wig_tab_list_get_active(self->list);
  guint active_pos = 0;
  guint i = 0;
  for (GSList *l = self->tab_widgets; l; l = g_slist_next(l), i++) {
    WigTabWidget *widget = WIG_TAB_WIDGET(l->data);
    if (wig_tab_widget_get_tab(widget) == active) {
      gtk_widget_add_css_class(GTK_WIDGET(widget), "active");
      active_pos = i;
    } else {
      gtk_widget_remove_css_class(GTK_WIDGET(widget), "active");
    }
  }

  /* The active tab is the last to shrink, so a selection change can shift
   * width between tabs; also keep it on screen. */
  wig_tab_bar_queue_relayout(self, active ? (int)active_pos : -1);
}

static void wig_tab_bar_tab_selected_changed(WigTab *tab, GParamSpec *pspec, WigTabWidget *tab_widget)
{
  if (wig_tab_get_selected(tab))
    gtk_widget_add_css_class(GTK_WIDGET(tab_widget), "selected");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(tab_widget), "selected");
}

static void wig_tab_bar_close_requested(WigTabWidget *tab_widget, WigTabBar *self)
{
  g_signal_emit(self, signals[SIGNAL_CLOSE_TAB], 0, wig_tab_widget_get_tab(tab_widget));
}

static void wig_tab_bar_tab_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigTabBar *self)
{
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  WigTab *tab = wig_tab_widget_get_tab(WIG_TAB_WIDGET(widget));
  if (tab)
    wig_tab_list_set_active(self->list, tab);
}

static void wig_tab_bar_tab_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigTabBar *self)
{
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  wig_tab_widget_show_context_menu(WIG_TAB_WIDGET(widget), self->list);
}

static void wig_tab_bar_tab_added(WigTabList *list, WigTab *tab, guint position, WigTabBar *self)
{
  GtkWidget *widget = wig_tab_widget_new(tab);
  WigTabWidget *tab_widget = WIG_TAB_WIDGET(widget);

  wig_tab_widget_set_width(tab_widget, self->tab_width > 0 ? self->tab_width : MAX_TAB_WIDTH);

  g_signal_connect_object(tab_widget, "close-requested", G_CALLBACK(wig_tab_bar_close_requested), self,
                          G_CONNECT_DEFAULT);
  g_signal_connect_object(tab, "notify::selected", G_CALLBACK(wig_tab_bar_tab_selected_changed), tab_widget,
                          G_CONNECT_DEFAULT);

  GtkGestureClick *gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  g_signal_connect_object(gesture, "pressed", G_CALLBACK(wig_tab_bar_tab_pressed), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));

  GtkGestureClick *right_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_gesture), GDK_BUTTON_SECONDARY);
  g_signal_connect_object(right_gesture, "pressed", G_CALLBACK(wig_tab_bar_tab_right_pressed), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(right_gesture));

  GtkWidget *prev_sibling = position > 0 ? GTK_WIDGET(g_slist_nth_data(self->tab_widgets, position - 1)) : NULL;
  self->tab_widgets = g_slist_insert(self->tab_widgets, tab_widget, (gint)position);
  gtk_box_insert_child_after(GTK_BOX(self->tab_box), widget, prev_sibling);

  if (wig_tab_widget_get_tab(tab_widget) == wig_tab_list_get_active(self->list))
    gtk_widget_add_css_class(widget, "active");

  wig_tab_bar_queue_relayout(self, (int)position);
}

static void wig_tab_bar_tab_removed(WigTabList *list, WigTab *tab, guint position, WigTabBar *self)
{
  GSList *link = g_slist_nth(self->tab_widgets, position);
  WigTabWidget *tab_widget = WIG_TAB_WIDGET(link->data);
  self->tab_widgets = g_slist_delete_link(self->tab_widgets, link);
  gtk_box_remove(GTK_BOX(self->tab_box), GTK_WIDGET(tab_widget));

  wig_tab_bar_queue_relayout(self, -1);
}

static void wig_tab_bar_new_tab_clicked(GtkButton *button, WigTabBar *self)
{
  WigTab *tab = NULL;
  g_signal_emit_by_name(self->list, "create-tab", &tab);
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
  return (step > 0.0 ? step : MIN_TAB_WIDTH) * 3;
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
  int width = gtk_widget_get_width(GTK_WIDGET(self)) - wig_tab_bar_child_width(self->new_tab_button);
  guint n = wig_tab_list_get_n_tabs(self->list);
  return (int)n * MIN_TAB_WIDTH > width;
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
    step = MIN_TAB_WIDTH;
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

/* Width available to the tabs: the bar minus the always-present "+" button and,
 * when shown, the </> scroll buttons (which consume bar width too).  The </>
 * buttons are deliberately excluded from the is_scrollable() check (see that
 * function's comment) but must be accounted for here, where we divide the real
 * remaining width among the tabs. */
static int wig_tab_bar_available_width(WigTabBar *self)
{
  return gtk_widget_get_width(GTK_WIDGET(self)) - wig_tab_bar_child_width(self->new_tab_button)
      - wig_tab_bar_child_width(self->scroll_left_button) - wig_tab_bar_child_width(self->scroll_right_button);
}

/* Compute the width of the active tab and of every other tab for the current
 * bar width.  The active tab keeps its full size for as long as possible:
 * every other tab shrinks down to MIN_TAB_WIDTH first, and only once they have
 * all bottomed out does the active tab start to shrink.  When even the active
 * tab can no longer fit, all tabs sit at MIN_TAB_WIDTH, the box overflows its
 * viewport, and the surrounding scrolled window makes the bar scrollable.
 * Returns FALSE if the bar has no usable width yet. */
static gboolean wig_tab_bar_compute_widths(WigTabBar *self, guint n, int *active_width, int *others_width)
{
  int available = wig_tab_bar_available_width(self);
  if (available <= 0 || n == 0)
    return FALSE;

  if (n == 1) {
    *active_width = *others_width = CLAMP(available, MIN_TAB_WIDTH, MAX_TAB_WIDTH);
  } else {
    *active_width = CLAMP(available - (int)(n - 1) * MIN_TAB_WIDTH, MIN_TAB_WIDTH, MAX_TAB_WIDTH);
    *others_width = CLAMP((available - *active_width) / (int)(n - 1), MIN_TAB_WIDTH, MAX_TAB_WIDTH);
  }
  return TRUE;
}

/* Returns TRUE once the tab is fully in view.  May return FALSE while the
 * layout has not settled (the tab has no bounds yet, or the adjustment's upper
 * bound still lags behind a freshly added tab); the caller keeps the request
 * pending and the next layout pass, with up-to-date geometry, completes it. */
static gboolean wig_tab_bar_scroll_to_index(WigTabBar *self, int index)
{
  guint n = wig_tab_list_get_n_tabs(self->list);
  if (index < 0 || (guint)index >= n)
    return TRUE;

  /* Use the tab widget's actual allocated bounds rather than a modelled
   * position: the modelled width (MIN_TAB_WIDTH-based) does not match the real
   * allocated width once CSS padding, the favicon and the label are accounted
   * for, so the modelled x can be hundreds of pixels off for later tabs. */
  GtkWidget *target = GTK_WIDGET(g_slist_nth_data(self->tab_widgets, (guint)index));
  graphene_rect_t bounds;
  if (!target || !gtk_widget_compute_bounds(target, self->tab_box, &bounds))
    return FALSE;
  int x = (int)bounds.origin.x;
  int width = (int)bounds.size.width;

  GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  /* page_size is already the viewport width; the </> buttons live outside the
   * scrolled window, so they must NOT be subtracted from it. */
  double page = gtk_adjustment_get_page_size(adj);
  double value = gtk_adjustment_get_value(adj);
  double lower = gtk_adjustment_get_lower(adj);
  double upper = gtk_adjustment_get_upper(adj);

  double desired = value;
  if (x < value)
    desired = x;
  else if (x + width > value + page)
    desired = x + width - page;

  double clamped = CLAMP(desired, lower, MAX(lower, upper - page));
  gtk_adjustment_set_value(adj, clamped);
  return clamped >= desired - 0.5;
}

static void wig_tab_bar_distribute_width(WigTabBar *self)
{
  guint n = wig_tab_list_get_n_tabs(self->list);
  int active_width;
  int others_width;
  if (!wig_tab_bar_compute_widths(self, n, &active_width, &others_width))
    return;

  /* Remembered as the initial guess for tab widgets created later. */
  self->tab_width = others_width;

  WigTab *active = wig_tab_list_get_active(self->list);
  for (GSList *l = self->tab_widgets; l; l = g_slist_next(l)) {
    WigTabWidget *widget = WIG_TAB_WIDGET(l->data);
    gboolean is_active = wig_tab_widget_get_tab(widget) == active;
    /* wig_tab_widget_set_width() is a no-op when the width is unchanged, so the
     * relayout it triggers settles after a single pass without looping. */
    wig_tab_widget_set_width(widget, is_active ? active_width : others_width);
  }
}

static gboolean wig_tab_bar_relayout_idle(gpointer data)
{
  WigTabBar *self = WIG_TAB_BAR(data);
  self->relayout_idle_id = 0;

  wig_tab_bar_distribute_width(self);

  gboolean buttons_visible = gtk_widget_get_visible(self->scroll_left_button);
  wig_tab_bar_update_scroll_visibility(self);

  /* If button visibility changed a resize is queued and the scrolled window's
   * page_size is stale.  Leave scroll_to_pos set so the upcoming hadjustment
   * "changed" can scroll with the correct page_size.  When visibility is
   * unchanged the page_size is current and we can scroll immediately (this
   * handles active-tab changes where the content width does not change and
   * hadjustment "changed" will not fire). */
  if (gtk_widget_get_visible(self->scroll_left_button) == buttons_visible)
    wig_tab_bar_try_scroll(self);

  return G_SOURCE_REMOVE;
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
    self->relayout_idle_id = g_idle_add(wig_tab_bar_relayout_idle, self);
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

/* The hadjustment emits "changed" when the content width updates, i.e. right
 * after the tab box re-lays-out with new tab widths.  That is the moment a
 * scroll to a just-added tab can finally succeed, since the adjustment's upper
 * bound now includes it, and also when scrollability may have toggled.
 * Visibility is NOT updated here — toggling visibility during a layout pass
 * causes GTK to allocate without measuring; visibility is deferred to the idle. */
static void wig_tab_bar_hadjustment_changed(WigTabBar *self, GtkAdjustment *adj)
{
  wig_tab_bar_try_scroll(self);
  wig_tab_bar_update_scroll_sensitivity(self);
}

/* "value-changed" fires as the strip scrolls, toggling the </> sensitivity. */
static void wig_tab_bar_hadjustment_value_changed(WigTabBar *self, GtkAdjustment *adj)
{
  wig_tab_bar_update_scroll_sensitivity(self);
}

static void wig_tab_bar_dispose(GObject *object)
{
  WigTabBar *self = WIG_TAB_BAR(object);
  g_clear_handle_id(&self->relayout_idle_id, g_source_remove);
  g_clear_pointer(&self->tab_widgets, g_slist_free);
  g_clear_object(&self->list);
  // FIXME: Why do we need to manually unparent?
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

  signals[SIGNAL_CLOSE_TAB] = g_signal_new("close-tab", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 1, WIG_TYPE_TAB);
}

static void wig_tab_bar_load_css(void)
{
  static gboolean loaded = FALSE;
  if (loaded)
    return;
  loaded = TRUE;

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider,
                                    /* Horizontal padding here defines the collapsed (favicon-only)
                                     * width together with WIG_TAB_FAVICON_SIZE; keep them in sync. */
                                    "wig-tab { padding: 4px 10px; min-height: 36px; }"
                                    "wig-tab.active {"
                                    "  background-color: alpha(@accent_bg_color, 0.2);"
                                    "  border-radius: 4px;"
                                    "}");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

GtkWidget *wig_tab_bar_new(WigTabList *list)
{
  g_return_val_if_fail(WIG_IS_TAB_LIST(list), NULL);

  wig_tab_bar_load_css();

  WigTabBar *self = WIG_TAB_BAR(g_object_new(WIG_TYPE_TAB_BAR, NULL));
  self->list = g_object_ref(list);
  /* Small gap between the scrolled tab strip and the flanking buttons. */
  gtk_box_layout_set_spacing(GTK_BOX_LAYOUT(gtk_widget_get_layout_manager(GTK_WIDGET(self))), 6);

  /* Leftmost child: the "<" scroll button, shown only while scrollable. */
  self->scroll_left_button = gtk_button_new_from_icon_name("pan-start-symbolic");
  gtk_widget_add_css_class(self->scroll_left_button, "flat");
  gtk_widget_set_visible(self->scroll_left_button, FALSE);
  g_signal_connect_object(self->scroll_left_button, "clicked", G_CALLBACK(wig_tab_bar_scroll_left_clicked), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_set_parent(self->scroll_left_button, GTK_WIDGET(self));

  self->tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  /* GTK_POLICY_EXTERNAL: no scrollbar widget is shown, but the adjustment still
   * scrolls (driven by wig_tab_bar_scroll() and the scroll-left/right actions). */
  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  /* Hug the tab strip's natural width (rather than expanding to fill) so that
   * while the tabs fit, the trailing "+" button sits right after the last tab.
   * Once the strip overflows, the box layout caps the scrolled window at the
   * remaining width, the strip scrolls inside it, and the "+" button is left
   * pinned at the right edge. */
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(self->scrolled_window), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window), self->tab_box);
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

  g_signal_connect_object(list, "tab-added", G_CALLBACK(wig_tab_bar_tab_added), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(list, "tab-removed", G_CALLBACK(wig_tab_bar_tab_removed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(list, "notify::active-tab", G_CALLBACK(wig_tab_bar_update_active), self, G_CONNECT_SWAPPED);

  return GTK_WIDGET(self);
}
