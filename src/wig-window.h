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

#include "wig-application.h"

G_BEGIN_DECLS

#define WIG_TYPE_WINDOW (wig_window_get_type())
G_DECLARE_FINAL_TYPE(WigWindow, wig_window, WIG, WINDOW, GtkApplicationWindow)

typedef enum {
  WIG_TAB_LAYOUT_HORIZONTAL,
  WIG_TAB_LAYOUT_VERTICAL,
} WigTabLayout;

#define WIG_TYPE_TAB_LAYOUT (wig_tab_layout_get_type())
GType wig_tab_layout_get_type(void);

WigWindow *wig_window_new(WigApplication *application);
void wig_window_add_web_view(WigWindow *win, WebKitWebView *web_view);
WebKitWebView *wig_window_focus_tab_by_site(WigWindow *win, const char *uri, WebKitWebView *ignore);

WigWindow *wig_window_restore(WigApplication *app, const WigSessionWindow *saved);
WigSessionWindow *wig_window_capture_session(WigWindow *win);

G_END_DECLS
