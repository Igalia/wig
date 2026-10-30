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

#include "wig-pwa-synthetic-manifest.h"
#include "wig-pwa-util.h"

static void test_page_becomes_an_app(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) manifest = wig_pwa_synthetic_manifest_new("https://example.com/app/page?q=1", "Example App",
                                                                &error);

  g_assert_no_error(error);
  g_assert_nonnull(manifest);
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "name"), ==, "Example App");
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "short_name"), ==, "example.com");
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "start_url"), ==, "https://example.com/app/page?q=1");
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "id"), ==, "https://example.com/app/page?q=1");
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "scope"), ==, "https://example.com/");
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "display"), ==, "standalone");
}

static void test_fragment_is_dropped(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) manifest = wig_pwa_synthetic_manifest_new("https://example.com/read#section", "Reader", &error);

  g_assert_no_error(error);
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "start_url"), ==, "https://example.com/read");
}

static void test_credentials_are_dropped(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) manifest = wig_pwa_synthetic_manifest_new("https://user:secret@example.com/x", "X", &error);

  g_assert_no_error(error);
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "start_url"), ==, "https://example.com/x");
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "scope"), ==, "https://example.com/");
}

static void test_port_is_kept(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) manifest = wig_pwa_synthetic_manifest_new("http://localhost:8080/dev", NULL, &error);

  g_assert_no_error(error);
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "start_url"), ==, "http://localhost:8080/dev");
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "scope"), ==, "http://localhost:8080/");
  /* Without a title the site itself is the best name there is. */
  g_assert_cmpstr(wig_pwa_variant_lookup_string(manifest, "name"), ==, "localhost");
}

static void test_pages_that_are_not_sites(void)
{
  const char *addresses[] = { "wig:new-tab", "about:blank", "file:///tmp/page.html", "", NULL };

  for (guint i = 0; addresses[i]; i++) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) manifest = wig_pwa_synthetic_manifest_new(addresses[i], "Title", &error);

    g_assert_null(manifest);
    g_assert_nonnull(error);
    g_assert_false(wig_pwa_synthetic_manifest_is_possible(addresses[i]));
  }

  g_assert_false(wig_pwa_synthetic_manifest_is_possible(NULL));
  g_assert_true(wig_pwa_synthetic_manifest_is_possible("https://example.com/"));
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/pwa/synthetic-manifest/page", test_page_becomes_an_app);
  g_test_add_func("/pwa/synthetic-manifest/fragment", test_fragment_is_dropped);
  g_test_add_func("/pwa/synthetic-manifest/credentials", test_credentials_are_dropped);
  g_test_add_func("/pwa/synthetic-manifest/port", test_port_is_kept);
  g_test_add_func("/pwa/synthetic-manifest/not-a-site", test_pages_that_are_not_sites);
  return g_test_run();
}
