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

#include "wig-tab-strip-layout.h"

/*
 * The tab widgets live in a horizontal strip inside a GtkScrolledWindow that
 * expands to fill the bar.  A plain GtkBox would size each tab to its natural
 * width and hand leftover space to the hexpanding tabs, so a single tab fills
 * the whole strip and WIG_TAB_MAX_WIDTH is never honoured.
 *
 * This layout manager instead divides the width it is allocated evenly among the
 * tabs, clamped to [tab minimum, WIG_TAB_MAX_WIDTH].  Because that allocated
 * width is the scrolled window's viewport, the tabs widen and narrow as the
 * window resizes with no involvement from the tab bar.  Its reported minimum
 * width is n * (tab minimum); once the viewport drops below that the strip
 * overflows and the scrolled window scrolls, with every tab pinned at its
 * minimum width.
 */

struct _WigTabStripLayout {
  GtkLayoutManager parent;
};

G_DEFINE_FINAL_TYPE(WigTabStripLayout, wig_tab_strip_layout, GTK_TYPE_LAYOUT_MANAGER)

/* Count the tabs that participate in layout. */
static int wig_tab_strip_layout_n_tabs(GtkWidget *widget)
{
  int n = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    if (gtk_widget_should_layout(c))
      n++;
  }
  return n;
}

/* The widest tab's own minimum width (CSS min-width plus favicon, label and
 * close button); @widget is the tab box.  Returns 0 when it has no tabs. */
int wig_tab_strip_layout_child_min_width(GtkWidget *widget)
{
  int floor = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    if (!gtk_widget_should_layout(c))
      continue;
    int child_min;
    gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &child_min, NULL, NULL, NULL);
    floor = MAX(floor, child_min);
  }
  return floor;
}

/* The width each tab gets for a strip allocated @for_width: divide the space
 * evenly, but never below the tab minimum (so the strip overflows and scrolls)
 * nor above WIG_TAB_MAX_WIDTH (so tabs don't grow unbounded on a wide window). */
static int wig_tab_strip_layout_tab_width(GtkWidget *widget, int for_width)
{
  int n = wig_tab_strip_layout_n_tabs(widget);
  if (n == 0)
    return WIG_TAB_MAX_WIDTH;
  int min = wig_tab_strip_layout_child_min_width(widget);
  return CLAMP(for_width / n, min, WIG_TAB_MAX_WIDTH);
}

static void wig_tab_strip_layout_measure(GtkLayoutManager *manager, GtkWidget *widget, GtkOrientation orientation,
                                         int for_size, int *minimum, int *natural, int *minimum_baseline,
                                         int *natural_baseline)
{
  int n = wig_tab_strip_layout_n_tabs(widget);
  int min = 0;
  int nat = 0;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    /* Minimum: every tab at its own minimum width — below this the strip
     * overflows and the scrolled window scrolls.  Natural: every tab at
     * WIG_TAB_MAX_WIDTH, the most the strip ever wants. */
    min = n * wig_tab_strip_layout_child_min_width(widget);
    nat = n * WIG_TAB_MAX_WIDTH;
  } else {
    /* Height is the tallest tab, measured at the width it will be given. */
    int tab_width = wig_tab_strip_layout_tab_width(widget, for_size >= 0 ? for_size : nat);
    for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
      if (!gtk_widget_should_layout(c))
        continue;
      int child_min, child_nat;
      gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, tab_width, &child_min, &child_nat, NULL, NULL);
      min = MAX(min, child_min);
      nat = MAX(nat, child_nat);
    }
  }

  *minimum = min;
  *natural = nat;
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void wig_tab_strip_layout_allocate(GtkLayoutManager *manager, GtkWidget *widget, int width, int height,
                                          int baseline)
{
  /* width is the space the scrolled window gave the strip: the viewport width
   * when the tabs fit, or the (larger) minimum strip width when they overflow.
   * Either way, dividing it gives the right per-tab width and tracks resizes
   * with no help from the tab bar. */
  int tab_width = wig_tab_strip_layout_tab_width(widget, width);
  int x = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    if (!gtk_widget_should_layout(c))
      continue;
    GtkAllocation alloc = { .x = x, .y = 0, .width = tab_width, .height = height };
    gtk_widget_size_allocate(c, &alloc, -1);
    x += tab_width;
  }
}

static void wig_tab_strip_layout_class_init(WigTabStripLayoutClass *klass)
{
  GtkLayoutManagerClass *layout_class = GTK_LAYOUT_MANAGER_CLASS(klass);
  layout_class->measure = wig_tab_strip_layout_measure;
  layout_class->allocate = wig_tab_strip_layout_allocate;
}

static void wig_tab_strip_layout_init(WigTabStripLayout *self)
{
}

GtkLayoutManager *wig_tab_strip_layout_new(void)
{
  return GTK_LAYOUT_MANAGER(g_object_new(WIG_TYPE_TAB_STRIP_LAYOUT, NULL));
}
