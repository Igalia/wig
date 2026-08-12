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

#include "wig-internal-page.h"

#include <libsoup/soup.h>

static const char *error_page = "<!DOCTYPE html><html><body>Error loading page.</body></html>";

char *wig_internal_page_render(const char *resource_path, TmplScope *scope)
{
  g_autoptr(TmplTemplate) tmpl = tmpl_template_new(NULL);
  g_autoptr(GError) error = NULL;
  if (!tmpl_template_parse_resource(tmpl, resource_path, NULL, &error)) {
    g_warning("Failed to parse template '%s': %s", resource_path, error->message);
    return g_strdup(error_page);
  }

  g_autoptr(TmplScope) tmp_scope = NULL;
  if (!scope) {
    tmp_scope = tmpl_scope_new();
    scope = tmp_scope;
  }

  g_autofree char *nonce = g_uuid_string_random();
  tmpl_scope_set_string(scope, "nonce", nonce);

  char *html = tmpl_template_expand_string(tmpl, scope, &error);
  if (!html) {
    g_warning("Failed to expand template '%s': %s", resource_path, error->message);
    return g_strdup(error_page);
  }

  return html;
}

void wig_internal_page_finish_request(WebKitURISchemeRequest *request, char *html)
{
  g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_data(html, -1, g_free);
  g_autoptr(WebKitURISchemeResponse) response = webkit_uri_scheme_response_new(stream, -1);
  webkit_uri_scheme_response_set_content_type(response, "text/html; charset=utf-8");

  g_autoptr(SoupMessageHeaders) headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
  soup_message_headers_append(headers, "Cache-Control", "no-store");
  webkit_uri_scheme_response_set_http_headers(response, g_steal_pointer(&headers));

  webkit_uri_scheme_request_finish_with_response(request, response);
}

/**
 * wig_internal_page_html_escape:
 * @text: (nullable): the text to escape
 *
 * Escapes @text for interpolation into an internal page's HTML.
 *
 * Returns: (transfer full): the escaped text, empty if @text was %NULL
 */
char *wig_internal_page_html_escape(const char *text)
{
  return g_markup_escape_text(text ? text : "", -1);
}

typedef struct {
  WebKitURISchemeRequest *request;
  GInputStream *body;
  GMemoryOutputStream *buffer;
  WigFormBodyReadyFunc callback;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
} FormBodyState;

static void form_body_state_free(FormBodyState *state)
{
  g_object_unref(state->request);
  g_object_unref(state->body);
  g_object_unref(state->buffer);
  if (state->user_data_destroy)
    state->user_data_destroy(state->user_data);
  g_free(state);
}

static void on_form_body_read(GObject *source, GAsyncResult *res, gpointer user_data)
{
  FormBodyState *state = user_data;
  GError *error = NULL;
  g_autoptr(GHashTable) params = NULL;
  if (g_output_stream_splice_finish(G_OUTPUT_STREAM(source), res, &error)) {
    gsize size = g_memory_output_stream_get_data_size(state->buffer);
    g_autofree char *body = g_strndup(g_memory_output_stream_get_data(state->buffer), size);
    params = g_uri_parse_params(body, -1, "&", G_URI_PARAMS_WWW_FORM, NULL);
  } else {
    g_warning("internal-page: failed to read request body: %s", error->message);
    g_clear_error(&error);
  }

  state->callback(state->request, params, state->user_data);
  form_body_state_free(state);
}

gboolean wig_internal_page_read_form_body(WebKitURISchemeRequest *request, WigFormBodyReadyFunc callback,
                                          gpointer user_data, GDestroyNotify user_data_destroy)
{
  const char *method = webkit_uri_scheme_request_get_http_method(request);
  g_autoptr(GInputStream) body = webkit_uri_scheme_request_get_http_body(request);

  if (g_strcmp0(method, "POST") != 0 || !body) {
    if (user_data_destroy)
      user_data_destroy(user_data);
    return FALSE;
  }

  FormBodyState *state = g_new0(FormBodyState, 1);
  state->request = g_object_ref(request);
  state->body = g_steal_pointer(&body);
  state->buffer = G_MEMORY_OUTPUT_STREAM(g_memory_output_stream_new_resizable());
  state->callback = callback;
  state->user_data = user_data;
  state->user_data_destroy = user_data_destroy;
  g_output_stream_splice_async(G_OUTPUT_STREAM(state->buffer), state->body,
                               G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                               G_PRIORITY_DEFAULT, NULL, on_form_body_read, state);
  return TRUE;
}
