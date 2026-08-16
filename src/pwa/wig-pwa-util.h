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

static inline const char *wig_pwa_variant_lookup_string(GVariant *dictionary, const char *key)
{
  const char *value = NULL;
  return g_variant_lookup(dictionary, key, "&s", &value) ? value : NULL;
}

static inline const char *wig_pwa_manifest_get_name(GVariant *manifest, const char *fallback)
{
  const char *name = wig_pwa_variant_lookup_string(manifest, "name");
  if (!name || !*name)
    name = wig_pwa_variant_lookup_string(manifest, "short_name");
  return name && *name ? name : fallback;
}

static inline GVariant *wig_pwa_variant_array_get_dict(GVariant *array, gsize index)
{
  g_autoptr(GVariant) value = NULL;
  g_variant_get_child(array, index, "v", &value);
  return g_variant_is_of_type(value, G_VARIANT_TYPE_VARDICT) ? g_steal_pointer(&value) : NULL;
}

static inline gboolean wig_pwa_path_segment_is_dot(const char *segment)
{
  return g_str_equal(segment, ".") || g_ascii_strcasecmp(segment, "%2e") == 0;
}

static inline gboolean wig_pwa_path_segment_is_double_dot(const char *segment)
{
  return g_str_equal(segment, "..") || g_ascii_strcasecmp(segment, ".%2e") == 0
      || g_ascii_strcasecmp(segment, "%2e.") == 0 || g_ascii_strcasecmp(segment, "%2e%2e") == 0;
}

static inline char *wig_pwa_path_remove_dot_segments(const char *path)
{
  gboolean absolute = path[0] == '/';
  g_auto(GStrv) segments = g_strsplit(path, "/", -1);
  g_autoptr(GPtrArray) normalized = g_ptr_array_new_with_free_func(g_free);
  for (guint i = absolute ? 1 : 0; segments[i]; i++) {
    if (wig_pwa_path_segment_is_dot(segments[i]))
      continue;
    if (wig_pwa_path_segment_is_double_dot(segments[i])) {
      if (normalized->len)
        g_ptr_array_remove_index(normalized, normalized->len - 1);
      continue;
    }
    g_ptr_array_add(normalized, g_strdup(segments[i]));
  }

  guint n_segments = g_strv_length(segments);
  const char *last_segment = n_segments ? segments[n_segments - 1] : "";
  gboolean needs_trailing_slash = g_str_has_suffix(path, "/") || wig_pwa_path_segment_is_dot(last_segment)
      || wig_pwa_path_segment_is_double_dot(last_segment);
  g_autoptr(GString) result = g_string_new(absolute ? "/" : NULL);
  for (guint i = 0; i < normalized->len; i++) {
    if (i)
      g_string_append_c(result, '/');
    g_string_append(result, g_ptr_array_index(normalized, i));
  }
  if (needs_trailing_slash && !g_str_has_suffix(result->str, "/"))
    g_string_append_c(result, '/');
  return g_string_free(g_steal_pointer(&result), FALSE);
}
