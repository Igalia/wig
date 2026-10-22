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

#include "wig-utils.h"

#define SEARCH_ENGINE "https://duckduckgo.com/?q=%s"

static void assert_completes_to(char *(*complete)(const char *, const char *), const char *typed, const char *expected)
{
  g_autofree char *uri = complete(typed, SEARCH_ENGINE);

  g_test_message("'%s' -> '%s'", typed, uri);
  g_assert_cmpstr(uri, ==, expected);
}

static void test_complete_sites(void)
{
  assert_completes_to(wig_util_complete_uri, "domain.com", "https://domain.com");
  assert_completes_to(wig_util_complete_uri, "domain.com/path", "https://domain.com/path");
  assert_completes_to(wig_util_complete_uri, "domain.com?q=1", "https://domain.com?q=1");
  assert_completes_to(wig_util_complete_uri, "domain.com#top", "https://domain.com#top");
  assert_completes_to(wig_util_complete_uri, "Example.COM", "https://Example.COM");
  assert_completes_to(wig_util_complete_uri, "bücher.de", "https://bücher.de");
}

static void test_complete_public_suffixes(void)
{
  assert_completes_to(wig_util_complete_uri, "github.io", "https://github.io");
  assert_completes_to(wig_util_complete_uri, "blogspot.com", "https://blogspot.com");
  // The suffixes registries hand names out under have no site of their own.
  assert_completes_to(wig_util_complete_uri, "co.uk", "https://duckduckgo.com/?q=co.uk");
  assert_completes_to(wig_util_complete_uri, "gov.uk", "https://duckduckgo.com/?q=gov.uk");
  assert_completes_to(wig_util_complete_uri, "com.au", "https://duckduckgo.com/?q=com.au");
  assert_completes_to(wig_util_complete_uri, "or.jp", "https://duckduckgo.com/?q=or.jp");
}

static void test_complete_local(void)
{
  assert_completes_to(wig_util_complete_uri, "localhost", "http://localhost");
  assert_completes_to(wig_util_complete_uri, "localhost:8080", "http://localhost:8080");
  assert_completes_to(wig_util_complete_uri, "192.168.1.1", "http://192.168.1.1");
  assert_completes_to(wig_util_complete_uri, "foo.test", "http://foo.test");
}

static void test_complete_passthrough(void)
{
  assert_completes_to(wig_util_complete_uri, "https://domain", "https://domain");
  assert_completes_to(wig_util_complete_uri, "file:///tmp", "file:///tmp");
  assert_completes_to(wig_util_complete_uri, "about:blank", "about:blank");
  assert_completes_to(wig_util_complete_uri, "about:config", "wig:config");
}

static void test_complete_search(void)
{
  assert_completes_to(wig_util_complete_uri, "domain", "https://duckduckgo.com/?q=domain");
  assert_completes_to(wig_util_complete_uri, "com", "https://duckduckgo.com/?q=com");
  assert_completes_to(wig_util_complete_uri, "foo bar", "https://duckduckgo.com/?q=foo%20bar");
  assert_completes_to(wig_util_complete_uri, "is example.com down?",
                      "https://duckduckgo.com/?q=is%20example.com%20down%3F");
  assert_completes_to(wig_util_complete_uri, "node.js", "https://duckduckgo.com/?q=node.js");
  assert_completes_to(wig_util_complete_uri, "user:pass@evil.com", "https://duckduckgo.com/?q=user%3Apass%40evil.com");
}

static void test_dot_com_adds_tld(void)
{
  assert_completes_to(wig_util_complete_uri_dot_com, "domain", "https://www.domain.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "my.intranet", "https://www.my.intranet.com");
  // A last label that is not a TLD is part of the name, as in Chromium.
  assert_completes_to(wig_util_complete_uri_dot_com, "node.js", "https://www.node.js.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "x.y", "https://www.x.y.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "domain?q=1", "https://www.domain.com?q=1");
  assert_completes_to(wig_util_complete_uri_dot_com, "domain:8080/x", "https://www.domain.com:8080/x");
  assert_completes_to(wig_util_complete_uri_dot_com, "Domain", "https://www.domain.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "app", "https://www.app.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "3", "https://www.3.com");
}

static void test_dot_com_internationalized(void)
{
  assert_completes_to(wig_util_complete_uri_dot_com, "bücher", "https://www.bücher.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "café", "https://www.café.com");
}

static void test_dot_com_www(void)
{
  assert_completes_to(wig_util_complete_uri_dot_com, "www.domain", "https://www.domain.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "www", "https://www.com");
}

static void test_dot_com_keeps_tld(void)
{
  assert_completes_to(wig_util_complete_uri_dot_com, "domain.com", "https://domain.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "domain.com?q=1", "https://domain.com?q=1");
  assert_completes_to(wig_util_complete_uri_dot_com, "https://domain.com#top", "https://domain.com#top");
  assert_completes_to(wig_util_complete_uri_dot_com, "Example.COM", "https://Example.COM");
  assert_completes_to(wig_util_complete_uri_dot_com, "github.io", "https://github.io");
  assert_completes_to(wig_util_complete_uri_dot_com, "blogspot.com", "https://blogspot.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "co.uk", "https://duckduckgo.com/?q=co.uk");
  assert_completes_to(wig_util_complete_uri_dot_com, "foo.test", "http://foo.test");
}

static void test_dot_com_keeps_scheme(void)
{
  assert_completes_to(wig_util_complete_uri_dot_com, "https://domain", "https://domain");
  assert_completes_to(wig_util_complete_uri_dot_com, "http://domain", "http://domain");
  assert_completes_to(wig_util_complete_uri_dot_com, "https://intranet/path", "https://intranet/path");
  assert_completes_to(wig_util_complete_uri_dot_com, "http://router/admin", "http://router/admin");
}

static void test_dot_com_keeps_host(void)
{
  // What follows a colon is a port only when it is a number; anything else,
  // turned into a URL, would load the host after the "@".
  assert_completes_to(wig_util_complete_uri_dot_com, "user:pass@evil.com",
                      "https://duckduckgo.com/?q=user%3Apass%40evil.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "mailto:foo@bar.com",
                      "https://duckduckgo.com/?q=mailto%3Afoo%40bar.com");
  assert_completes_to(wig_util_complete_uri_dot_com, "foo:bar", "https://duckduckgo.com/?q=foo%3Abar");
  assert_completes_to(wig_util_complete_uri_dot_com, "foo:", "https://duckduckgo.com/?q=foo%3A");
  assert_completes_to(wig_util_complete_uri_dot_com, "foo:8080x", "https://duckduckgo.com/?q=foo%3A8080x");
  assert_completes_to(wig_util_complete_uri_dot_com, "foo:8080", "https://www.foo.com:8080");
  assert_completes_to(wig_util_complete_uri_dot_com, "foo:8080#top", "https://www.foo.com:8080#top");
}

static void test_dot_com_leaves_others(void)
{
  assert_completes_to(wig_util_complete_uri_dot_com, "localhost", "http://localhost");
  assert_completes_to(wig_util_complete_uri_dot_com, "localhost:8080", "http://localhost:8080");
  assert_completes_to(wig_util_complete_uri_dot_com, "192.168.1.1", "http://192.168.1.1");
  assert_completes_to(wig_util_complete_uri_dot_com, "192.168.1", "https://duckduckgo.com/?q=192.168.1");
  assert_completes_to(wig_util_complete_uri_dot_com, "file:///tmp", "file:///tmp");
  assert_completes_to(wig_util_complete_uri_dot_com, "about:config", "wig:config");
  assert_completes_to(wig_util_complete_uri_dot_com, "foo bar", "https://duckduckgo.com/?q=foo%20bar");
  assert_completes_to(wig_util_complete_uri_dot_com, "-foo", "https://duckduckgo.com/?q=-foo");
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/utils/complete-uri/sites", test_complete_sites);
  g_test_add_func("/utils/complete-uri/public-suffixes", test_complete_public_suffixes);
  g_test_add_func("/utils/complete-uri/local", test_complete_local);
  g_test_add_func("/utils/complete-uri/passthrough", test_complete_passthrough);
  g_test_add_func("/utils/complete-uri/search", test_complete_search);
  g_test_add_func("/utils/complete-uri-dot-com/adds-tld", test_dot_com_adds_tld);
  g_test_add_func("/utils/complete-uri-dot-com/internationalized", test_dot_com_internationalized);
  g_test_add_func("/utils/complete-uri-dot-com/www", test_dot_com_www);
  g_test_add_func("/utils/complete-uri-dot-com/keeps-tld", test_dot_com_keeps_tld);
  g_test_add_func("/utils/complete-uri-dot-com/keeps-scheme", test_dot_com_keeps_scheme);
  g_test_add_func("/utils/complete-uri-dot-com/keeps-host", test_dot_com_keeps_host);
  g_test_add_func("/utils/complete-uri-dot-com/leaves-others", test_dot_com_leaves_others);
  return g_test_run();
}
