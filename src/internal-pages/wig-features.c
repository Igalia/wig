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

#include "wig-features.h"
#include "wig-internal-page.h"

#define WIG_FEATURE_OVERRIDES_KEY "feature-overrides"

static WebKitFeature *find_feature(WebKitFeatureList *features, const char *identifier)
{
  for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
    WebKitFeature *feature = webkit_feature_list_get(features, i);
    if (g_str_equal(webkit_feature_get_identifier(feature), identifier))
      return feature;
  }
  return NULL;
}

static void apply_feature_mode(WebKitSettings *web_settings, GSettings *settings, WebKitFeature *feature,
                               const char *mode)
{
  gboolean permanent = g_str_equal(mode, "permanent");
  if (!permanent && !g_str_equal(mode, "default") && !g_str_equal(mode, "session")) {
    g_warning("features: unknown mode '%s'", mode);
    return;
  }

  const char *identifier = webkit_feature_get_identifier(feature);
  gboolean enabled = g_str_equal(mode, "default") ? webkit_feature_get_default_value(feature)
                                                  : !webkit_feature_get_default_value(feature);

  g_debug("features: %s %s (%s)", identifier, enabled ? "enabled" : "disabled", mode);
  webkit_settings_set_feature_enabled(web_settings, feature, enabled);

  /* A dictionary can only be written whole, so the other overrides are carried
   * over and this feature is left out unless it is being kept for good. */
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sb}"));

  g_autoptr(GVariant) overrides = g_settings_get_value(settings, WIG_FEATURE_OVERRIDES_KEY);
  GVariantIter iter;
  const char *key;
  gboolean value;
  g_variant_iter_init(&iter, overrides);
  while (g_variant_iter_next(&iter, "{&sb}", &key, &value)) {
    if (!g_str_equal(key, identifier))
      g_variant_builder_add(&builder, "{sb}", key, value);
  }

  if (permanent)
    g_variant_builder_add(&builder, "{sb}", identifier, enabled);

  g_settings_set_value(settings, WIG_FEATURE_OVERRIDES_KEY, g_variant_builder_end(&builder));
}

void wig_features_apply_overrides(WebKitSettings *web_settings, GSettings *settings)
{
  g_autoptr(GVariant) overrides = g_settings_get_value(settings, WIG_FEATURE_OVERRIDES_KEY);
  g_autoptr(WebKitFeatureList) experimental = webkit_settings_get_experimental_features();
  g_autoptr(WebKitFeatureList) development = webkit_settings_get_development_features();

  GVariantIter iter;
  const char *identifier;
  gboolean enabled;
  g_variant_iter_init(&iter, overrides);
  while (g_variant_iter_next(&iter, "{&sb}", &identifier, &enabled)) {
    WebKitFeature *feature = find_feature(experimental, identifier);
    if (!feature)
      feature = find_feature(development, identifier);

    /* Features come and go between WebKit versions, so an override can outlive
     * the feature it names. */
    if (!feature) {
      g_debug("features: no feature named '%s' to override", identifier);
      continue;
    }

    g_debug("features: %s %s (permanent)", identifier, enabled ? "enabled" : "disabled");
    webkit_settings_set_feature_enabled(web_settings, feature, enabled);
  }
}

static const char *feature_status_string(WebKitFeatureStatus status)
{
  switch (status) {
  case WEBKIT_FEATURE_STATUS_EMBEDDER:
    return "embedder";
  case WEBKIT_FEATURE_STATUS_UNSTABLE:
    return "unstable";
  case WEBKIT_FEATURE_STATUS_INTERNAL:
    return "internal";
  case WEBKIT_FEATURE_STATUS_DEVELOPER:
    return "developer";
  case WEBKIT_FEATURE_STATUS_TESTABLE:
    return "testable";
  case WEBKIT_FEATURE_STATUS_PREVIEW:
    return "preview";
  case WEBKIT_FEATURE_STATUS_STABLE:
    return "stable";
  case WEBKIT_FEATURE_STATUS_MATURE:
    return "mature";
  default:
    return "unknown";
  }
}

TmplScope *handle_features_uri(WebKitURISchemeRequest *request, WebKitSettings *web_settings, GSettings *settings,
                               gboolean developer)
{
  g_autoptr(WebKitFeatureList) features = developer ? webkit_settings_get_development_features()
                                                    : webkit_settings_get_experimental_features();

  const char *full_uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(full_uri, G_URI_FLAGS_NONE, NULL);
  const char *query = parsed ? g_uri_get_query(parsed) : NULL;

  if (query) {
    g_autoptr(GHashTable) params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
    if (params) {
      const char *identifier = g_hash_table_lookup(params, "feature");
      const char *mode = g_hash_table_lookup(params, "mode");
      if (identifier && mode) {
        WebKitFeature *feature = find_feature(features, identifier);
        if (feature)
          apply_feature_mode(web_settings, settings, feature, mode);
        else
          g_warning("features: no feature named '%s'", identifier);
      }
    }
  }

  g_autoptr(GVariant) overrides = g_settings_get_value(settings, WIG_FEATURE_OVERRIDES_KEY);

  /* Group features by category. */
  g_autoptr(GHashTable) cat_features = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                             (GDestroyNotify)g_ptr_array_unref);
  g_autoptr(GPtrArray) cat_order = g_ptr_array_new_with_free_func(g_free);

  for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
    WebKitFeature *feature = webkit_feature_list_get(features, i);
    const char *cat_name = webkit_feature_get_category(feature);
    if (!cat_name || !*cat_name)
      cat_name = "Other";

    GPtrArray *feat_list = g_hash_table_lookup(cat_features, cat_name);
    if (!feat_list) {
      feat_list = g_ptr_array_new();
      g_hash_table_insert(cat_features, g_strdup(cat_name), feat_list);
      g_ptr_array_add(cat_order, g_strdup(cat_name));
    }
    g_ptr_array_add(feat_list, feature);
  }

  GVariantBuilder cats_builder;
  g_variant_builder_init(&cats_builder, G_VARIANT_TYPE("aa{sv}"));

  for (guint i = 0; i < cat_order->len; i++) {
    const char *cat_name = cat_order->pdata[i];
    GPtrArray *feat_list = g_hash_table_lookup(cat_features, cat_name);

    GVariantBuilder feats_builder;
    g_variant_builder_init(&feats_builder, G_VARIANT_TYPE("aa{sv}"));

    for (guint j = 0; j < feat_list->len; j++) {
      WebKitFeature *feature = feat_list->pdata[j];
      GVariantBuilder feat_builder;
      g_variant_builder_init(&feat_builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&feat_builder, "{sv}", "identifier",
                            g_variant_new_string(webkit_feature_get_identifier(feature)));
      g_variant_builder_add(&feat_builder, "{sv}", "name",
                            g_variant_new_take_string(wig_internal_page_html_escape(webkit_feature_get_name(feature))));
      g_variant_builder_add(&feat_builder, "{sv}", "status",
                            g_variant_new_string(feature_status_string(webkit_feature_get_status(feature))));
      g_variant_builder_add(
          &feat_builder, "{sv}", "details",
          g_variant_new_take_string(wig_internal_page_html_escape(webkit_feature_get_details(feature))));
      gboolean default_value = webkit_feature_get_default_value(feature);
      gboolean stored_value;
      gboolean permanent = g_variant_lookup(overrides, webkit_feature_get_identifier(feature), "b", &stored_value);
      gboolean is_default = !permanent && webkit_settings_get_feature_enabled(web_settings, feature) == default_value;

      g_variant_builder_add(&feat_builder, "{sv}", "default_label",
                            g_variant_new_string(default_value ? "enabled" : "disabled"));
      g_variant_builder_add(&feat_builder, "{sv}", "override_label",
                            g_variant_new_string(default_value ? "Disabled" : "Enabled"));
      g_variant_builder_add(&feat_builder, "{sv}", "mode_default", g_variant_new_boolean(is_default));
      g_variant_builder_add(&feat_builder, "{sv}", "mode_session", g_variant_new_boolean(!is_default && !permanent));
      g_variant_builder_add(&feat_builder, "{sv}", "mode_permanent", g_variant_new_boolean(permanent));
      g_variant_builder_add_value(&feats_builder, g_variant_builder_end(&feat_builder));
    }

    GVariantBuilder cat_builder;
    g_variant_builder_init(&cat_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&cat_builder, "{sv}", "name",
                          g_variant_new_take_string(wig_internal_page_html_escape(cat_name)));
    g_variant_builder_add(&cat_builder, "{sv}", "features", g_variant_builder_end(&feats_builder));
    g_variant_builder_add_value(&cats_builder, g_variant_builder_end(&cat_builder));
  }

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_string(scope, "title", developer ? "Developer Features" : "Experimental Features");
  tmpl_scope_set_variant(scope, "categories", g_variant_builder_end(&cats_builder));

  return g_steal_pointer(&scope);
}
