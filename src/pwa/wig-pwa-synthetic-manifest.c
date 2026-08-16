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

/* Parsing keeps whatever encoding the address already has, so re-joining the
 * pieces cannot encode them a second time. */
#define PARSE_FLAGS (G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_SCHEME_NORMALIZE | G_URI_FLAGS_ENCODED)

static GUri *parse_installable_url(const char *page_url, GError **error)
{
  if (!page_url || !*page_url) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "There is no page to install");
    return NULL;
  }

  g_autoptr(GUri) uri = g_uri_parse(page_url, PARSE_FLAGS, error);
  if (!uri)
    return NULL;

  const char *scheme = g_uri_get_scheme(uri);
  const char *host = g_uri_get_host(uri);
  if (!scheme || (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0) || !host
      || !*host) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "'%s' is not a web page that can be installed",
                page_url);
    return NULL;
  }

  return g_steal_pointer(&uri);
}

gboolean wig_pwa_synthetic_manifest_is_possible(const char *page_url)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GUri) uri = parse_installable_url(page_url, &error);

  return uri != NULL;
}

/* A page that serves no manifest still tells us everything a small one needs:
 * where it starts, what it is called, and which site it belongs to. Credentials
 * in the address are left out; a launcher is not the place for them. */
GVariant *wig_pwa_synthetic_manifest_new(const char *page_url, const char *title, GError **error)
{
  g_autoptr(GUri) uri = parse_installable_url(page_url, error);
  if (!uri)
    return NULL;

  const char *scheme = g_uri_get_scheme(uri);
  const char *host = g_uri_get_host(uri);
  int port = g_uri_get_port(uri);

  /* The fragment is a position within the page rather than a page of its own,
   * so the app starts at the address without it. */
  g_autofree char *start_url = g_uri_join(PARSE_FLAGS, scheme, NULL, host, port, g_uri_get_path(uri),
                                          g_uri_get_query(uri), NULL);
  /* Nothing says how much of the site belongs to the app, so the whole origin
   * does; anything narrower would hand ordinary links back to the browser. */
  g_autofree char *scope = g_uri_join(PARSE_FLAGS, scheme, NULL, host, port, "/", NULL, NULL);

  const char *name = title && *title ? title : host;

  GVariantDict manifest;
  g_variant_dict_init(&manifest, NULL);
  g_variant_dict_insert(&manifest, "id", "s", start_url);
  g_variant_dict_insert(&manifest, "name", "s", name);
  g_variant_dict_insert(&manifest, "short_name", "s", host);
  g_variant_dict_insert(&manifest, "start_url", "s", start_url);
  g_variant_dict_insert(&manifest, "scope", "s", scope);
  g_variant_dict_insert(&manifest, "display", "s", "standalone");

  return g_variant_ref_sink(g_variant_dict_end(&manifest));
}
