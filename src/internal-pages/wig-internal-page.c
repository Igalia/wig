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

#include "wig-internal-page.h"

static const char *error_page = "<!DOCTYPE html><html><body>Error loading page.</body></html>";

char *wig_internal_page_render(const char *resource_path, TmplScope *scope)
{
  g_autoptr(TmplTemplate) tmpl = tmpl_template_new(NULL);
  g_autoptr(GError) error = NULL;
  if (!tmpl_template_parse_resource(tmpl, resource_path, NULL, &error)) {
    g_warning("Failed to parse template '%s': %s", resource_path, error->message);
    return g_strdup(error_page);
  }

  char *html = tmpl_template_expand_string(tmpl, scope, &error);
  if (!html) {
    g_warning("Failed to expand template '%s': %s", resource_path, error->message);
    return g_strdup(error_page);
  }

  return html;
}
