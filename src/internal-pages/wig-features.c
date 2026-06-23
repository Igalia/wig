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

TmplScope *handle_features_uri(WebKitURISchemeRequest *request, WebKitSettings *settings, gboolean developer)
{
  g_autoptr(WebKitFeatureList) features = developer ? webkit_settings_get_development_features()
                                                    : webkit_settings_get_experimental_features();

  const char *full_uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(full_uri, G_URI_FLAGS_NONE, NULL);
  const char *query = parsed ? g_uri_get_query(parsed) : NULL;

  if (query) {
    g_autoptr(GHashTable) params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
    if (params) {
      const char *toggle = g_hash_table_lookup(params, "toggle");
      const char *enabled_str = g_hash_table_lookup(params, "enabled");
      if (toggle && enabled_str) {
        gboolean enabled = g_str_equal(enabled_str, "true") || g_str_equal(enabled_str, "1");
        for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
          WebKitFeature *feature = webkit_feature_list_get(features, i);
          if (g_str_equal(webkit_feature_get_identifier(feature), toggle)) {
            g_debug("Setting feature %s to %d", toggle, enabled);
            webkit_settings_set_feature_enabled(settings, feature, enabled);
            break;
          }
        }
      }
    }
  }

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
      const char *details = webkit_feature_get_details(feature);
      GVariantBuilder feat_builder;
      g_variant_builder_init(&feat_builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&feat_builder, "{sv}", "identifier",
                            g_variant_new_string(webkit_feature_get_identifier(feature)));
      g_variant_builder_add(&feat_builder, "{sv}", "name", g_variant_new_string(webkit_feature_get_name(feature)));
      g_variant_builder_add(&feat_builder, "{sv}", "status",
                            g_variant_new_string(feature_status_string(webkit_feature_get_status(feature))));
      g_variant_builder_add(&feat_builder, "{sv}", "details", g_variant_new_string(details ? details : ""));
      g_variant_builder_add(&feat_builder, "{sv}", "default",
                            g_variant_new_boolean(webkit_feature_get_default_value(feature)));
      g_variant_builder_add(&feat_builder, "{sv}", "enabled",
                            g_variant_new_boolean(webkit_settings_get_feature_enabled(settings, feature)));
      g_variant_builder_add_value(&feats_builder, g_variant_builder_end(&feat_builder));
    }

    GVariantBuilder cat_builder;
    g_variant_builder_init(&cat_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&cat_builder, "{sv}", "name", g_variant_new_string(cat_name));
    g_variant_builder_add(&cat_builder, "{sv}", "features", g_variant_builder_end(&feats_builder));
    g_variant_builder_add_value(&cats_builder, g_variant_builder_end(&cat_builder));
  }

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_string(scope, "title", developer ? "Developer Features" : "Experimental Features");
  tmpl_scope_set_variant(scope, "categories", g_variant_builder_end(&cats_builder));

  return g_steal_pointer(&scope);
}
