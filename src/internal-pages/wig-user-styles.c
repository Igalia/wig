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

#include "wig-user-styles.h"
#include "wig-internal-page.h"

void wig_user_style_sheet_record_free(WigUserStyleSheetRecord *record)
{
  g_free(record->source);
  webkit_user_style_sheet_unref(record->stylesheet);
  g_free(record);
}

static void add_style_sheet_item(GVariantBuilder *items_builder, WigUserStyleSheetRecord *record, guint index)
{
  g_autofree char *index_str = g_strdup_printf("%u", index);
  g_autofree char *source = g_markup_escape_text(record->source, -1);

  guint lines = 1;
  for (const char *p = record->source; *p; p++) {
    if (*p == '\n')
      lines++;
  }
  g_autofree char *line_count = g_strdup_printf("%u line%s", lines, lines == 1 ? "" : "s");

  const char *level_label = record->level == WEBKIT_USER_STYLE_LEVEL_AUTHOR ? "Author Level" : "User Level";

  const char *frames_label = record->injected_frames == WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES ? "All Frames"
                                                                                              : "Top Frame Only";

  GVariantBuilder item_builder;
  g_variant_builder_init(&item_builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&item_builder, "{sv}", "index_str", g_variant_new_string(index_str));
  g_variant_builder_add(&item_builder, "{sv}", "source", g_variant_new_string(source));
  g_variant_builder_add(&item_builder, "{sv}", "line_count", g_variant_new_string(line_count));
  g_variant_builder_add(&item_builder, "{sv}", "level_label", g_variant_new_string(level_label));
  g_variant_builder_add(&item_builder, "{sv}", "frames_label", g_variant_new_string(frames_label));
  g_variant_builder_add_value(items_builder, g_variant_builder_end(&item_builder));
}

static void add_style_sheet_from_params(WebKitUserContentManager *manager, GPtrArray *style_sheets, GHashTable *params)
{
  const char *source_val = g_hash_table_lookup(params, "source");
  if (!source_val || !*source_val)
    return;

  const char *level_val = g_hash_table_lookup(params, "level");
  const char *frames_val = g_hash_table_lookup(params, "frames");

  WebKitUserStyleLevel level = (g_strcmp0(level_val, "author") == 0) ? WEBKIT_USER_STYLE_LEVEL_AUTHOR
                                                                     : WEBKIT_USER_STYLE_LEVEL_USER;

  WebKitUserContentInjectedFrames injected_frames = (g_strcmp0(frames_val, "top") == 0)
      ? WEBKIT_USER_CONTENT_INJECT_TOP_FRAME
      : WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES;

  g_autoptr(WebKitUserStyleSheet) stylesheet = webkit_user_style_sheet_new(source_val, injected_frames, level, NULL,
                                                                           NULL);

  WigUserStyleSheetRecord *record = g_new0(WigUserStyleSheetRecord, 1);
  record->source = g_strdup(source_val);
  record->level = level;
  record->injected_frames = injected_frames;
  record->stylesheet = g_steal_pointer(&stylesheet);

  webkit_user_content_manager_add_style_sheet(manager, record->stylesheet);
  g_ptr_array_add(style_sheets, record);

  g_debug("user-styles: added style sheet (%s, %s), total %u",
          level == WEBKIT_USER_STYLE_LEVEL_AUTHOR ? "author" : "user",
          injected_frames == WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES ? "all" : "top", style_sheets->len);
}

static void finish_request(WebKitURISchemeRequest *request, GPtrArray *style_sheets)
{
  GVariantBuilder items_builder;
  g_variant_builder_init(&items_builder, G_VARIANT_TYPE("aa{sv}"));
  for (guint i = 0; i < style_sheets->len; i++)
    add_style_sheet_item(&items_builder, g_ptr_array_index(style_sheets, i), i);

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_variant(scope, "items", g_variant_builder_end(&items_builder));
  tmpl_scope_set_boolean(scope, "has_styles", style_sheets->len > 0);

  g_autofree char *html = wig_internal_page_render("/com/igalia/wig/internal-pages/user-styles.html", scope);
  wig_internal_page_finish_request(request, g_steal_pointer(&html));
}

typedef struct {
  WebKitUserContentManager *manager;
  GPtrArray *style_sheets;
} StylesContext;

static StylesContext *styles_context_new(WebKitUserContentManager *manager, GPtrArray *style_sheets)
{
  StylesContext *ctx = g_new0(StylesContext, 1);
  ctx->manager = g_object_ref(manager);
  ctx->style_sheets = style_sheets;
  return ctx;
}

static void styles_context_free(gpointer data)
{
  StylesContext *ctx = data;
  g_object_unref(ctx->manager);
  g_free(ctx);
}

static void on_body_read(WebKitURISchemeRequest *request, GHashTable *params, gpointer user_data)
{
  StylesContext *ctx = user_data;
  if (params)
    add_style_sheet_from_params(ctx->manager, ctx->style_sheets, params);
  finish_request(request, ctx->style_sheets);
}

void handle_user_styles_uri(WebKitURISchemeRequest *request, WebKitUserContentManager *manager, GPtrArray *style_sheets)
{
  /* New style sheets are submitted via a POST form. */
  if (wig_internal_page_read_form_body(request, on_body_read, styles_context_new(manager, style_sheets),
                                       styles_context_free))
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
      if (idx < style_sheets->len) {
        WigUserStyleSheetRecord *record = g_ptr_array_index(style_sheets, idx);
        g_debug("user-styles: removing index %" G_GUINT64_FORMAT, idx);
        webkit_user_content_manager_remove_style_sheet(manager, record->stylesheet);
        g_ptr_array_remove_index(style_sheets, (guint)idx);
      }
    }
  }

  finish_request(request, style_sheets);
}
