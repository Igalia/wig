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

#include "wig-website-data.h"
#include "wig-internal-page.h"

static const struct {
  WebKitWebsiteDataTypes type;
  const char *name;
} data_types[] = {
  { WEBKIT_WEBSITE_DATA_MEMORY_CACHE, "Memory Cache" },
  { WEBKIT_WEBSITE_DATA_DISK_CACHE, "Disk Cache" },
  { WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE, "Offline Application Cache" },
  { WEBKIT_WEBSITE_DATA_SESSION_STORAGE, "Session Storage" },
  { WEBKIT_WEBSITE_DATA_LOCAL_STORAGE, "Local Storage" },
  { WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES, "IndexedDB Databases" },
  { WEBKIT_WEBSITE_DATA_COOKIES, "Cookies" },
  { WEBKIT_WEBSITE_DATA_DEVICE_ID_HASH_SALT, "Device ID Hash Salt" },
  { WEBKIT_WEBSITE_DATA_HSTS_CACHE, "HSTS Cache" },
  { WEBKIT_WEBSITE_DATA_ITP, "ITP" },
  { WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS, "Service Worker Registrations" },
  { WEBKIT_WEBSITE_DATA_DOM_CACHE, "DOM Cache" },
};

static const char *type_name_for_value(guint64 value)
{
  for (gsize i = 0; i < G_N_ELEMENTS(data_types); i++) {
    if ((guint64)data_types[i].type == value)
      return data_types[i].name;
  }
  return "Unknown";
}

static char *format_size(guint64 bytes)
{
  if (bytes == 0)
    return g_strdup("\xe2\x80\x94"); /* em dash */
  return g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS);
}

typedef struct {
  WebKitURISchemeRequest *request;
  WebKitWebsiteDataManager *manager;
  WebKitWebsiteDataTypes fetch_types;
  char *remove_origin;
  GList *fetch_results;
} WebsiteDataState;

static void do_final_fetch(WebsiteDataState *state);

static void website_data_state_free(WebsiteDataState *state)
{
  g_object_unref(state->request);
  g_object_unref(state->manager);
  g_free(state->remove_origin);
  g_list_free_full(state->fetch_results, (GDestroyNotify)webkit_website_data_unref);
  g_free(state);
}

static void finish_request(WebsiteDataState *state)
{
  GVariantBuilder types_builder;
  g_variant_builder_init(&types_builder, G_VARIANT_TYPE("aa{sv}"));
  for (gsize i = 0; i < G_N_ELEMENTS(data_types); i++) {
    g_autofree char *key = g_strdup_printf("%u", (guint)data_types[i].type);
    GVariantBuilder item_builder;
    g_variant_builder_init(&item_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&item_builder, "{sv}", "name", g_variant_new_string(data_types[i].name));
    g_variant_builder_add(&item_builder, "{sv}", "key", g_variant_new_string(key));
    g_variant_builder_add(&item_builder, "{sv}", "active",
                          g_variant_new_boolean(state->fetch_types == data_types[i].type));
    g_variant_builder_add_value(&types_builder, g_variant_builder_end(&item_builder));
  }

  GVariantBuilder entries_builder;
  g_variant_builder_init(&entries_builder, G_VARIANT_TYPE("aa{sv}"));
  guint n_entries = 0;
  for (GList *l = state->fetch_results; l; l = l->next) {
    WebKitWebsiteData *data = l->data;
    const char *name = webkit_website_data_get_name(data);
    g_autofree char *origin_escaped = g_uri_escape_string(name, NULL, FALSE);
    g_autofree char *size = format_size(webkit_website_data_get_size(data, state->fetch_types));
    GVariantBuilder entry_builder;
    g_variant_builder_init(&entry_builder, G_VARIANT_TYPE("a{sv}"));
    g_autofree char *origin = g_markup_escape_text(name, -1);
    g_variant_builder_add(&entry_builder, "{sv}", "origin", g_variant_new_string(origin));
    g_variant_builder_add(&entry_builder, "{sv}", "origin_escaped", g_variant_new_string(origin_escaped));
    g_variant_builder_add(&entry_builder, "{sv}", "size", g_variant_new_string(size));
    g_variant_builder_add_value(&entries_builder, g_variant_builder_end(&entry_builder));
    n_entries++;
  }

  g_autofree char *fetch_key = state->fetch_types ? g_strdup_printf("%u", (guint)state->fetch_types) : g_strdup("0");

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_variant(scope, "types", g_variant_builder_end(&types_builder));
  tmpl_scope_set_variant(scope, "entries", g_variant_builder_end(&entries_builder));
  tmpl_scope_set_boolean(scope, "has_results", state->fetch_types != 0);
  tmpl_scope_set_boolean(scope, "has_entries", n_entries > 0);
  tmpl_scope_set_string(scope, "fetched_type_name", type_name_for_value((guint64)state->fetch_types));
  tmpl_scope_set_string(scope, "fetch_key", fetch_key);

  g_autofree char *html = wig_internal_page_render("/com/igalia/wig/internal-pages/website-data.html", scope);
  wig_internal_page_finish_request(state->request, g_steal_pointer(&html));

  website_data_state_free(state);
}

static void on_final_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  WebsiteDataState *state = user_data;
  GError *error = NULL;
  state->fetch_results = webkit_website_data_manager_fetch_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), res, &error);
  if (error) {
    g_warning("website-data: fetch failed: %s", error->message);
    g_clear_error(&error);
  }
  finish_request(state);
}

static void do_final_fetch(WebsiteDataState *state)
{
  webkit_website_data_manager_fetch(state->manager, state->fetch_types, NULL, on_final_fetch_done, state);
}

static void on_clear_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  WebsiteDataState *state = user_data;
  GError *error = NULL;
  webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), res, &error);
  if (error) {
    g_warning("website-data: clear failed: %s", error->message);
    g_clear_error(&error);
  }
  do_final_fetch(state);
}

static void on_remove_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  WebsiteDataState *state = user_data;
  GError *error = NULL;
  webkit_website_data_manager_remove_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), res, &error);
  if (error) {
    g_warning("website-data: remove failed: %s", error->message);
    g_clear_error(&error);
  }
  do_final_fetch(state);
}

static void on_initial_fetch_for_remove_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  WebsiteDataState *state = user_data;
  GError *error = NULL;
  g_autolist(WebKitWebsiteData) all = webkit_website_data_manager_fetch_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), res,
                                                                               &error);
  if (error) {
    g_warning("website-data: fetch for remove failed: %s", error->message);
    g_clear_error(&error);
    do_final_fetch(state);
    return;
  }

  g_autolist(WebKitWebsiteData) to_remove = NULL;
  for (GList *l = all; l; l = l->next) {
    WebKitWebsiteData *data = l->data;
    if (g_str_equal(webkit_website_data_get_name(data), state->remove_origin)) {
      to_remove = g_list_prepend(to_remove, webkit_website_data_ref(data));
      break;
    }
  }
  if (!to_remove) {
    g_warning("website-data: origin '%s' not found for removal", state->remove_origin);
    do_final_fetch(state);
    return;
  }

  webkit_website_data_manager_remove(state->manager, state->fetch_types, g_steal_pointer(&to_remove), NULL,
                                     on_remove_done, state);
}

void handle_website_data_uri(WebKitURISchemeRequest *request, WebKitWebsiteDataManager *manager)
{
  WebsiteDataState *state = g_new0(WebsiteDataState, 1);
  state->request = g_object_ref(request);
  state->manager = g_object_ref(manager);

  const char *full_uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(full_uri, G_URI_FLAGS_NONE, NULL);
  const char *query = parsed ? g_uri_get_query(parsed) : NULL;

  if (query) {
    g_autoptr(GHashTable) params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
    if (params) {
      const char *fetch_val = g_hash_table_lookup(params, "fetch");
      const char *action = g_hash_table_lookup(params, "action");
      const char *types_val = g_hash_table_lookup(params, "types");
      const char *origin_val = g_hash_table_lookup(params, "origin");

      if (fetch_val) {
        state->fetch_types = (WebKitWebsiteDataTypes)g_ascii_strtoull(fetch_val, NULL, 10);
        g_debug("website-data: fetching type %u", (guint)state->fetch_types);
        do_final_fetch(state);
        return;
      }

      if (action && types_val) {
        state->fetch_types = (WebKitWebsiteDataTypes)g_ascii_strtoull(types_val, NULL, 10);

        if (g_str_equal(action, "clear")) {
          g_debug("website-data: clearing type %u", (guint)state->fetch_types);
          webkit_website_data_manager_clear(state->manager, state->fetch_types, 0, NULL, on_clear_done, state);
          return;
        }

        if (g_str_equal(action, "remove") && origin_val) {
          state->remove_origin = g_uri_unescape_string(origin_val, NULL);
          g_debug("website-data: removing origin '%s' for type %u", state->remove_origin, (guint)state->fetch_types);
          webkit_website_data_manager_fetch(state->manager, state->fetch_types, NULL, on_initial_fetch_for_remove_done,
                                            state);
          return;
        }
      }
    }
  }

  finish_request(state);
}
