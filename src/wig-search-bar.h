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

#include "wig-tab.h"

G_BEGIN_DECLS

#define WIG_TYPE_SEARCH_BAR (wig_search_bar_get_type())
G_DECLARE_FINAL_TYPE(WigSearchBar, wig_search_bar, WIG, SEARCH_BAR, GtkWidget)

GtkWidget *wig_search_bar_new(void);
void wig_search_bar_set_tab(WigSearchBar *self, WigTab *tab);
void wig_search_bar_open(WigSearchBar *self);
void wig_search_bar_close(WigSearchBar *self);
gboolean wig_search_bar_is_open(WigSearchBar *self);
void wig_search_bar_find_next(WigSearchBar *self);
void wig_search_bar_find_previous(WigSearchBar *self);

G_END_DECLS
