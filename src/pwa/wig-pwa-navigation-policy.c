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

#include "wig-pwa-util.h"

#include <wpe/webkit.h>

typedef struct {
  char *path;
  char *origin;
} Scope;

struct _WigPwaNavigationPolicy {
  GObject parent;

  Scope *primary_scope;
  GPtrArray *extension_scopes;
};

G_DEFINE_FINAL_TYPE(WigPwaNavigationPolicy, wig_pwa_navigation_policy, G_TYPE_OBJECT)

static void scope_free(Scope *scope)
{
  g_clear_pointer(&scope->path, g_free);
  g_clear_pointer(&scope->origin, g_free);
  g_free(scope);
}

static char *encoded_path_from_url(const char *url)
{
  const char *authority = strstr(url, "://");
  if (!authority)
    return g_strdup("/");

  const char *path = strpbrk(authority + 3, "/?#");
  if (!path || *path != '/')
    return g_strdup("/");
  const char *end = strpbrk(path, "?#");
  return end ? g_strndup(path, (gsize)(end - path)) : g_strdup(path);
}

static char *web_url_from_input(const char *url)
{
  g_autoptr(GString) normalized = g_string_sized_new(strlen(url));
  for (const char *p = url; *p; p++) {
    if (*p == '\t' || *p == '\n' || *p == '\r')
      continue;
    g_string_append_c(normalized, *p == '\\' ? '/' : *p);
  }
  return g_string_free(g_steal_pointer(&normalized), FALSE);
}

static char *normalize_path(const char *path)
{
  if (!path || !*path)
    return g_strdup("/");

  g_autofree char *web_path = g_strdup(path);
  g_strdelimit(web_path, "\\", '/');
  g_autofree char *encoded_path = g_uri_escape_string(web_path, "/%:@!$&'()*+,;=-._~[]|", FALSE);
  return wig_pwa_path_remove_dot_segments(encoded_path);
}

static Scope *scope_new(const char *url, GError **error)
{
  g_autofree char *web_url = web_url_from_input(url);
  g_autoptr(GUri) uri = g_uri_parse(
      web_url, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_SCHEME_NORMALIZE | G_URI_FLAGS_ENCODED_PATH, error);
  if (!uri)
    return NULL;

  const char *scheme = g_uri_get_scheme(uri);
  if (!scheme || (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0)
      || !g_uri_get_host(uri)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid web app scope '%s'", url);
    return NULL;
  }

  Scope *scope = g_new0(Scope, 1);
  g_autofree char *encoded_path = encoded_path_from_url(web_url);
  scope->path = normalize_path(encoded_path);
  g_autoptr(WebKitSecurityOrigin) origin = webkit_security_origin_new_for_uri(web_url);
  scope->origin = webkit_security_origin_to_string(origin);
  return scope;
}

static gboolean scope_matches(Scope *scope, const char *origin, const char *path)
{
  return g_strcmp0(scope->origin, origin) == 0 && g_str_has_prefix(path, scope->path);
}

static void wig_pwa_navigation_policy_dispose(GObject *object)
{
  WigPwaNavigationPolicy *self = WIG_PWA_NAVIGATION_POLICY(object);
  g_clear_pointer(&self->extension_scopes, g_ptr_array_unref);
  G_OBJECT_CLASS(wig_pwa_navigation_policy_parent_class)->dispose(object);
}

static void wig_pwa_navigation_policy_finalize(GObject *object)
{
  WigPwaNavigationPolicy *self = WIG_PWA_NAVIGATION_POLICY(object);
  g_clear_pointer(&self->primary_scope, scope_free);
  G_OBJECT_CLASS(wig_pwa_navigation_policy_parent_class)->finalize(object);
}

static void wig_pwa_navigation_policy_class_init(WigPwaNavigationPolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_pwa_navigation_policy_dispose;
  object_class->finalize = wig_pwa_navigation_policy_finalize;
}

static void wig_pwa_navigation_policy_init(WigPwaNavigationPolicy *self)
{
  self->extension_scopes = g_ptr_array_new_with_free_func((GDestroyNotify)scope_free);
}

WigPwaNavigationPolicy *wig_pwa_navigation_policy_new(const char *primary_scope, const char *const *extension_scopes,
                                                      GError **error)
{
  g_return_val_if_fail(primary_scope != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autoptr(WigPwaNavigationPolicy) self = g_object_new(WIG_TYPE_PWA_NAVIGATION_POLICY, NULL);
  self->primary_scope = scope_new(primary_scope, error);
  if (!self->primary_scope)
    return NULL;

  for (guint i = 0; extension_scopes && extension_scopes[i]; i++) {
    Scope *scope = scope_new(extension_scopes[i], error);
    if (!scope)
      return NULL;
    g_ptr_array_add(self->extension_scopes, scope);
  }
  return g_steal_pointer(&self);
}

WigPwaScopeMatch wig_pwa_navigation_policy_match(WigPwaNavigationPolicy *self, const char *uri)
{
  g_return_val_if_fail(WIG_IS_PWA_NAVIGATION_POLICY(self), WIG_PWA_SCOPE_MATCH_OUTSIDE);
  if (!uri)
    return WIG_PWA_SCOPE_MATCH_OUTSIDE;

  g_autofree char *web_uri = web_url_from_input(uri);
  g_autoptr(GError) error = NULL;
  g_autoptr(GUri) parsed = g_uri_parse(
      web_uri, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_SCHEME_NORMALIZE | G_URI_FLAGS_ENCODED_PATH, &error);
  if (!parsed || !g_uri_get_scheme(parsed) || !g_uri_get_host(parsed))
    return WIG_PWA_SCOPE_MATCH_OUTSIDE;

  g_autoptr(WebKitSecurityOrigin) security_origin = webkit_security_origin_new_for_uri(web_uri);
  g_autofree char *origin = webkit_security_origin_to_string(security_origin);
  g_autofree char *encoded_path = encoded_path_from_url(web_uri);
  g_autofree char *path = normalize_path(encoded_path);
  if (scope_matches(self->primary_scope, origin, path))
    return WIG_PWA_SCOPE_MATCH_PRIMARY;

  for (guint i = 0; i < self->extension_scopes->len; i++) {
    if (scope_matches(g_ptr_array_index(self->extension_scopes, i), origin, path))
      return WIG_PWA_SCOPE_MATCH_EXTENSION;
  }
  return WIG_PWA_SCOPE_MATCH_OUTSIDE;
}
