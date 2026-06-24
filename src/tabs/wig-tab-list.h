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
#include <wpe/webkit.h>

#include "wig-tab.h"

G_BEGIN_DECLS

#define WIG_TYPE_TAB_LIST (wig_tab_list_get_type())
G_DECLARE_FINAL_TYPE(WigTabList, wig_tab_list, WIG, TAB_LIST, GObject)

WigTabList *wig_tab_list_new(void);

WigTab *wig_tab_list_append(WigTabList *self, WebKitWebView *web_view);
void wig_tab_list_close(WigTabList *self, WigTab *tab);
void wig_tab_list_move(WigTabList *self, WigTab *tab, guint new_index);

guint wig_tab_list_get_n_tabs(WigTabList *self);
WigTab *wig_tab_list_get_nth(WigTabList *self, guint i);
guint wig_tab_list_index_of(WigTabList *self, WigTab *tab);

WigTab *wig_tab_list_get_active(WigTabList *self);
void wig_tab_list_set_active(WigTabList *self, WigTab *tab);

G_END_DECLS
