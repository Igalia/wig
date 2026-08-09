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

#include "wig-pwa-navigation-policy.h"

static void test_primary_scope(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_navigation_policy_new("https://example.com/app/", NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/"), ==, WIG_PWA_SCOPE_MATCH_PRIMARY);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/page?q=1#section"), ==,
                  WIG_PWA_SCOPE_MATCH_PRIMARY);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com:443/app/page"), ==,
                  WIG_PWA_SCOPE_MATCH_PRIMARY);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app"), ==, WIG_PWA_SCOPE_MATCH_OUTSIDE);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/application"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/../outside"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/%2e%2e/outside"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app\\..\\outside"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/.\t./outside"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "http://example.com/app/"), ==, WIG_PWA_SCOPE_MATCH_OUTSIDE);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://other.example/app/"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);
}

static void test_literal_prefix_scope(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_navigation_policy_new("https://example.com/app", NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/apple"), ==,
                  WIG_PWA_SCOPE_MATCH_PRIMARY);
}

static void test_extension_scope(void)
{
  const char *extensions[] = { "https://support.example.com/help/", NULL };
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_navigation_policy_new("https://example.com/app/", extensions,
                                                                           &error);
  g_assert_no_error(error);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://support.example.com/help/article"), ==,
                  WIG_PWA_SCOPE_MATCH_EXTENSION);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://support.example.com/other"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);
}

static void test_encoded_path_scope(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_navigation_policy_new("https://example.com/app/%41/", NULL,
                                                                           &error);
  g_assert_no_error(error);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/%41/page"), ==,
                  WIG_PWA_SCOPE_MATCH_PRIMARY);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/A/page"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);

  g_clear_object(&policy);
  policy = wig_pwa_navigation_policy_new("https://example.com/app/%5B/", NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/[/page"), ==,
                  WIG_PWA_SCOPE_MATCH_OUTSIDE);
}

static void test_url_standard_origin(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_navigation_policy_new("https://127.0.0.1/app/", NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://127.1/app/page"), ==, WIG_PWA_SCOPE_MATCH_PRIMARY);
}

static void test_url_standard_path_encoding(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_navigation_policy_new("https://example.com/app/%C3%A9/", NULL,
                                                                           &error);
  g_assert_no_error(error);
  g_assert_cmpint(wig_pwa_navigation_policy_match(policy, "https://example.com/app/é/page"), ==,
                  WIG_PWA_SCOPE_MATCH_PRIMARY);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/pwa/navigation-policy/primary", test_primary_scope);
  g_test_add_func("/pwa/navigation-policy/literal-prefix", test_literal_prefix_scope);
  g_test_add_func("/pwa/navigation-policy/extension", test_extension_scope);
  g_test_add_func("/pwa/navigation-policy/encoded-path", test_encoded_path_scope);
  g_test_add_func("/pwa/navigation-policy/url-standard-origin", test_url_standard_origin);
  g_test_add_func("/pwa/navigation-policy/url-standard-path-encoding", test_url_standard_path_encoding);
  return g_test_run();
}
