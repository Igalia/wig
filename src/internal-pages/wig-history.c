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

#include "wig-history.h"

static gint64 parse_int64_param(GHashTable *params, const char *name, gint64 fallback)
{
  const char *value = g_hash_table_lookup(params, name);
  if (!value || !*value)
    return fallback;
  return (gint64)g_ascii_strtoll(value, NULL, 10);
}

static guint parse_uint_param(GHashTable *params, const char *name, guint fallback)
{
  const char *value = g_hash_table_lookup(params, name);
  if (!value || !*value)
    return fallback;
  return (guint)g_ascii_strtoull(value, NULL, 10);
}

static char *format_visit_time(gint64 visit_time)
{
  g_autoptr(GDateTime) date_time = g_date_time_new_from_unix_local(visit_time / 1000);
  return date_time ? g_date_time_format(date_time, "%x %X") : g_strdup("");
}

static char *build_history_url(const char *query, gint64 before_time, guint limit)
{
  g_autoptr(GString) url = g_string_new("wig:history");
  gboolean has_param = FALSE;

  if (query && *query) {
    g_autofree char *escaped = g_uri_escape_string(query, NULL, TRUE);
    g_string_append_printf(url, "%cq=%s", has_param ? '&' : '?', escaped);
    has_param = TRUE;
  }

  if (before_time > 0) {
    g_string_append_printf(url, "%cbefore=%" G_GINT64_FORMAT, has_param ? '&' : '?', before_time);
    has_param = TRUE;
  }

  if (limit != 100)
    g_string_append_printf(url, "%climit=%u", has_param ? '&' : '?', limit);

  return g_string_free(g_steal_pointer(&url), FALSE);
}

TmplScope *handle_history_uri(WebKitURISchemeRequest *request, WigHistoryStore *store)
{
  const char *full_uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(full_uri, G_URI_FLAGS_NONE, NULL);
  const char *raw_query = parsed ? g_uri_get_query(parsed) : NULL;
  g_autoptr(GHashTable) params = raw_query ? g_uri_parse_params(raw_query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;

  const char *query = params ? g_hash_table_lookup(params, "q") : NULL;
  gint64 before_time = params ? parse_int64_param(params, "before", 0) : 0;
  guint limit = params ? parse_uint_param(params, "limit", 100) : 100;
  limit = CLAMP(limit, 1, 500);

  gboolean has_more = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = store ? wig_history_store_query(store, query, before_time, limit, &has_more, &error)
                                     : g_ptr_array_new_with_free_func(g_object_unref);
  if (!items) {
    g_warning("history: query failed: %s", error->message);
    items = g_ptr_array_new_with_free_func(g_object_unref);
  }

  GVariantBuilder items_builder;
  g_variant_builder_init(&items_builder, G_VARIANT_TYPE("aa{sv}"));

  gint64 next_before = 0;
  for (guint i = 0; i < items->len; i++) {
    WigHistoryItem *item = g_ptr_array_index(items, i);
    gint64 visit_time = wig_history_item_get_last_visit_time(item);
    next_before = visit_time;

    g_autofree char *visit_time_str = g_strdup_printf("%" G_GINT64_FORMAT, visit_time);
    g_autofree char *visit_count_str = g_strdup_printf("%u", wig_history_item_get_visit_count(item));
    g_autofree char *typed_count_str = g_strdup_printf("%u", wig_history_item_get_typed_count(item));
    g_autofree char *formatted_time = format_visit_time(visit_time);

    GVariantBuilder item_builder;
    g_variant_builder_init(&item_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&item_builder, "{sv}", "id", g_variant_new_string(wig_history_item_get_id(item)));
    g_variant_builder_add(&item_builder, "{sv}", "url", g_variant_new_string(wig_history_item_get_url(item)));
    g_variant_builder_add(&item_builder, "{sv}", "title", g_variant_new_string(wig_history_item_get_title(item)));
    g_variant_builder_add(&item_builder, "{sv}", "last_visit_time", g_variant_new_string(visit_time_str));
    g_variant_builder_add(&item_builder, "{sv}", "formatted_time", g_variant_new_string(formatted_time));
    g_variant_builder_add(&item_builder, "{sv}", "visit_count", g_variant_new_string(visit_count_str));
    g_variant_builder_add(&item_builder, "{sv}", "typed_count", g_variant_new_string(typed_count_str));
    g_variant_builder_add_value(&items_builder, g_variant_builder_end(&item_builder));
  }

  g_autofree char *next_url = has_more ? build_history_url(query, next_before, limit) : g_strdup("");

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_string(scope, "query", query ? query : "");
  tmpl_scope_set_string(scope, "next_url", next_url);
  tmpl_scope_set_variant(scope, "items", g_variant_builder_end(&items_builder));
  tmpl_scope_set_boolean(scope, "has_items", items->len > 0);
  tmpl_scope_set_boolean(scope, "has_more", has_more);
  tmpl_scope_set_boolean(scope, "has_query", query && *query);

  return g_steal_pointer(&scope);
}