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

#include "wig-pwa-launch-handler.h"

static gboolean parse_client_mode(const char *value, WigPwaLaunchClientMode *mode)
{
  if (g_strcmp0(value, "auto") == 0)
    *mode = WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;
  else if (g_strcmp0(value, "focus-existing") == 0)
    *mode = WIG_PWA_LAUNCH_CLIENT_MODE_FOCUS_EXISTING;
  else if (g_strcmp0(value, "navigate-existing") == 0)
    *mode = WIG_PWA_LAUNCH_CLIENT_MODE_NAVIGATE_EXISTING;
  else if (g_strcmp0(value, "navigate-new") == 0)
    *mode = WIG_PWA_LAUNCH_CLIENT_MODE_NAVIGATE_NEW;
  else
    return FALSE;
  return TRUE;
}

WigPwaLaunchClientMode wig_pwa_launch_handler_get_client_mode(GVariant *manifest)
{
  g_return_val_if_fail(manifest && g_variant_is_of_type(manifest, G_VARIANT_TYPE_VARDICT),
                       WIG_PWA_LAUNCH_CLIENT_MODE_AUTO);

  g_autoptr(GVariant) launch_handler = g_variant_lookup_value(manifest, "launch_handler", G_VARIANT_TYPE_VARDICT);
  if (!launch_handler)
    return WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;

  g_autoptr(GVariant) client_mode = g_variant_lookup_value(launch_handler, "client_mode", NULL);
  if (!client_mode)
    return WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;

  WigPwaLaunchClientMode mode;
  if (g_variant_is_of_type(client_mode, G_VARIANT_TYPE_STRING))
    return parse_client_mode(g_variant_get_string(client_mode, NULL), &mode) ? mode : WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;

  if (!g_variant_is_of_type(client_mode, G_VARIANT_TYPE("av")))
    return WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;

  gsize length = g_variant_n_children(client_mode);
  for (gsize i = 0; i < length; i++) {
    g_autoptr(GVariant) value = NULL;
    g_variant_get_child(client_mode, i, "v", &value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)
        && parse_client_mode(g_variant_get_string(value, NULL), &mode))
      return mode;
  }
  return WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;
}
