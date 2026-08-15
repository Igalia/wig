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

G_BEGIN_DECLS

#define WIG_TYPE_NATIVE_PAGE (wig_native_page_get_type())
G_DECLARE_DERIVABLE_TYPE(WigNativePage, wig_native_page, WIG, NATIVE_PAGE, GtkWidget)

struct _WigNativePageClass {
  GtkWidgetClass parent_class;
};

const char *wig_native_page_get_title(WigNativePage *self);
void wig_native_page_set_title(WigNativePage *self, const char *title);
const char *wig_native_page_get_uri(WigNativePage *self);
void wig_native_page_set_uri(WigNativePage *self, const char *uri);

G_END_DECLS
