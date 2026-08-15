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
#include <wpe/webkit.h>

// https://en.wikipedia.org/wiki/Special-use_domain_name
static const char *const special_use_tlds[] = {
  "test", "local", "localhost", "invalid", "example", "arpa", "onion", "alt", "internal",
};

static const char *const valid_schemes[] = {
  "https", "http", "file", "about", "webkit", "wig",
};

static gboolean array_contains(const char *const *array, gsize length, const char *value)
{
  for (gsize i = 0; i < length; i++) {
    if (g_strcmp0(array[i], value) == 0)
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
 * wig_util_get_uri_completion_type:
 *
 * Returns how an entry string should be completed before loading.
 */
WigUtilUriCompletionType wig_util_get_uri_completion_type(const char *url)
{
  if (!url || !*url)
    return WIG_UTIL_URI_COMPLETION_SEARCH;

  const char *scheme = g_uri_peek_scheme(url);
  if (scheme && array_contains(valid_schemes, G_N_ELEMENTS(valid_schemes), scheme))
    return WIG_UTIL_URI_COMPLETION_PASSTHROUGH;

  const char *host_end = url + strcspn(url, "/:");
  g_autofree char *hostname = g_strndup(url, (gsize)(host_end - url));

  if (g_strcmp0(hostname, "localhost") == 0 || g_hostname_is_ip_address(hostname))
    return WIG_UTIL_URI_COMPLETION_HTTP;

  psl_ctx_t *psl = get_psl_context();
  const char *domain = psl_registrable_domain(psl, hostname);
  if (!domain)
    return WIG_UTIL_URI_COMPLETION_SEARCH;

  const char *dot = strchr(domain, '.');
  const char *suffix = dot ? dot + 1 : domain;

  if (dot && array_contains(special_use_tlds, G_N_ELEMENTS(special_use_tlds), suffix))
    return WIG_UTIL_URI_COMPLETION_HTTP;

  if (psl_is_public_suffix2(psl, suffix, PSL_TYPE_ANY | PSL_TYPE_NO_STAR_RULE))
    return WIG_UTIL_URI_COMPLETION_HTTPS;

  return WIG_UTIL_URI_COMPLETION_SEARCH;
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
char *wig_util_complete_uri(const char *url, const char *search_engine)
{
  switch (wig_util_get_uri_completion_type(url)) {
  case WIG_UTIL_URI_COMPLETION_PASSTHROUGH: {
    const char *scheme = g_uri_peek_scheme(url);

    // We can't override about so just rewrite them.
    if (g_strcmp0(scheme, "about") == 0 && g_strcmp0(url, "about:blank") != 0)
      return g_strconcat("wig:", url + strlen("about:"), NULL);

    return g_strdup(url);
  }

  case WIG_UTIL_URI_COMPLETION_HTTPS:
    return g_strconcat("https://", url, NULL);

  case WIG_UTIL_URI_COMPLETION_HTTP:
    return g_strconcat("http://", url, NULL);

  case WIG_UTIL_URI_COMPLETION_SEARCH:
    return wig_util_search_uri(url ? url : "", search_engine);
  }

  g_assert_not_reached();
}

char *wig_util_search_uri(const char *terms, const char *search_engine)
{
  g_autofree char *escaped = g_uri_escape_string(terms, NULL, FALSE);
  g_autoptr(GString) search_string = g_string_new(search_engine);
  if (g_string_replace(search_string, "%s", escaped, 0) != 1) {
    g_warning("Invalid search engine template, must contain one %%s");
    return g_strconcat("https://duckduckgo.com/?q=", escaped, NULL);
  }
  return g_string_free(g_steal_pointer(&search_string), FALSE);
}
// FIXME: We should have a real name along with the URL for the search engine setting.
char *wig_util_search_engine_name(const char *search_engine)
{
  g_autoptr(GUri) uri = g_uri_parse(search_engine, G_URI_FLAGS_PARSE_RELAXED, NULL);
  const char *host = uri ? g_uri_get_host(uri) : NULL;
  if (!host)
    return NULL;

  if (g_str_has_prefix(host, "www."))
    host += strlen("www.");
  return g_strdup(host);
}

#if HAVE_FAVICON_SUPPORT
GIcon *wig_util_best_page_icon(WebKitImageList *icons, int min_size)
{
  if (!icons)
    return NULL;

  WebKitImage *best = NULL;
  WebKitImage *largest = NULL;
  int best_width = 0;
  int largest_width = -1;

  gsize n = webkit_image_list_get_length(icons);
  for (gsize i = 0; i < n; i++) {
    WebKitImage *image = webkit_image_list_get(icons, i);
    int width = webkit_image_get_width(image);
    if (width > largest_width) {
      largest = image;
      largest_width = width;
    }
    if (width >= min_size && (!best || width < best_width)) {
      best = image;
      best_width = width;
    }
  }

  return G_ICON(best ? best : largest);
}
#endif
