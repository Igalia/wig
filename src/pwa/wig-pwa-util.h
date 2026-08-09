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

#include <glib.h>

/* Look up a string member of a vardict, returning a borrowed value or NULL. */
static inline const char *wig_pwa_variant_lookup_string(GVariant *dictionary, const char *key)
{
  const char *value = NULL;
  return g_variant_lookup(dictionary, key, "&s", &value) ? value : NULL;
}

/* Whether a URL path segment is a "." (single-dot) segment, including its
 * percent-encoded spelling. */
static inline gboolean wig_pwa_path_segment_is_dot(const char *segment)
{
  return g_str_equal(segment, ".") || g_ascii_strcasecmp(segment, "%2e") == 0;
}

/* Whether a URL path segment is a ".." (double-dot) segment, including its
 * percent-encoded spellings. */
static inline gboolean wig_pwa_path_segment_is_double_dot(const char *segment)
{
  return g_str_equal(segment, "..") || g_ascii_strcasecmp(segment, ".%2e") == 0
      || g_ascii_strcasecmp(segment, "%2e.") == 0 || g_ascii_strcasecmp(segment, "%2e%2e") == 0;
}
