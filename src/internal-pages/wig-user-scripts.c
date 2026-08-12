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

#include "wig-user-scripts.h"
#include "wig-internal-page.h"

void wig_user_script_record_free(WigUserScriptRecord *record)
{
  g_free(record->source);
  webkit_user_script_unref(record->script);
  g_free(record);
}

static void add_script_item(GVariantBuilder *items_builder, WigUserScriptRecord *record, guint index)
{
  g_autofree char *index_str = g_strdup_printf("%u", index);
  g_autofree char *source = g_markup_escape_text(record->source, -1);

  guint lines = 1;
  for (const char *p = record->source; *p; p++) {
    if (*p == '\n')
      lines++;
  }
  g_autofree char *line_count = g_strdup_printf("%u line%s", lines, lines == 1 ? "" : "s");

  const char *time_label = record->injection_time == WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START ? "Document Start"
                                                                                                 : "Document End";

  const char *frames_label = record->injected_frames == WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES ? "All Frames"
                                                                                              : "Top Frame Only";

  GVariantBuilder item_builder;
  g_variant_builder_init(&item_builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&item_builder, "{sv}", "index_str", g_variant_new_string(index_str));
  g_variant_builder_add(&item_builder, "{sv}", "source", g_variant_new_string(source));
  g_variant_builder_add(&item_builder, "{sv}", "line_count", g_variant_new_string(line_count));
  g_variant_builder_add(&item_builder, "{sv}", "time_label", g_variant_new_string(time_label));
  g_variant_builder_add(&item_builder, "{sv}", "frames_label", g_variant_new_string(frames_label));
  g_variant_builder_add_value(items_builder, g_variant_builder_end(&item_builder));
}

static void add_script_from_params(WebKitUserContentManager *manager, GPtrArray *scripts, GHashTable *params)
{
  const char *source_val = g_hash_table_lookup(params, "source");
  if (!source_val || !*source_val)
    return;

  const char *time_val = g_hash_table_lookup(params, "time");
  const char *frames_val = g_hash_table_lookup(params, "frames");

  WebKitUserScriptInjectionTime injection_time = (g_strcmp0(time_val, "end") == 0)
      ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END
      : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START;

  WebKitUserContentInjectedFrames injected_frames = (g_strcmp0(frames_val, "top") == 0)
      ? WEBKIT_USER_CONTENT_INJECT_TOP_FRAME
      : WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES;

  g_autoptr(WebKitUserScript) script = webkit_user_script_new(source_val, injected_frames, injection_time, NULL, NULL);

  WigUserScriptRecord *record = g_new0(WigUserScriptRecord, 1);
  record->source = g_strdup(source_val);
  record->injection_time = injection_time;
  record->injected_frames = injected_frames;
  record->script = g_steal_pointer(&script);

  webkit_user_content_manager_add_script(manager, record->script);
  g_ptr_array_add(scripts, record);

  g_debug("user-scripts: added script (%s, %s), total %u",
          injection_time == WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START ? "start" : "end",
          injected_frames == WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES ? "all" : "top", scripts->len);
}

static void finish_request(WebKitURISchemeRequest *request, GPtrArray *scripts)
{
  GVariantBuilder items_builder;
  g_variant_builder_init(&items_builder, G_VARIANT_TYPE("aa{sv}"));
  for (guint i = 0; i < scripts->len; i++)
    add_script_item(&items_builder, g_ptr_array_index(scripts, i), i);

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_variant(scope, "items", g_variant_builder_end(&items_builder));
  tmpl_scope_set_boolean(scope, "has_scripts", scripts->len > 0);

  g_autofree char *html = wig_internal_page_render("/com/igalia/wig/internal-pages/user-scripts.html", scope);
  wig_internal_page_finish_request(request, g_steal_pointer(&html));
}

typedef struct {
  WebKitUserContentManager *manager;
  GPtrArray *scripts;
} ScriptsContext;

static ScriptsContext *scripts_context_new(WebKitUserContentManager *manager, GPtrArray *scripts)
{
  ScriptsContext *ctx = g_new0(ScriptsContext, 1);
  ctx->manager = g_object_ref(manager);
  ctx->scripts = scripts;
  return ctx;
}

static void scripts_context_free(gpointer data)
{
  ScriptsContext *ctx = data;
  g_object_unref(ctx->manager);
  g_free(ctx);
}

static void on_body_read(WebKitURISchemeRequest *request, GHashTable *params, gpointer user_data)
{
  ScriptsContext *ctx = user_data;
  if (params)
    add_script_from_params(ctx->manager, ctx->scripts, params);
  finish_request(request, ctx->scripts);
}

void handle_user_scripts_uri(WebKitURISchemeRequest *request, WebKitUserContentManager *manager, GPtrArray *scripts)
{
  /* New scripts are submitted via a POST form. */
  if (wig_internal_page_read_form_body(request, on_body_read, scripts_context_new(manager, scripts),
                                       scripts_context_free))
    return;

  /* Removal is a plain GET link carrying the index in the query. */
  const char *full_uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(full_uri, G_URI_FLAGS_NONE, NULL);
  const char *query = parsed ? g_uri_get_query(parsed) : NULL;

  if (query) {
    g_autoptr(GHashTable) params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
    const char *remove_val = params ? g_hash_table_lookup(params, "remove") : NULL;
    if (remove_val) {
      guint64 idx = g_ascii_strtoull(remove_val, NULL, 10);
      if (idx < scripts->len) {
        WigUserScriptRecord *record = g_ptr_array_index(scripts, idx);
        g_debug("user-scripts: removing index %" G_GUINT64_FORMAT, idx);
        webkit_user_content_manager_remove_script(manager, record->script);
        g_ptr_array_remove_index(scripts, (guint)idx);
      }
    }
  }

  finish_request(request, scripts);
}
