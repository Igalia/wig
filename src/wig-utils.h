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

#include <gio/gio.h>
#include <glib.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef enum {
  WIG_UTIL_URI_COMPLETION_PASSTHROUGH,
  WIG_UTIL_URI_COMPLETION_HTTPS,
  WIG_UTIL_URI_COMPLETION_HTTP,
  WIG_UTIL_URI_COMPLETION_SEARCH,
} WigUtilUriCompletionType;

WigUtilUriCompletionType wig_util_get_uri_completion_type(const char *url);
char *wig_util_complete_uri(const char *url, const char *search_engine);
char *wig_util_search_uri(const char *terms, const char *search_engine);
char *wig_util_search_engine_name(const char *search_engine);
#if HAVE_FAVICON_SUPPORT
GIcon *wig_util_best_page_icon(WebKitImageList *icons, int min_size);
#endif

G_END_DECLS
