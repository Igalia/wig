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

#include "wig-memory-pressure.h"

TmplScope *handle_memory_pressure_uri(WebKitURISchemeRequest *request, WebKitMemoryPressureSettings *settings)
{
  const char *full_uri = webkit_uri_scheme_request_get_uri(request);
  g_autoptr(GUri) parsed = g_uri_parse(full_uri, G_URI_FLAGS_NONE, NULL);
  const char *query = parsed ? g_uri_get_query(parsed) : NULL;

  if (query) {
    g_autoptr(GHashTable) params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
    if (params) {
      const char *val;

      val = g_hash_table_lookup(params, "memory_limit");
      if (val) {
        guint64 limit = g_ascii_strtoull(val, NULL, 10);
        g_debug("memory-pressure: setting memory_limit to %" G_GUINT64_FORMAT " MiB", limit);
        webkit_memory_pressure_settings_set_memory_limit(settings, (guint)limit);
      }

      val = g_hash_table_lookup(params, "conservative_threshold");
      if (val) {
        gdouble v = g_ascii_strtod(val, NULL);
        g_debug("memory-pressure: setting conservative_threshold to %g", v);
        webkit_memory_pressure_settings_set_conservative_threshold(settings, v);
      }

      val = g_hash_table_lookup(params, "strict_threshold");
      if (val) {
        gdouble v = g_ascii_strtod(val, NULL);
        g_debug("memory-pressure: setting strict_threshold to %g", v);
        webkit_memory_pressure_settings_set_strict_threshold(settings, v);
      }

      val = g_hash_table_lookup(params, "kill_threshold");
      if (val) {
        gdouble v = g_ascii_strtod(val, NULL);
        g_debug("memory-pressure: setting kill_threshold to %g", v);
        webkit_memory_pressure_settings_set_kill_threshold(settings, v);
      }

      val = g_hash_table_lookup(params, "poll_interval");
      if (val) {
        gdouble v = g_ascii_strtod(val, NULL);
        g_debug("memory-pressure: setting poll_interval to %g", v);
        webkit_memory_pressure_settings_set_poll_interval(settings, v);
      }

      webkit_network_session_set_memory_pressure_settings(settings);
    }
  }

  g_autofree char *memory_limit_str = g_strdup_printf("%u", webkit_memory_pressure_settings_get_memory_limit(settings));
  g_autofree char *conservative_str = g_strdup_printf(
      "%.2f", webkit_memory_pressure_settings_get_conservative_threshold(settings));
  g_autofree char *strict_str = g_strdup_printf("%.2f", webkit_memory_pressure_settings_get_strict_threshold(settings));
  g_autofree char *kill_str = g_strdup_printf("%.2f", webkit_memory_pressure_settings_get_kill_threshold(settings));
  g_autofree char *poll_str = g_strdup_printf("%.1f", webkit_memory_pressure_settings_get_poll_interval(settings));

  g_autoptr(TmplScope) scope = tmpl_scope_new();
  tmpl_scope_set_string(scope, "memory_limit", memory_limit_str);
  tmpl_scope_set_string(scope, "conservative_threshold", conservative_str);
  tmpl_scope_set_string(scope, "strict_threshold", strict_str);
  tmpl_scope_set_string(scope, "kill_threshold", kill_str);
  tmpl_scope_set_string(scope, "poll_interval", poll_str);
  tmpl_scope_set_boolean(scope, "saved", query != NULL);

  return g_steal_pointer(&scope);
}
