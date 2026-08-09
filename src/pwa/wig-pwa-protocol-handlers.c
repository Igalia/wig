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

#include "wig-pwa-protocol-handlers.h"

#include "wig-pwa-util.h"

#include <gio/gio.h>
#include <wpe/webkit.h>

static gboolean protocol_is_allowed(const char *protocol)
{
  static const char *const safelisted[] = {
    "bitcoin",     "ftp",  "ftps", "geo", "im",    "irc", "ircs", "magnet", "mailto", "matrix", "mms",  "news", "nntp",
    "openpgp4fpr", "sftp", "sip",  "sms", "smsto", "ssh", "tel",  "urn",    "webcal", "wtai",   "xmpp", NULL,
  };
  if (g_strv_contains(safelisted, protocol))
    return TRUE;
  if (!g_str_has_prefix(protocol, "web+") || !protocol[4])
    return FALSE;
  for (const char *p = protocol + 4; *p; p++) {
    if (!g_ascii_islower(*p))
      return FALSE;
  }
  return TRUE;
}

static char *normalize_handler_template(const char *template)
{
  g_autoptr(GString) normalized = g_string_sized_new(strlen(template));
  gboolean in_query_or_fragment = FALSE;
  for (const char *p = template; *p; p++) {
    if (*p == '\t' || *p == '\n' || *p == '\r')
      continue;
    if (*p == '?' || *p == '#')
      in_query_or_fragment = TRUE;
    g_string_append_c(normalized, *p == '\\' && !in_query_or_fragment ? '/' : *p);
  }
  return g_string_free(g_steal_pointer(&normalized), FALSE);
}

static char *serialize_protocol_input(const char *input_url, const char *scheme)
{
  g_autoptr(GString) cleaned_builder = g_string_sized_new(strlen(input_url));
  for (const char *p = input_url; *p; p++) {
    if (*p != '\t' && *p != '\n' && *p != '\r')
      g_string_append_c(cleaned_builder, *p);
  }
  g_autofree char *cleaned = g_string_free(g_steal_pointer(&cleaned_builder), FALSE);
  const char *colon = strchr(cleaned, ':');
  if (!colon)
    return NULL;

  g_autoptr(GString) serialized = g_string_new(scheme);
  g_string_append_c(serialized, ':');
  g_autofree char *special_remainder = NULL;
  const char *remainder = colon + 1;
  gboolean is_ftp = g_str_equal(scheme, "ftp");
  if (is_ftp) {
    special_remainder = g_strdup(remainder);
    g_strdelimit(special_remainder, "\\", '/');
    remainder = special_remainder;
    while (*remainder == '/')
      remainder++;
  }
  if (is_ftp || g_str_has_prefix(remainder, "//")) {
    g_string_append(serialized, "//");
    const char *authority = is_ftp ? remainder : remainder + 2;
    const char *authority_end = strpbrk(authority, "/?#");
    if (!authority_end)
      authority_end = authority + strlen(authority);
    const char *at = g_strrstr_len(authority, (gssize)(authority_end - authority), "@");
    g_string_append(serialized, at ? at + 1 : authority);
  } else {
    g_string_append(serialized, remainder);
  }
  g_autofree char *value = g_string_free(g_steal_pointer(&serialized), FALSE);
  g_autoptr(GString) encoded = g_string_sized_new(strlen(value));
  for (const guchar *p = (const guchar *)value; *p; p++) {
    if (*p < 0x20 || *p >= 0x7f)
      g_string_append_printf(encoded, "%%%02X", *p);
    else
      g_string_append_c(encoded, (char)*p);
  }
  return g_string_free(g_steal_pointer(&encoded), FALSE);
}

static char *resolve_handler_reference(const char *base_url, const char *reference)
{
  if (g_uri_peek_scheme(reference))
    return g_strdup(reference);

  const char *scheme_end = strstr(base_url, "://");
  if (!scheme_end)
    return NULL;
  if (g_str_has_prefix(reference, "//")) {
    g_autofree char *scheme = g_strndup(base_url, (gsize)(scheme_end - base_url));
    return g_strconcat(scheme, ":", reference, NULL);
  }

  const char *origin_end = strpbrk(scheme_end + 3, "/?#");
  if (!origin_end)
    origin_end = base_url + strlen(base_url);
  g_autofree char *origin = g_strndup(base_url, (gsize)(origin_end - base_url));
  if (reference[0] == '/')
    return g_strconcat(origin, reference, NULL);

  const char *base_end = strpbrk(base_url, "?#");
  g_autofree char *base = base_end ? g_strndup(base_url, (gsize)(base_end - base_url)) : g_strdup(base_url);
  if (reference[0] == '?' || reference[0] == '#')
    return g_strconcat(base, reference, NULL);

  char *last_slash = strrchr(base, '/');
  if (!last_slash || last_slash < scheme_end + 3)
    return g_strconcat(origin, "/", reference, NULL);
  last_slash[1] = '\0';
  return g_strconcat(base, reference, NULL);
}

static char *normalize_handler_url_path(const char *url)
{
  const char *authority = strstr(url, "://");
  if (!authority)
    return NULL;
  const char *path = strpbrk(authority + 3, "/?#");
  if (!path)
    return g_strconcat(url, "/", NULL);
  if (*path != '/') {
    g_autofree char *origin = g_strndup(url, (gsize)(path - url));
    return g_strconcat(origin, "/", path, NULL);
  }
  const char *path_end = strpbrk(path, "?#");
  g_autofree char *raw_path = path_end ? g_strndup(path, (gsize)(path_end - path)) : g_strdup(path);
  g_autofree char *normalized_path = wig_pwa_path_remove_dot_segments(raw_path);

  g_autoptr(GString) result = g_string_new_len(url, (gssize)(path - url));
  g_string_append(result, normalized_path);
  if (path_end)
    g_string_append(result, path_end);
  return g_string_free(g_steal_pointer(&result), FALSE);
}

static gboolean uri_is_potentially_trustworthy(const char *url)
{
  g_autoptr(WebKitSecurityOrigin) security_origin = webkit_security_origin_new_for_uri(url);
  g_autofree char *origin = webkit_security_origin_to_string(security_origin);
  g_autoptr(GError) error = NULL;
  g_autoptr(GUri) uri = g_uri_parse(origin, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_SCHEME_NORMALIZE, &error);
  if (!uri)
    return FALSE;

  const char *scheme = g_uri_get_scheme(uri);
  if (g_ascii_strcasecmp(scheme, "https") == 0)
    return TRUE;
  if (g_ascii_strcasecmp(scheme, "http") != 0)
    return FALSE;

  const char *host = g_uri_get_host(uri);
  g_autofree char *lower_host = g_ascii_strdown(host, -1);
  if (g_str_equal(lower_host, "localhost") || g_str_has_suffix(lower_host, ".localhost"))
    return TRUE;
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
  return address && g_inet_address_get_is_loopback(address);
}

static char *handler_protocol(GVariant *handler)
{
  const char *protocol = wig_pwa_variant_lookup_string(handler, "protocol");
  if (!protocol || !wig_pwa_variant_lookup_string(handler, "url"))
    return NULL;
  g_autofree char *lowercase_protocol = g_ascii_strdown(protocol, -1);
  return protocol_is_allowed(lowercase_protocol) ? g_steal_pointer(&lowercase_protocol) : NULL;
}

static char *resolve_handler_template(GVariant *handler, const char *manifest_url,
                                      WigPwaNavigationPolicy *navigation_policy, char **out_placeholder)
{
  g_autofree char *placeholder = g_uuid_string_random();
  g_strdelimit(placeholder, "-", 'x');
  g_autoptr(GString) with_placeholder = g_string_new_take(
      normalize_handler_template(wig_pwa_variant_lookup_string(handler, "url")));
  if (!g_string_replace(with_placeholder, "%s", placeholder, 1))
    return NULL;

  g_autofree char *raw_resolved = resolve_handler_reference(manifest_url, with_placeholder->str);
  g_autofree char *resolved = raw_resolved ? normalize_handler_url_path(raw_resolved) : NULL;
  if (!resolved)
    return NULL;

  g_autoptr(GError) error = NULL;
  g_autoptr(GUri) uri = g_uri_parse(resolved, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_SCHEME_NORMALIZE, &error);
  const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  if (!scheme || (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0)
      || !g_uri_get_host(uri) || strstr(g_uri_get_host(uri), placeholder) || !uri_is_potentially_trustworthy(resolved)
      || wig_pwa_navigation_policy_match(navigation_policy, resolved) != WIG_PWA_SCOPE_MATCH_PRIMARY)
    return NULL;

  if (out_placeholder)
    *out_placeholder = g_steal_pointer(&placeholder);
  return g_steal_pointer(&resolved);
}

char *wig_pwa_protocol_handlers_resolve(GVariant *manifest, const char *manifest_url,
                                        WigPwaNavigationPolicy *navigation_policy, const char *input_url)
{
  g_return_val_if_fail(manifest && g_variant_is_of_type(manifest, G_VARIANT_TYPE_VARDICT), NULL);
  g_return_val_if_fail(manifest_url != NULL, NULL);
  g_return_val_if_fail(WIG_IS_PWA_NAVIGATION_POLICY(navigation_policy), NULL);
  g_return_val_if_fail(input_url != NULL, NULL);

  g_autoptr(GError) error = NULL;
  g_autoptr(GUri) input = g_uri_parse(input_url,
                                      G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_SCHEME_NORMALIZE
                                          | G_URI_FLAGS_ENCODED_PATH | G_URI_FLAGS_ENCODED_QUERY
                                          | G_URI_FLAGS_ENCODED_FRAGMENT,
                                      &error);
  const char *input_scheme = input ? g_uri_get_scheme(input) : NULL;
  if (!input_scheme)
    return NULL;

  g_autoptr(GVariant) handlers = g_variant_lookup_value(manifest, "protocol_handlers", G_VARIANT_TYPE("av"));
  gsize length = handlers ? g_variant_n_children(handlers) : 0;
  for (gsize i = 0; i < length; i++) {
    g_autoptr(GVariant) handler = wig_pwa_variant_array_get_dict(handlers, i);
    g_autofree char *protocol = handler ? handler_protocol(handler) : NULL;
    if (!protocol || g_ascii_strcasecmp(protocol, input_scheme) != 0)
      continue;

    g_autofree char *placeholder = NULL;
    g_autofree char *handler_template = resolve_handler_template(handler, manifest_url, navigation_policy,
                                                                 &placeholder);
    if (!handler_template)
      continue;

    g_autofree char *serialized_input = serialize_protocol_input(input_url, protocol);
    g_autofree char *encoded_input = g_uri_escape_string(serialized_input, "!'()*", FALSE);
    g_autoptr(GString) result = g_string_new(handler_template);
    if (g_string_replace(result, placeholder, encoded_input, 1)
        && wig_pwa_navigation_policy_match(navigation_policy, result->str) == WIG_PWA_SCOPE_MATCH_PRIMARY)
      return g_string_free(g_steal_pointer(&result), FALSE);
  }
  return NULL;
}

GStrv wig_pwa_protocol_handlers_get_schemes(GVariant *manifest, const char *manifest_url,
                                            WigPwaNavigationPolicy *navigation_policy)
{
  g_return_val_if_fail(manifest && g_variant_is_of_type(manifest, G_VARIANT_TYPE_VARDICT), NULL);
  g_return_val_if_fail(manifest_url != NULL, NULL);
  g_return_val_if_fail(WIG_IS_PWA_NAVIGATION_POLICY(navigation_policy), NULL);

  g_autoptr(GPtrArray) schemes = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GVariant) handlers = g_variant_lookup_value(manifest, "protocol_handlers", G_VARIANT_TYPE("av"));
  gsize length = handlers ? g_variant_n_children(handlers) : 0;
  for (gsize i = 0; i < length; i++) {
    g_autoptr(GVariant) handler = wig_pwa_variant_array_get_dict(handlers, i);
    g_autofree char *protocol = handler ? handler_protocol(handler) : NULL;
    if (!protocol || g_ptr_array_find_with_equal_func(schemes, protocol, (GEqualFunc)g_str_equal, NULL))
      continue;

    g_autofree char *handler_template = resolve_handler_template(handler, manifest_url, navigation_policy, NULL);
    if (handler_template)
      g_ptr_array_add(schemes, g_steal_pointer(&protocol));
  }
  g_ptr_array_add(schemes, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&schemes), FALSE);
}
