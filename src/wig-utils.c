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

#include <libpsl.h>

// https://en.wikipedia.org/wiki/Special-use_domain_name
static const char *const special_use_tlds[] = {
  "test", "local", "localhost", "invalid", "example", "arpa", "onion", "alt", "internal",
};

static const char *const valid_schemes[] = {
  "https", "http", "file", "about", "webkit",
};

static gboolean array_contains(const char *const *array, gsize length, const char *value)
{
  for (gsize i = 0; i < length; i++) {
    if (strcmp(array[i], value) == 0)
      return TRUE;
  }
  return FALSE;
}

static psl_ctx_t *get_psl_context(void)
{
  static psl_ctx_t *psl = NULL;

  if (!psl)
    psl = psl_latest(NULL);

  return psl;
}

/**
 * wig_util_complete_uri:
 *
 * This takes an incomplete URL fragment, that one would type into an entry,
 * and transforms it into a useful URL similar to other browsers.
 *
 * If it's a special-use/local domain or an IP, we add http.
 * If it's a valid domain, we add https.
 * Otherwise use a search engine on the random string.
 */
char *wig_util_complete_uri(const char *url)
{
  const char *scheme = g_uri_peek_scheme(url);
  if (scheme && array_contains(valid_schemes, G_N_ELEMENTS(valid_schemes), scheme))
    return g_strdup(url);

  const char *host_end = url + strcspn(url, "/:");
  g_autofree char *hostname = g_strndup(url, (gsize)(host_end - url));

  if (!strcmp(hostname, "localhost") || g_hostname_is_ip_address(hostname))
    return g_strconcat("http://", url, NULL);

  psl_ctx_t *psl = get_psl_context();
  const char *domain = psl_registrable_domain(psl, hostname);
  if (domain) {
    // Extract the public suffix from the registrable domain
    const char *dot = strchr(domain, '.');
    const char *suffix = dot ? dot + 1 : domain;

    if (dot && array_contains(special_use_tlds, G_N_ELEMENTS(special_use_tlds), suffix))
      return g_strconcat("http://", url, NULL);

    if (psl_is_public_suffix2(psl, suffix, PSL_TYPE_ANY | PSL_TYPE_NO_STAR_RULE))
      return g_strconcat("https://", url, NULL);
  }

  g_autofree char *escaped = g_uri_escape_string(url, NULL, FALSE);
  return g_strconcat("https://duckduckgo.com/?q=", escaped, NULL);
}
