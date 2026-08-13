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

#include "wig-settings-page.h"

gboolean uri_is_settings_page(const char *uri)
{
  if (!uri)
    return FALSE;

  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  return parsed && g_strcmp0(g_uri_get_scheme(parsed), "wig") == 0
      && g_strcmp0(g_uri_get_path(parsed), "settings") == 0;
}

static void handle_settings_change(WebKitURISchemeRequest *request, GSettings *settings)
{
  const char *uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED_QUERY, NULL);
  const char *query = parsed ? g_uri_get_query(parsed) : NULL;
  if (!query)
    return;

  g_autoptr(GError) error = NULL;
  g_autoptr(GHashTable) params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, &error);
  if (!params) {
    g_warning("settings: invalid query: %s", error->message);
    return;
  }

  const char *key = g_hash_table_lookup(params, "key");
  const char *value = g_hash_table_lookup(params, "value");
  if (!key || !value)
    return;

  gboolean changed = FALSE;
  if (g_str_equal(key, "restore-tabs")) {
    if (g_str_equal(value, "true"))
      changed = g_settings_set_boolean(settings, key, TRUE);
    else if (g_str_equal(value, "false"))
      changed = g_settings_set_boolean(settings, key, FALSE);
    else {
      g_warning("settings: invalid value '%s' for '%s'", value, key);
      return;
    }
  } else if (g_str_equal(key, "tab-layout")) {
    if (g_str_equal(value, "horizontal") || g_str_equal(value, "vertical"))
      changed = g_settings_set_string(settings, key, value);
    else {
      g_warning("settings: invalid value '%s' for '%s'", value, key);
      return;
    }
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  } else if (g_str_equal(key, "https-navigation-policy")) {
    if (g_str_equal(value, "keep-as-requested") || g_str_equal(value, "https-first")
        || g_str_equal(value, "https-only"))
      changed = g_settings_set_string(settings, key, value);
    else {
      g_warning("settings: invalid value '%s' for '%s'", value, key);
      return;
    }
#endif
  } else if (g_str_equal(key, "search-engine")) {
    if (!strstr(value, "%s")) {
      g_warning("settings: invalid value '%s' for '%s'", value, key);
      return;
    }
    changed = g_settings_set_string(settings, key, value);
  } else {
    g_warning("settings: unknown key '%s'", key);
    return;
  }

  if (!changed)
    g_warning("settings: failed to set '%s'", key);
}

TmplScope *handle_settings_uri(WebKitURISchemeRequest *request, GSettings *settings)
{
  handle_settings_change(request, settings);

  g_autofree char *tab_layout = g_settings_get_string(settings, "tab-layout");
  g_autofree char *search_engine = g_settings_get_string(settings, "search-engine");
  g_autofree char *escaped_search_engine = g_markup_escape_text(search_engine, -1);
  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_boolean(scope, "restore_tabs", g_settings_get_boolean(settings, "restore-tabs"));
  tmpl_scope_set_boolean(scope, "tab_layout_horizontal", g_str_equal(tab_layout, "horizontal"));
  tmpl_scope_set_boolean(scope, "tab_layout_vertical", g_str_equal(tab_layout, "vertical"));
  tmpl_scope_set_string(scope, "search_engine", escaped_search_engine);
  tmpl_scope_set_boolean(scope, "https_navigation_supported", HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT);
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  g_autofree char *https_policy = g_settings_get_string(settings, "https-navigation-policy");
  tmpl_scope_set_boolean(scope, "https_keep_as_requested", g_str_equal(https_policy, "keep-as-requested"));
  tmpl_scope_set_boolean(scope, "https_first", g_str_equal(https_policy, "https-first"));
  tmpl_scope_set_boolean(scope, "https_only", g_str_equal(https_policy, "https-only"));
#else
  tmpl_scope_set_boolean(scope, "https_keep_as_requested", TRUE);
  tmpl_scope_set_boolean(scope, "https_first", FALSE);
  tmpl_scope_set_boolean(scope, "https_only", FALSE);
#endif
  return g_steal_pointer(&scope);
}

static void update_settings_page_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(source), result,
                                                                                    &error);
  if (!value)
    g_warning("settings: failed to update page: %s", error->message);
}

void update_settings_page(WebKitWebView *web_view, GSettings *settings)
{
  g_autofree char *tab_layout = g_settings_get_string(settings, "tab-layout");
  g_autofree char *search_engine = g_settings_get_string(settings, "search-engine");
  GVariantBuilder arguments;
  g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&arguments, "{sv}", "restoreTabs",
                        g_variant_new_int32(g_settings_get_boolean(settings, "restore-tabs")));
  g_variant_builder_add(&arguments, "{sv}", "tabLayout", g_variant_new_string(tab_layout));
  g_variant_builder_add(&arguments, "{sv}", "searchEngine", g_variant_new_string(search_engine));
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  g_autofree char *https_policy = g_settings_get_string(settings, "https-navigation-policy");
#else
  g_autofree char *https_policy = g_strdup("keep-as-requested");
#endif
  g_variant_builder_add(&arguments, "{sv}", "httpsNavigationPolicy", g_variant_new_string(https_policy));
  g_autoptr(GVariant) args = g_variant_ref_sink(g_variant_builder_end(&arguments));

  static const char function[] = "if (!window.wigSettings) return false;"
                                 "window.wigSettings.update(restoreTabs, tabLayout, searchEngine,"
                                 "                          httpsNavigationPolicy);"
                                 "return true;";
  webkit_web_view_call_async_javascript_function(web_view, function, -1, args, NULL, WIG_SETTINGS_PAGE_URI, NULL,
                                                 update_settings_page_finished, NULL);
}
