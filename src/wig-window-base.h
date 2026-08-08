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
#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

#define WIG_TYPE_WINDOW_BASE (wig_window_base_get_type())
G_DECLARE_DERIVABLE_TYPE(WigWindowBase, wig_window_base, WIG, WINDOW_BASE, GtkApplicationWindow)

struct _WigWindowBaseClass {
  GtkApplicationWindowClass parent_class;

  void (*loading_changed)(WigWindowBase *self, gboolean is_loading);
};

guint wig_window_base_get_id(WigWindowBase *self);
void wig_window_base_set_toplevel(WigWindowBase *self, WPEToplevel *toplevel);
WPEToplevel *wig_window_base_get_toplevel(WigWindowBase *self);
void wig_window_base_set_navigation_buttons(WigWindowBase *self, GtkWidget *back_button, GtkWidget *forward_button);
void wig_window_base_attach_web_view(WigWindowBase *self, WebKitWebView *web_view);
void wig_window_base_detach_web_view(WigWindowBase *self, WebKitWebView *web_view);
void wig_window_base_set_active_web_view(WigWindowBase *self, WebKitWebView *web_view);
WebKitWebView *wig_window_base_get_active_web_view(WigWindowBase *self);
GtkWidget *wig_window_base_get_permissions_button(WigWindowBase *self);
GtkWidget *wig_window_base_get_downloads_button(WigWindowBase *self);
const char *wig_window_base_get_active_origin(WigWindowBase *self);

G_END_DECLS
