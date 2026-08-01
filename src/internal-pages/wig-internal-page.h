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

#include <tmpl-glib.h>
#include <wpe/webkit.h>

char *wig_internal_page_render(const char *resource_path, TmplScope *scope);

char *wig_internal_page_html_escape(const char *text);

/**
 * WigFormBodyReadyFunc:
 * @request: the request whose body was read
 * @params: (nullable): the parsed form parameters, or %NULL if reading failed
 * @user_data: the data passed to wig_internal_page_read_form_body()
 *
 * Called once a request body has been read and parsed.
 */
typedef void (*WigFormBodyReadyFunc)(WebKitURISchemeRequest *request, GHashTable *params, gpointer user_data);

/*
 * If @request is a POST with a body, asynchronously reads the body as
 * application/x-www-form-urlencoded and invokes @callback with the parsed
 * parameters once done, then returns %TRUE: the caller should return and let
 * @callback finish the request.
 *
 * Otherwise returns %FALSE and the caller should handle the request itself.
 * @user_data is freed with @user_data_destroy in either case.
 */
gboolean wig_internal_page_read_form_body(WebKitURISchemeRequest *request, WigFormBodyReadyFunc callback,
                                          gpointer user_data, GDestroyNotify user_data_destroy);
