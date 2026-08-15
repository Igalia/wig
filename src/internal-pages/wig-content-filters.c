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

#include "wig-content-filters.h"
#include "wig-internal-page.h"

typedef struct {
  WebKitURISchemeRequest *request;
  WebKitUserContentManager *manager;
  WebKitUserContentFilterStore *store;
  /* Set when an add failed so the UI can show the error message. */
  char *error_message;
} FiltersState;

static FiltersState *filters_state_new(WebKitURISchemeRequest *request, WebKitUserContentManager *manager,
                                       WebKitUserContentFilterStore *store)
{
  FiltersState *state = g_new0(FiltersState, 1);
  state->request = g_object_ref(request);
  state->manager = g_object_ref(manager);
  state->store = g_object_ref(store);
  return state;
}

static void filters_state_free(FiltersState *state)
{
  g_object_unref(state->request);
  g_object_unref(state->manager);
  g_object_unref(state->store);
  g_free(state->error_message);
  g_free(state);
}

static void do_fetch_identifiers(FiltersState *state);

static void finish_request(FiltersState *state, char **identifiers)
{
  GVariantBuilder items_builder;
  g_variant_builder_init(&items_builder, G_VARIANT_TYPE("aa{sv}"));

  guint n_filters = 0;
  if (identifiers) {
    for (char **id = identifiers; *id; id++, n_filters++) {
      GVariantBuilder item_builder;
      g_variant_builder_init(&item_builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&item_builder, "{sv}", "identifier", g_variant_new_string(*id));
      g_variant_builder_add_value(&items_builder, g_variant_builder_end(&item_builder));
    }
  }

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_variant(scope, "items", g_variant_builder_end(&items_builder));
  tmpl_scope_set_boolean(scope, "has_filters", n_filters > 0);
  if (state->error_message)
    tmpl_scope_set_string(scope, "error_message", state->error_message);
  else
    tmpl_scope_set_string(scope, "error_message", "");

  g_autofree char *html = wig_internal_page_render("/com/igalia/wig/internal-pages/content-filters.html", scope);
  wig_internal_page_finish_request(state->request, g_steal_pointer(&html));
  filters_state_free(state);
}

static void on_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  FiltersState *state = user_data;
  char **identifiers = webkit_user_content_filter_store_fetch_identifiers_finish(
      WEBKIT_USER_CONTENT_FILTER_STORE(source), res);
  finish_request(state, identifiers);
  g_strfreev(identifiers);
}

static void do_fetch_identifiers(FiltersState *state)
{
  webkit_user_content_filter_store_fetch_identifiers(state->store, NULL, on_fetch_done, state);
}

typedef struct {
  FiltersState *state;
  char *identifier;
} SaveState;

static void on_save_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  SaveState *save = user_data;
  FiltersState *state = save->state;
  GError *error = NULL;

  g_autoptr(WebKitUserContentFilter) filter = webkit_user_content_filter_store_save_finish(
      WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error);

  if (filter) {
    webkit_user_content_manager_add_filter(state->manager, filter);
    g_debug("content-filters: added filter '%s'", save->identifier);
  } else {
    g_warning("content-filters: failed to compile '%s': %s", save->identifier, error->message);
    state->error_message = g_strdup(error->message);
    g_clear_error(&error);
  }

  g_free(save->identifier);
  g_free(save);

  do_fetch_identifiers(state);
}

static void add_filter_from_params(FiltersState *state, GHashTable *params)
{
  const char *identifier = g_hash_table_lookup(params, "identifier");
  const char *source = g_hash_table_lookup(params, "source");

  if (!identifier || !*identifier || !source || !*source) {
    do_fetch_identifiers(state);
    return;
  }

  g_autoptr(GBytes) bytes = g_bytes_new(source, strlen(source));

  SaveState *save = g_new0(SaveState, 1);
  save->state = state;
  save->identifier = g_strdup(identifier);

  webkit_user_content_filter_store_save(state->store, identifier, bytes, NULL, on_save_done, save);
}

static void on_remove_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  FiltersState *state = user_data;
  GError *error = NULL;
  if (!webkit_user_content_filter_store_remove_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error)) {
    g_warning("content-filters: remove failed: %s", error->message);
    g_clear_error(&error);
  }
  do_fetch_identifiers(state);
}

static void remove_filter(FiltersState *state, const char *identifier)
{
  webkit_user_content_manager_remove_filter_by_id(state->manager, identifier);
  webkit_user_content_filter_store_remove(state->store, identifier, NULL, on_remove_done, state);
}

typedef struct {
  WebKitUserContentManager *manager;
  WebKitUserContentFilterStore *store;
} FiltersContext;

static void filters_context_free(gpointer data)
{
  FiltersContext *ctx = data;
  g_object_unref(ctx->manager);
  g_object_unref(ctx->store);
  g_free(ctx);
}

static void on_body_read(WebKitURISchemeRequest *request, GHashTable *params, gpointer user_data)
{
  FiltersContext *ctx = user_data;
  FiltersState *state = filters_state_new(request, ctx->manager, ctx->store);
  if (params)
    add_filter_from_params(state, params);
  else
    do_fetch_identifiers(state);
}

void handle_content_filters_uri(WebKitURISchemeRequest *request, WebKitUserContentManager *manager,
                                WebKitUserContentFilterStore *store)
{
  /* New filters are submitted via a POST form. */
  FiltersContext *ctx = g_new0(FiltersContext, 1);
  ctx->manager = g_object_ref(manager);
  ctx->store = g_object_ref(store);
  if (wig_internal_page_read_form_body(request, on_body_read, ctx, filters_context_free))
    return;

  /* Removal is a plain GET link carrying the identifier in the query. */
  const char *full_uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(full_uri, G_URI_FLAGS_NONE, NULL);
  const char *query = parsed ? g_uri_get_query(parsed) : NULL;

  FiltersState *state = filters_state_new(request, manager, store);

  if (query) {
    g_autoptr(GHashTable) params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
    const char *remove_val = params ? g_hash_table_lookup(params, "remove") : NULL;
    if (remove_val) {
      g_autofree char *identifier = g_uri_unescape_string(remove_val, NULL);
      g_debug("content-filters: removing '%s'", identifier);
      remove_filter(state, identifier);
      return;
    }
  }

  do_fetch_identifiers(state);
}

typedef struct {
  WebKitUserContentManager *manager;
  char *identifier;
} LoadSavedState;

static void on_load_saved_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoadSavedState *load = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(WebKitUserContentFilter) filter = webkit_user_content_filter_store_load_finish(
      WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error);

  if (filter) {
    webkit_user_content_manager_add_filter(load->manager, filter);
    g_debug("content-filters: restored filter '%s'", load->identifier);
  } else {
    g_warning("content-filters: failed to restore '%s': %s", load->identifier, error->message);
  }

  g_object_unref(load->manager);
  g_free(load->identifier);
  g_free(load);
}

static void on_load_saved_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr(WebKitUserContentManager) manager = user_data;
  WebKitUserContentFilterStore *store = WEBKIT_USER_CONTENT_FILTER_STORE(source);
  g_auto(GStrv) identifiers = webkit_user_content_filter_store_fetch_identifiers_finish(store, res);

  for (char **id = identifiers; id && *id; id++) {
    LoadSavedState *load = g_new0(LoadSavedState, 1);
    load->manager = g_object_ref(manager);
    load->identifier = g_strdup(*id);
    webkit_user_content_filter_store_load(store, *id, NULL, on_load_saved_done, load);
  }
}

void wig_content_filters_load_saved(WebKitUserContentManager *manager, WebKitUserContentFilterStore *store)
{
  webkit_user_content_filter_store_fetch_identifiers(store, NULL, on_load_saved_fetch_done, g_object_ref(manager));
}
