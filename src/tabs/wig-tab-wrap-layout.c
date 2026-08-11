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

#include "wig-tab-wrap-layout.h"

/*
 * Lays pinned tabs out left to right, starting a new row whenever the next one
 * would not fit.  A horizontal GtkBox cannot do this: in the sidebar it would
 * force the pane wider than the user sized it as soon as a few tabs are pinned.
 *
 * Pinned tabs are all a favicon wide, so the cells are uniform: every child is
 * given the size of the largest one, and the row a child lands on follows from
 * its index alone.
 */

struct _WigTabWrapLayout {
  GtkLayoutManager parent;
};

G_DEFINE_FINAL_TYPE(WigTabWrapLayout, wig_tab_wrap_layout, GTK_TYPE_LAYOUT_MANAGER)

/* The cell every child is allocated, and how many children there are to place. */
static void wig_tab_wrap_layout_cell(GtkWidget *widget, int *cell_width, int *cell_height, int *n_children)
{
  int width = 0;
  int height = 0;
  int n = 0;

  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    if (!gtk_widget_should_layout(c))
      continue;

    int child_width, child_height;
    gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &child_width, NULL, NULL);
    gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, child_width, NULL, &child_height, NULL, NULL);
    width = MAX(width, child_width);
    height = MAX(height, child_height);
    n++;
  }

  *cell_width = MAX(width, 1);
  *cell_height = height;
  *n_children = n;
}

static int wig_tab_wrap_layout_columns(int width, int cell_width, int n_children)
{
  return CLAMP(width / cell_width, 1, MAX(n_children, 1));
}

/* How tall the rows end up depends on the width they are given, and a layout
 * manager reports constant size unless it says otherwise. */
static GtkSizeRequestMode wig_tab_wrap_layout_get_request_mode(GtkLayoutManager *manager, GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void wig_tab_wrap_layout_measure(GtkLayoutManager *manager, GtkWidget *widget, GtkOrientation orientation,
                                        int for_size, int *minimum, int *natural, int *minimum_baseline,
                                        int *natural_baseline)
{
  int cell_width, cell_height, n;
  wig_tab_wrap_layout_cell(widget, &cell_width, &cell_height, &n);

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    /* Minimum: a single column, the narrowest the rows can be split into.
     * Natural: everything on one row. */
    *minimum = n ? cell_width : 0;
    *natural = cell_width * n;
  } else {
    int columns = for_size >= 0 ? wig_tab_wrap_layout_columns(for_size, cell_width, n) : MAX(n, 1);
    int rows = (n + columns - 1) / columns;
    *minimum = *natural = rows * cell_height;
  }

  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void wig_tab_wrap_layout_allocate(GtkLayoutManager *manager, GtkWidget *widget, int width, int height,
                                         int baseline)
{
  int cell_width, cell_height, n;
  wig_tab_wrap_layout_cell(widget, &cell_width, &cell_height, &n);
  int columns = wig_tab_wrap_layout_columns(width, cell_width, n);

  int i = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    if (!gtk_widget_should_layout(c))
      continue;

    GtkAllocation alloc = {
      .x = (i % columns) * cell_width, .y = (i / columns) * cell_height, .width = cell_width, .height = cell_height
    };
    gtk_widget_size_allocate(c, &alloc, -1);
    i++;
  }
}

static void wig_tab_wrap_layout_class_init(WigTabWrapLayoutClass *klass)
{
  GtkLayoutManagerClass *layout_class = GTK_LAYOUT_MANAGER_CLASS(klass);
  layout_class->get_request_mode = wig_tab_wrap_layout_get_request_mode;
  layout_class->measure = wig_tab_wrap_layout_measure;
  layout_class->allocate = wig_tab_wrap_layout_allocate;
}

static void wig_tab_wrap_layout_init(WigTabWrapLayout *self)
{
}

GtkLayoutManager *wig_tab_wrap_layout_new(void)
{
  return GTK_LAYOUT_MANAGER(g_object_new(WIG_TYPE_TAB_WRAP_LAYOUT, NULL));
}
