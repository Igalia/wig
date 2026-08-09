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

#include "wig-pwa-navigation-policy.h"
#include "wig-tab.h"
#include "wig-window-base.h"

G_BEGIN_DECLS

#define WIG_TYPE_PWA_WINDOW (wig_pwa_window_get_type())
G_DECLARE_FINAL_TYPE(WigPwaWindow, wig_pwa_window, WIG, PWA_WINDOW, WigWindowBase)

WigPwaWindow *wig_pwa_window_new(GtkApplication *application, WPEDisplay *display, const char *id,
                                 WigPwaNavigationPolicy *navigation_policy, GVariant *shortcuts,
                                 const char *background_color, WigTab *tab);
const char *wig_pwa_window_get_id(WigPwaWindow *self);
WebKitWebView *wig_pwa_window_get_web_view(WigPwaWindow *self);
gboolean wig_pwa_window_is_current_uri_in_scope(WigPwaWindow *self);

G_END_DECLS
