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

#include "wig-pwa-icon.h"

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static guint64 icon_size_distance(WebKitApplicationManifestIcon *icon, int requested_size, gboolean *has_larger_size)
{
  const char *const *sizes = webkit_application_manifest_icon_get_sizes(icon);
  guint64 largest_smaller_size = 0;
  guint64 requested_size_value = (guint64)requested_size;
  guint64 closest_distance = G_MAXUINT64;
  *has_larger_size = FALSE;
  for (guint i = 0; sizes && sizes[i]; i++) {
    if (g_str_equal(sizes[i], "any")) {
      *has_larger_size = TRUE;
      return 0;
    }

    char *end = NULL;
    guint64 width = g_ascii_strtoull(sizes[i], &end, 10);
    if (end == sizes[i] || *end != 'x')
      continue;
    guint64 height = g_ascii_strtoull(end + 1, &end, 10);
    if (*end || !width || !height)
      continue;
    guint64 size = MIN(width, height);
    if (size >= requested_size_value) {
      *has_larger_size = TRUE;
      closest_distance = MIN(closest_distance, size - requested_size_value);
    } else {
      largest_smaller_size = MAX(largest_smaller_size, size);
    }
  }
  if (!*has_larger_size && largest_smaller_size)
    return requested_size_value - largest_smaller_size;
  return closest_distance;
}

WebKitApplicationManifestIcon *wig_pwa_icon_select(WebKitApplicationManifestIcon *const *icons, int requested_size)
{
  g_return_val_if_fail(requested_size > 0, NULL);

  WebKitApplicationManifestIcon *best_icon = NULL;
  guint best_purpose = 0;
  gboolean best_has_larger_size = FALSE;
  guint64 closest_distance = G_MAXUINT64;
  for (guint i = 0; icons && icons[i]; i++) {
    guint purpose = webkit_application_manifest_icon_get_is_monochrome(icons[i]) ? 0
        : webkit_application_manifest_icon_get_is_maskable(icons[i])             ? 1
                                                                                 : 2;
    gboolean has_larger_size = FALSE;
    guint64 distance = icon_size_distance(icons[i], requested_size, &has_larger_size);
    if (!best_icon || purpose > best_purpose || (purpose == best_purpose && has_larger_size && !best_has_larger_size)
        || (purpose == best_purpose && has_larger_size == best_has_larger_size && distance < closest_distance)) {
      best_icon = icons[i];
      best_purpose = purpose;
      best_has_larger_size = has_larger_size;
      closest_distance = distance;
    }
  }
  return best_icon;
}
#endif
