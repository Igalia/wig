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

static GVariant *manifest_with_client_mode(GVariant *client_mode)
{
  GVariantBuilder launch_handler;
  g_variant_builder_init(&launch_handler, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&launch_handler, "{sv}", "client_mode", client_mode);

  GVariantBuilder manifest;
  g_variant_builder_init(&manifest, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&manifest, "{sv}", "launch_handler", g_variant_builder_end(&launch_handler));
  return g_variant_ref_sink(g_variant_builder_end(&manifest));
}

static GVariant *client_mode_array(const char *const *values)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("av"));
  for (guint i = 0; values[i]; i++)
    g_variant_builder_add(&builder, "v", g_variant_new_string(values[i]));
  return g_variant_ref_sink(g_variant_builder_end(&builder));
}

static void test_default_mode(void)
{
  GVariantBuilder manifest_builder;
  g_variant_builder_init(&manifest_builder, G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) manifest = g_variant_ref_sink(g_variant_builder_end(&manifest_builder));
  g_assert_cmpint(wig_pwa_launch_handler_get_client_mode(manifest), ==, WIG_PWA_LAUNCH_CLIENT_MODE_AUTO);

  g_autoptr(GVariant) invalid = manifest_with_client_mode(g_variant_new_string("invalid"));
  g_assert_cmpint(wig_pwa_launch_handler_get_client_mode(invalid), ==, WIG_PWA_LAUNCH_CLIENT_MODE_AUTO);
}

static void test_string_modes(void)
{
  const struct {
    const char *value;
    WigPwaLaunchClientMode expected;
  } cases[] = {
    { "auto", WIG_PWA_LAUNCH_CLIENT_MODE_AUTO },
    { "focus-existing", WIG_PWA_LAUNCH_CLIENT_MODE_FOCUS_EXISTING },
    { "navigate-existing", WIG_PWA_LAUNCH_CLIENT_MODE_NAVIGATE_EXISTING },
    { "navigate-new", WIG_PWA_LAUNCH_CLIENT_MODE_NAVIGATE_NEW },
  };

  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(GVariant) manifest = manifest_with_client_mode(g_variant_new_string(cases[i].value));
    g_assert_cmpint(wig_pwa_launch_handler_get_client_mode(manifest), ==, cases[i].expected);
  }
}

static void test_array_uses_first_valid_mode(void)
{
  const char *values[] = { "invalid", "navigate-existing", "focus-existing", NULL };
  g_autoptr(GVariant) modes = client_mode_array(values);
  g_autoptr(GVariant) manifest = manifest_with_client_mode(modes);
  g_assert_cmpint(wig_pwa_launch_handler_get_client_mode(manifest), ==, WIG_PWA_LAUNCH_CLIENT_MODE_NAVIGATE_EXISTING);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/pwa/launch-handler/default", test_default_mode);
  g_test_add_func("/pwa/launch-handler/string", test_string_modes);
  g_test_add_func("/pwa/launch-handler/array", test_array_uses_first_valid_mode);
  return g_test_run();
}
