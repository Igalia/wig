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

#include "wig-downloads.h"

#include "wig-application.h"
#include "wig-internal-page.h"

static char *format_bytes(guint64 bytes)
{
  return g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS);
}

TmplScope *handle_downloads_uri(WebKitURISchemeRequest *request, GPtrArray *downloads)
{
  const char *full_uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(full_uri, G_URI_FLAGS_NONE, NULL);
  const char *query = parsed ? g_uri_get_query(parsed) : NULL;
  g_autofree char *error_message = NULL;

  if (query) {
    g_autoptr(GHashTable) params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
    if (params) {
      const char *cancel_val = g_hash_table_lookup(params, "cancel");
      const char *clear_val = g_hash_table_lookup(params, "clear");
      const char *trash_val = g_hash_table_lookup(params, "trash");

      if (cancel_val) {
        guint64 idx = g_ascii_strtoull(cancel_val, NULL, 10);
        if (idx < downloads->len) {
          WigDownloadRecord *record = g_ptr_array_index(downloads, idx);
          if (record->state == WIG_DOWNLOAD_ACTIVE) {
            g_debug("downloads: cancelling index %" G_GUINT64_FORMAT, idx);
            webkit_download_cancel(record->download);
          }
        }
      }

      if (trash_val) {
        guint64 idx = g_ascii_strtoull(trash_val, NULL, 10);
        if (idx < downloads->len) {
          WigDownloadRecord *record = g_ptr_array_index(downloads, idx);
          const char *dest = webkit_download_get_destination(record->download);
          if (dest) {
            g_autoptr(GFile) file = g_file_new_for_path(dest);
            g_autoptr(GError) err = NULL;
            if (!g_file_trash(file, NULL, &err)) {
              g_warning("downloads: trash '%s': %s", dest, err->message);
              error_message = wig_internal_page_html_escape(err->message);
            } else
              g_debug("downloads: trashed '%s'", dest);
          }
        }
      }

      if (clear_val && g_str_equal(clear_val, "1")) {
        g_debug("downloads: clearing completed");
        for (gint i = (gint)downloads->len - 1; i >= 0; i--) {
          WigDownloadRecord *record = g_ptr_array_index(downloads, (guint)i);
          if (record->state != WIG_DOWNLOAD_ACTIVE)
            g_ptr_array_remove_index(downloads, (guint)i);
        }
      }
    }
  }

  GVariantBuilder items_builder;
  g_variant_builder_init(&items_builder, G_VARIANT_TYPE("aa{sv}"));

  for (gint i = (gint)downloads->len - 1; i >= 0; i--) {
    WigDownloadRecord *record = g_ptr_array_index(downloads, (guint)i);
    WebKitDownload *dl = record->download;

    const char *url = webkit_uri_request_get_uri(webkit_download_get_request(dl));
    const char *dest_path = webkit_download_get_destination(dl);

    g_autofree char *filename = NULL;
    g_autofree char *destination = NULL;
    if (dest_path) {
      destination = g_strdup(dest_path);
      filename = g_path_get_basename(dest_path);
    } else {
      destination = g_strdup("\xe2\x80\x94");
      g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
      const char *path_part = uri ? g_uri_get_path(uri) : NULL;
      const char *slash = path_part ? strrchr(path_part, '/') : NULL;
      filename = g_strdup(slash && slash[1] ? slash + 1 : url);
    }

    guint64 received = webkit_download_get_received_data_length(dl);
    WebKitURIResponse *response = webkit_download_get_response(dl);
    guint64 total = response ? webkit_uri_response_get_content_length(response) : 0;
    g_autofree char *received_str = format_bytes(received);
    g_autofree char *total_str = total > 0 ? format_bytes(total) : g_strdup("?");

    gdouble pct = webkit_download_get_estimated_progress(dl);
    g_autofree char *progress = g_strdup_printf("%.0f%%", pct * 100.0);

    gboolean in_progress = record->state == WIG_DOWNLOAD_ACTIVE;
    const char *status;
    switch (record->state) {
    case WIG_DOWNLOAD_COMPLETE:
      status = "complete";
      break;
    case WIG_DOWNLOAD_FAILED:
      status = "failed";
      break;
    case WIG_DOWNLOAD_CANCELLED:
      status = "cancelled";
      break;
    default:
      status = "downloading";
      break;
    }

    gboolean can_trash = !in_progress && dest_path != NULL && g_file_test(dest_path, G_FILE_TEST_EXISTS);
    g_autofree char *index_str = g_strdup_printf("%d", i);

    GVariantBuilder item_builder;
    g_variant_builder_init(&item_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&item_builder, "{sv}", "filename", g_variant_new_string(filename));
    g_variant_builder_add(&item_builder, "{sv}", "url", g_variant_new_string(url));
    g_variant_builder_add(&item_builder, "{sv}", "destination", g_variant_new_string(destination));
    g_variant_builder_add(&item_builder, "{sv}", "received", g_variant_new_string(received_str));
    g_variant_builder_add(&item_builder, "{sv}", "total", g_variant_new_string(total_str));
    g_variant_builder_add(&item_builder, "{sv}", "progress", g_variant_new_string(progress));
    g_variant_builder_add(&item_builder, "{sv}", "status", g_variant_new_string(status));
    g_variant_builder_add(&item_builder, "{sv}", "index_str", g_variant_new_string(index_str));
    g_variant_builder_add(&item_builder, "{sv}", "in_progress", g_variant_new_boolean(in_progress));
    g_variant_builder_add(&item_builder, "{sv}", "can_trash", g_variant_new_boolean(can_trash));
    g_variant_builder_add_value(&items_builder, g_variant_builder_end(&item_builder));
  }

  gboolean has_downloads = downloads->len > 0;
  gboolean has_completed = FALSE;
  for (guint i = 0; i < downloads->len; i++) {
    WigDownloadRecord *r = g_ptr_array_index(downloads, i);
    if (r->state != WIG_DOWNLOAD_ACTIVE) {
      has_completed = TRUE;
      break;
    }
  }

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_variant(scope, "items", g_variant_builder_end(&items_builder));
  tmpl_scope_set_boolean(scope, "has_downloads", has_downloads);
  tmpl_scope_set_boolean(scope, "has_completed", has_completed);
  tmpl_scope_set_string(scope, "error_message", error_message ? error_message : "");

  return g_steal_pointer(&scope);
}
