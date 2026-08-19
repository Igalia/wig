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

#include <gtk/gtk.h>

#include "wig-tab-list.h"
#include "wig-tab-widget.h"

G_BEGIN_DECLS

#define WIG_TYPE_TAB_LIST_VIEW (wig_tab_list_view_get_type())
G_DECLARE_DERIVABLE_TYPE(WigTabListView, wig_tab_list_view, WIG, TAB_LIST_VIEW, GtkWidget)

struct _WigTabListViewClass {
  GtkWidgetClass parent_class;

  /* Called after a WigTabWidget is created and inserted into the box, allowing
   * subclasses to perform additional setup (e.g. set width on the bar). */
  void (*tab_widget_added)(WigTabListView *self, WigTabWidget *tab_widget, guint position);
};

/* Called by subclass _new functions after g_object_new to wire up the list and
 * the boxes, connect signals, and replay any already-existing tabs.
 *
 * Pinned tabs live in @pinned_box, the rest in @tab_box; @separator sits between
 * the two and, like @pinned_box, is hidden while no tab is pinned. */
void wig_tab_list_view_setup(WigTabListView *self, WigTabList *list, GtkBox *pinned_box, GtkWidget *separator,
                             GtkBox *tab_box, GtkOrientation orientation);

WigTabList *wig_tab_list_view_get_list(WigTabListView *self);
GtkBox *wig_tab_list_view_get_tab_box(WigTabListView *self);
GSList *wig_tab_list_view_get_tab_widgets(WigTabListView *self);

G_END_DECLS
