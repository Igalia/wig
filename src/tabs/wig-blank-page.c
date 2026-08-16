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

#include "wig-blank-page.h"

#define WIG_BLANK_PAGE_TITLE "New Tab"

struct _WigBlankPage {
  WigNativePage parent;
};

G_DEFINE_FINAL_TYPE(WigBlankPage, wig_blank_page, WIG_TYPE_NATIVE_PAGE)

/* An empty document is drawn by whatever WebKit thinks a page with nothing in it
 * looks like, which is a white rectangle in a window that may be dark. The
 * addresses wig answers with nothing at all are shown as nothing at all. */
gboolean uri_is_blank_page(const char *uri)
{
  if (!uri)
    return FALSE;

  return g_str_equal(uri, "about:blank") || g_strcmp0(g_uri_peek_scheme(uri), "wig") == 0;
}

static void wig_blank_page_class_init(WigBlankPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_css_name(widget_class, "wig-blank-page");
}

static void wig_blank_page_init(WigBlankPage *self)
{
  wig_native_page_set_title(WIG_NATIVE_PAGE(self), WIG_BLANK_PAGE_TITLE);
}

GtkWidget *wig_blank_page_new(const char *uri)
{
  return g_object_new(WIG_TYPE_BLANK_PAGE, "uri", uri, NULL);
}
