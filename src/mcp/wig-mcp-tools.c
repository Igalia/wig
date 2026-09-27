/* SPDX-License-Identifier: MIT */

#include <math.h>
#include <string.h>

#include <cairo.h>
#include <wpe/wpe-platform.h>

#include "wig-application.h"
#include "wig-mcp-json.h"
#include "wig-mcp-navigation.h"
#include "wig-mcp-network.h"
#include "wig-mcp-result.h"
#include "wig-mcp-scripts.h"
#include "wig-mcp-tool-table.h"
#include "wig-mcp-tools.h"
#include "wig-tab-list.h"
#include "wig-tab.h"
#include "wig-window.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(cairo_surface_t, cairo_surface_destroy)

static WigWindow *ensure_window(WigMcpServer *self)
{
  WigWindow *window = WIG_WINDOW(gtk_application_get_active_window(GTK_APPLICATION(self->application)));
  if (window)
    return window;

  GList *windows = gtk_application_get_windows(GTK_APPLICATION(self->application));
  if (windows)
    return WIG_WINDOW(windows->data);

  return wig_window_new(self->application);
}

static WigTab *create_tab(WigMcpServer *self, WigWindow *window, const char *uri)
{
  gboolean present_after_creation = !gtk_widget_get_visible(GTK_WIDGET(window));
  g_autoptr(WebKitWebView) web_view = wig_application_create_web_view(self->application);
  wig_window_add_web_view(window, web_view);

  WigTabList *list = wig_window_get_tab_list(window);
  WigTab *tab = wig_tab_list_get_nth(list, wig_tab_list_get_n_tabs(list) - 1);
  wig_tab_list_set_active(list, tab);
  wig_application_suppress_next_history_navigation(self->application, web_view);
  webkit_web_view_load_uri(web_view, uri);
  if (present_after_creation)
    gtk_window_present(GTK_WINDOW(window));
  return tab;
}

static WigTab *find_tab(WigMcpServer *self, JsonObject *params, WigWindow **out_window, char **error)
{
  guint64 requested = 0;
  gboolean has_requested = wig_mcp_json_get_uint_member(params, "tab_handle", &requested);
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(self->application));

  if (!has_requested) {
    WigWindow *window = ensure_window(self);
    WigTab *tab = wig_tab_list_get_active(wig_window_get_tab_list(window));
    if (!tab)
      tab = create_tab(self, window, "about:blank");
    if (out_window)
      *out_window = window;
    return tab;
  }

  if (requested == 0 || requested > G_MAXUINT) {
    *error = g_strdup("Invalid tab_handle");
    return NULL;
  }
  for (GList *l = windows; l; l = l->next) {
    WigWindow *window = WIG_WINDOW(l->data);
    WigTab *tab = wig_tab_list_get_by_id(wig_window_get_tab_list(window), (guint)requested);
    if (tab) {
      if (out_window)
        *out_window = window;
      return tab;
    }
  }
  *error = g_strdup_printf("Tab handle %" G_GUINT64_FORMAT " was not found", requested);
  return NULL;
}

static JsonNode *page_info_node(WigTab *tab)
{
  WebKitWebView *web_view = wig_tab_get_web_view(tab);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "tab_handle");
  json_builder_add_int_value(builder, wig_tab_get_id(tab));
  wig_mcp_json_add_nullable_string(builder, "url", wig_tab_get_uri(tab));
  wig_mcp_json_add_nullable_string(builder, "title", wig_tab_get_title(tab));
  json_builder_set_member_name(builder, "loading");
  json_builder_add_boolean_value(builder, web_view && webkit_web_view_is_loading(web_view));
  json_builder_set_member_name(builder, "progress");
  json_builder_add_double_value(builder, web_view ? webkit_web_view_get_estimated_load_progress(web_view) : 0.0);
  json_builder_end_object(builder);

  return wig_mcp_json_builder_take_root(builder);
}

static char *page_info_json(WigTab *tab)
{
  g_autoptr(JsonNode) node = page_info_node(tab);
  return wig_mcp_json_node_to_string(node);
}

static void add_network_summary(JsonBuilder *builder, NetworkRecord *record)
{
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "request_id");
  json_builder_add_int_value(builder, (gint64)record->id);
  json_builder_set_member_name(builder, "navigation");
  json_builder_add_int_value(builder, (gint64)record->generation);
  wig_mcp_json_add_nullable_string(builder, "url", record->url);
  wig_mcp_json_add_nullable_string(builder, "method", record->method);

  json_builder_set_member_name(builder, "status");
  if (record->status)
    json_builder_add_int_value(builder, record->status);
  else
    json_builder_add_null_value(builder);

  wig_mcp_json_add_nullable_string(builder, "mime_type", record->mime_type);

  json_builder_set_member_name(builder, "start");
  json_builder_add_double_value(builder, (double)record->start_us / 1000.0);

  json_builder_set_member_name(builder, "end");
  if (record->end_us)
    json_builder_add_double_value(builder, (double)record->end_us / 1000.0);
  else
    json_builder_add_null_value(builder);

  json_builder_set_member_name(builder, "duration");
  if (record->end_us)
    json_builder_add_double_value(builder, (double)(record->end_us - record->start_us) / 1000.0);
  else
    json_builder_add_null_value(builder);

  wig_mcp_json_add_nullable_string(builder, "error", record->error);
  json_builder_end_object(builder);
}

static gboolean mime_is_textual(const char *mime)
{
  return mime
      && (g_str_has_prefix(mime, "text/") || strstr(mime, "json") || strstr(mime, "javascript") || strstr(mime, "xml")
          || strstr(mime, "svg"));
}

typedef struct {
  WigMcpCall *call;
  WebKitWebResource *resource;
  guint64 id;
  char *url;
  char *method;
  char *mime_type;
  char *request_headers;
  char *response_headers;
  char *record_error;
  guint status;
  gint64 start_us;
  gint64 end_us;
} NetworkBodyCall;

static void network_body_call_free(NetworkBodyCall *body_call)
{
  g_object_unref(body_call->resource);
  g_free(body_call->url);
  g_free(body_call->method);
  g_free(body_call->mime_type);
  g_free(body_call->request_headers);
  g_free(body_call->response_headers);
  g_free(body_call->record_error);
  g_free(body_call);
}

static void add_network_body_summary(JsonBuilder *builder, NetworkBodyCall *body_call)
{
  NetworkRecord record = {
    .id = body_call->id,
    .url = body_call->url,
    .method = body_call->method,
    .mime_type = body_call->mime_type,
    .error = body_call->record_error,
    .status = body_call->status,
    .start_us = body_call->start_us,
    .end_us = body_call->end_us,
  };
  add_network_summary(builder, &record);
}

static void on_network_body_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  NetworkBodyCall *body_call = user_data;
  g_autoptr(GError) error = NULL;
  gsize length = 0;
  g_autofree guchar *data = webkit_web_resource_get_data_finish(WEBKIT_WEB_RESOURCE(source), result, &length, &error);
  if (!data) {
    wig_mcp_call_finish_text(body_call->call, error->message, TRUE);
    network_body_call_free(body_call);
    return;
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "request");
  add_network_body_summary(builder, body_call);

  json_builder_set_member_name(builder, "request_headers");
  g_autoptr(JsonNode) request_headers = wig_mcp_json_parse(body_call->request_headers ? body_call->request_headers
                                                                                      : "{}");
  json_builder_add_value(builder, g_steal_pointer(&request_headers));

  json_builder_set_member_name(builder, "response_headers");
  g_autoptr(JsonNode) response_headers = wig_mcp_json_parse(body_call->response_headers ? body_call->response_headers
                                                                                        : "{}");
  json_builder_add_value(builder, g_steal_pointer(&response_headers));

  json_builder_set_member_name(builder, "request_body");
  json_builder_add_null_value(builder);
  json_builder_set_member_name(builder, "request_body_available");
  json_builder_add_boolean_value(builder, FALSE);
  json_builder_set_member_name(builder, "request_body_note");
  json_builder_add_string_value(builder, "WebKit does not expose upload request bodies");

  gsize output_length = MIN(length, (gsize)MCP_MAX_NETWORK_BODY_BYTES);
  json_builder_set_member_name(builder, "response_body");
  if (mime_is_textual(body_call->mime_type)) {
    g_autofree char *text = g_utf8_make_valid((const char *)data, (gssize)output_length);
    json_builder_add_string_value(builder, text);
    json_builder_set_member_name(builder, "encoding");
    json_builder_add_string_value(builder, "text");
  } else {
    g_autofree char *encoded = g_base64_encode(data, output_length);
    json_builder_add_string_value(builder, encoded);
    json_builder_set_member_name(builder, "encoding");
    json_builder_add_string_value(builder, "base64");
  }

  json_builder_set_member_name(builder, "response_body_bytes");
  json_builder_add_int_value(builder, (gint64)length);
  json_builder_set_member_name(builder, "response_body_truncated");
  json_builder_add_boolean_value(builder, output_length != length);
  json_builder_end_object(builder);

  g_autoptr(JsonNode) detail = wig_mcp_json_builder_take_root(builder);
  g_autofree char *json = wig_mcp_json_node_to_string(detail);
  wig_mcp_call_finish_text(body_call->call, json, FALSE);
  network_body_call_free(body_call);
}

/* JS_RESULT_INTERACTION is JSON whose "content" member, when present, holds a
 * rendered page tree that is split out into its own text block. */
typedef enum { JS_RESULT_JSON, JS_RESULT_STRING, JS_RESULT_INTERACTION, JS_RESULT_CONSOLE } JsResultMode;

typedef struct {
  WigMcpServer *server;
  WigMcpCall *call;
  WebKitWebView *web_view; /* weak, only compared */
  WigApplication *application;
  JsResultMode mode;
  gboolean error_if_not_ok;
  gboolean suppress_navigation_history;
} JavascriptCall;

static void javascript_call_free(JavascriptCall *js_call)
{
  js_call->server->javascript_calls = g_list_remove(js_call->server->javascript_calls, js_call);
  wig_mcp_server_unref(js_call->server);
  g_clear_object(&js_call->application);
  g_free(js_call);
}

static const char *dialog_type_name(WebKitScriptDialogType type);

void wig_mcp_tools_dialog_opened(WigMcpServer *self, WebKitWebView *web_view, PendingDialog *pending)
{
  WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(pending->dialog);
  g_autofree char *message = g_strdup_printf(
      "The page opened a %s dialog (dialog_id %" G_GUINT64_FORMAT ") saying \"%s\" and is paused until it is "
      "answered with browser_dialogs.",
      dialog_type_name(type), pending->id, webkit_script_dialog_get_message(pending->dialog));

  for (GList *l = self->javascript_calls; l; l = l->next) {
    JavascriptCall *js_call = l->data;
    if (js_call->web_view != web_view || !js_call->call)
      continue;

    g_debug("mcp: a %s dialog interrupted a call on its page", dialog_type_name(type));
    wig_mcp_call_finish_text(g_steal_pointer(&js_call->call), message, FALSE);
  }

  wig_mcp_navigation_dialog_opened(self, web_view, message);
}

static void finish_javascript_value(JavascriptCall *js_call, WebKitWebView *web_view, JSCValue *value, GError *error)
{
  if (js_call->suppress_navigation_history)
    wig_application_cancel_history_navigation_suppression(js_call->application, web_view);
  if (!js_call->call) {
    javascript_call_free(js_call);
    return;
  }
  if (!value) {
    wig_mcp_call_finish_text(js_call->call, error->message, TRUE);
    javascript_call_free(js_call);
    return;
  }

  g_autofree char *output = js_call->mode == JS_RESULT_STRING ? jsc_value_to_string(value)
                                                              : jsc_value_to_json(value, 0);
  if (!output) {
    JSCException *exception = jsc_context_get_exception(jsc_value_get_context(value));
    if (exception) {
      wig_mcp_call_finish_text(js_call->call, jsc_exception_get_message(exception), TRUE);
      javascript_call_free(js_call);
      return;
    }
    output = g_strdup("null");
  }

  if (js_call->mode == JS_RESULT_STRING) {
    output = wig_mcp_result_truncate_text(g_steal_pointer(&output));
  } else if (strlen(output) > MCP_MAX_TEXT_RESULT_BYTES) {
    /* A JSON result has to stay parseable, so say it was too large rather than
     * handing back a fragment. */
    g_autofree char *message = g_strdup_printf("Result is %" G_GSIZE_FORMAT
                                               " bytes, over the %d byte limit. Return less from the script.",
                                               strlen(output), MCP_MAX_TEXT_RESULT_BYTES);
    wig_mcp_call_finish_text(js_call->call, message, TRUE);
    javascript_call_free(js_call);
    return;
  }

  if (js_call->mode == JS_RESULT_CONSOLE) {
    g_autoptr(JsonNode) output_node = wig_mcp_json_parse(output);
    JsonObject *object = output_node && JSON_NODE_HOLDS_OBJECT(output_node) ? json_node_get_object(output_node) : NULL;
    JsonNode *cleared = object ? json_object_get_member(object, "cleared") : NULL;
    if (cleared && JSON_NODE_HOLDS_ARRAY(cleared)) {
      ViewState *state = wig_mcp_network_get_view_state(js_call->call->server, web_view);
      JsonArray *ids = json_node_get_array(cleared);
      for (guint i = 0; state && i < json_array_get_length(ids); i++)
        wig_mcp_network_remove_console_message(state, (guint64)json_array_get_int_element(ids, i));
      json_object_remove_member(object, "cleared");
      g_clear_pointer(&output, g_free);
      output = wig_mcp_json_node_to_string(output_node);
    }
  }

  gboolean is_error = FALSE;
  g_autofree char *page = NULL;
  if (js_call->error_if_not_ok || js_call->mode == JS_RESULT_INTERACTION) {
    g_autoptr(JsonNode) output_node = wig_mcp_json_parse(output);
    if (output_node && JSON_NODE_HOLDS_OBJECT(output_node)) {
      JsonObject *object = json_node_get_object(output_node);
      JsonNode *ok = json_object_get_member(object, "ok");
      is_error = js_call->error_if_not_ok && ok && JSON_NODE_HOLDS_VALUE(ok)
          && json_node_get_value_type(ok) == G_TYPE_BOOLEAN && !json_node_get_boolean(ok);

      /* Lift the page tree out of the JSON so it can be its own block, and
       * re-serialize what is left as the outcome summary. */
      if (js_call->mode == JS_RESULT_INTERACTION && json_object_has_member(object, "content")) {
        page = g_strdup(json_object_get_string_member(object, "content"));
        json_object_remove_member(object, "content");
        g_clear_pointer(&output, g_free);
        output = wig_mcp_json_node_to_string(output_node);
      }
    }
  }

  if (page)
    wig_mcp_call_finish_node_take(js_call->call, wig_mcp_result_text_pair_new(output, page, is_error));
  else
    wig_mcp_call_finish_text(js_call->call, output, is_error);
  javascript_call_free(js_call);
}

static void on_javascript_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  WebKitWebView *web_view = WEBKIT_WEB_VIEW(source);
  g_autoptr(GError) error = NULL;
  g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(web_view, result, &error);
  finish_javascript_value(user_data, web_view, value, error);
}

static void on_javascript_function_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  WebKitWebView *web_view = WEBKIT_WEB_VIEW(source);
  g_autoptr(GError) error = NULL;
  g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(web_view, result, &error);
  finish_javascript_value(user_data, web_view, value, error);
}

static JavascriptCall *javascript_call_new(WigMcpServer *self, McpCall *protocol_call, WebKitWebView *web_view,
                                           JsResultMode mode, gboolean error_if_not_ok,
                                           gboolean suppress_navigation_history)
{
  JavascriptCall *js_call = g_new0(JavascriptCall, 1);
  js_call->server = wig_mcp_server_ref(self);
  js_call->call = wig_mcp_call_new(self, protocol_call, web_view);
  js_call->web_view = web_view;
  self->javascript_calls = g_list_prepend(self->javascript_calls, js_call);
  if (suppress_navigation_history)
    js_call->application = g_object_ref(self->application);
  js_call->mode = mode;
  js_call->error_if_not_ok = error_if_not_ok;
  js_call->suppress_navigation_history = suppress_navigation_history;
  if (suppress_navigation_history)
    wig_application_suppress_next_history_navigation(self->application, web_view);

  return js_call;
}

/* Call a function body in the page, passing arguments as data rather than
 * substituting them into the source. `arguments` is a floating a{sv} whose keys
 * become the function's parameter names.
 *
 * The default script world is required, despite exposing these helpers to page
 * scripts: an isolated world's global object is rebuilt for every evaluation,
 * so nothing stored there survives to the next call. Only the default world's
 * global lives as long as the document, which is the lifetime node handles
 * need. */
static void start_javascript_function_call(WigMcpServer *self, McpCall *protocol_call, WebKitWebView *web_view,
                                           const char *body, GVariant *arguments, JsResultMode mode,
                                           gboolean error_if_not_ok, gboolean suppress_navigation_history)
{
  JavascriptCall *js_call = javascript_call_new(self, protocol_call, web_view, mode, error_if_not_ok,
                                                suppress_navigation_history);

  GCancellable *cancellable = mcp_call_get_cancellable(protocol_call);
  webkit_web_view_call_async_javascript_function(web_view, body, -1, arguments, NULL, "wig-mcp://tool", cancellable,
                                                 on_javascript_function_ready, js_call);
}

static void start_javascript_call(WigMcpServer *self, McpCall *protocol_call, WebKitWebView *web_view,
                                  const char *script, JsResultMode mode, gboolean error_if_not_ok,
                                  gboolean suppress_navigation_history)
{
  JavascriptCall *js_call = javascript_call_new(self, protocol_call, web_view, mode, error_if_not_ok,
                                                suppress_navigation_history);

  GCancellable *cancellable = mcp_call_get_cancellable(protocol_call);
  webkit_web_view_evaluate_javascript(web_view, script, -1, NULL, "wig-mcp://tool", cancellable, on_javascript_ready,
                                      js_call);
}

typedef struct {
  GByteArray *bytes;
} PngBuffer;

static cairo_status_t write_png_bytes(void *closure, const unsigned char *data, unsigned int length)
{
  PngBuffer *buffer = closure;
  g_byte_array_append(buffer->bytes, data, length);
  return CAIRO_STATUS_SUCCESS;
}

static void on_snapshot_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  WigMcpCall *call = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(WebKitImage) image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
  if (!image) {
    wig_mcp_call_finish_text(call, error->message, TRUE);
    return;
  }

  GBytes *pixels = webkit_image_as_bytes(image);
  gsize length = 0;
  const guint8 *data = g_bytes_get_data(pixels, &length);
  int width = webkit_image_get_width(image);
  int height = webkit_image_get_height(image);
  guint stride = webkit_image_get_stride(image);
  if (length < (gsize)stride * (gsize)height) {
    wig_mcp_call_finish_text(call, "Snapshot returned an invalid pixel buffer", TRUE);
    return;
  }

  g_autofree guint8 *mutable_data = g_memdup2(data, length);
  g_autoptr(cairo_surface_t) surface = cairo_image_surface_create_for_data(mutable_data, CAIRO_FORMAT_ARGB32, width,
                                                                           height, (int)stride);
  PngBuffer buffer = { g_byte_array_new() };
  cairo_status_t status = cairo_surface_write_to_png_stream(surface, write_png_bytes, &buffer);
  if (status != CAIRO_STATUS_SUCCESS) {
    g_byte_array_unref(buffer.bytes);
    wig_mcp_call_finish_text(call, cairo_status_to_string(status), TRUE);
    return;
  }

  gsize png_length = buffer.bytes->len;
  g_autofree char *encoded = g_base64_encode(buffer.bytes->data, buffer.bytes->len);
  g_byte_array_unref(buffer.bytes);

  /* Images are inlined as base64, so an oversized capture would be delivered
   * whole or not at all. Refuse it and say how large it was: a full-document
   * capture of a long page is the usual cause. */
  if (strlen(encoded) > MCP_MAX_SCREENSHOT_BYTES) {
    g_autofree char *message = g_strdup_printf(
        "Screenshot is %" G_GSIZE_FORMAT " KiB of PNG (%" G_GSIZE_FORMAT
        " KiB encoded), over the %d KiB limit. Capture the viewport instead of the full document, or reduce the "
        "viewport size.",
        png_length / 1024, strlen(encoded) / 1024, MCP_MAX_SCREENSHOT_BYTES / 1024);
    wig_mcp_call_finish_text(call, message, TRUE);
    return;
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "content");
  json_builder_begin_array(builder);
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "image");
  json_builder_set_member_name(builder, "data");
  json_builder_add_string_value(builder, encoded);
  json_builder_set_member_name(builder, "mimeType");
  json_builder_add_string_value(builder, "image/png");
  json_builder_end_object(builder);
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  wig_mcp_call_finish_node_take(call, wig_mcp_json_builder_take_root(builder));
}

static const char *dialog_type_name(WebKitScriptDialogType type)
{
  switch (type) {
  case WEBKIT_SCRIPT_DIALOG_ALERT:
    return "alert";
  case WEBKIT_SCRIPT_DIALOG_CONFIRM:
    return "confirm";
  case WEBKIT_SCRIPT_DIALOG_PROMPT:
    return "prompt";
  case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
    return "beforeunload";
  }
  return "unknown";
}

static gboolean get_optional_boolean(JsonObject *arguments, const char *name, gboolean fallback, gboolean *value)
{
  *value = fallback;
  if (!arguments || !json_object_has_member(arguments, name))
    return TRUE;

  JsonNode *node = json_object_get_member(arguments, name);
  if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    return FALSE;

  *value = json_node_get_boolean(node);
  return TRUE;
}

static gboolean get_optional_number(JsonObject *arguments, const char *name, double fallback, double *value)
{
  *value = fallback;
  if (!arguments || !json_object_has_member(arguments, name))
    return TRUE;

  JsonNode *node = json_object_get_member(arguments, name);
  if (!JSON_NODE_HOLDS_VALUE(node))
    return FALSE;

  GType type = json_node_get_value_type(node);
  if (type == G_TYPE_INT64)
    *value = (double)json_node_get_int(node);
  else if (type == G_TYPE_DOUBLE)
    *value = json_node_get_double(node);
  else
    return FALSE;

  return isfinite(*value);
}

/* Extract the page as an indented node tree. Options travel as a{sv} function
 * arguments, and booleans are sent as 0/1 numbers because
 * webkit_web_view_call_async_javascript_function() only accepts numbers,
 * strings, and dictionaries. */
static void start_tree_extraction(WigMcpServer *self, McpCall *call, WebKitWebView *web_view, JsonObject *arguments)
{
  const char *region = wig_mcp_json_get_string_member(arguments, "region");
  if (!region)
    region = "viewport";
  if (!g_str_equal(region, "viewport") && !g_str_equal(region, "document")) {
    wig_mcp_result_send_text(call, "region must be viewport or document", TRUE);
    return;
  }

  double max_nodes = 0;
  if (!get_optional_number(arguments, "max_nodes", MCP_TREE_MAX_NODES, &max_nodes) || max_nodes < 1
      || max_nodes > 20000) {
    wig_mcp_result_send_text(call, "max_nodes must be a number from 1 through 20000", TRUE);
    return;
  }

  double max_words = 0;
  if (!get_optional_number(arguments, "max_words_per_paragraph", MCP_TREE_MAX_WORDS_PER_PARAGRAPH, &max_words)
      || max_words < 1 || max_words > 2000) {
    wig_mcp_result_send_text(call, "max_words_per_paragraph must be a number from 1 through 2000", TRUE);
    return;
  }

  gboolean include_containers = FALSE;
  if (!get_optional_boolean(arguments, "include_containers", FALSE, &include_containers)) {
    wig_mcp_result_send_text(call, "include_containers must be a boolean", TRUE);
    return;
  }

  ViewState *state = wig_mcp_network_get_view_state(self, web_view);
  guint64 epoch = state ? state->committed_navigation_generation : 0;

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "epoch", g_variant_new_double((double)epoch));
  g_variant_builder_add(&options, "{sv}", "region", g_variant_new_string(region));
  g_variant_builder_add(&options, "{sv}", "maxNodes", g_variant_new_double(max_nodes));
  g_variant_builder_add(&options, "{sv}", "maxWords", g_variant_new_double(max_words));
  g_variant_builder_add(&options, "{sv}", "maxBytes", g_variant_new_double((double)MCP_TREE_MAX_BYTES));
  g_variant_builder_add(&options, "{sv}", "includeContainers", g_variant_new_double(include_containers ? 1 : 0));

  GVariantBuilder args;
  g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&args, "{sv}", "options", g_variant_builder_end(&options));

  g_autofree char *body = wig_mcp_scripts_tree();
  start_javascript_function_call(self, call, web_view, body, g_variant_builder_end(&args), JS_RESULT_STRING, FALSE,
                                 FALSE);
}

typedef struct {
  const char *url_substring; /* already lowercased */
  const char *method; /* already lowercased */
  gint64 status_min;
  gint64 status_max;
  gboolean has_status;
  double since_ms;
} NetworkFilter;

static gboolean network_record_matches(NetworkRecord *record, const NetworkFilter *filter)
{
  if (filter->since_ms > 0 && (double)record->start_us / 1000.0 < filter->since_ms)
    return FALSE;

  /* A record with no status yet has not had a response, so it cannot satisfy a
   * status range rather than counting as status 0. */
  if (filter->has_status
      && (record->status == 0 || (gint64)record->status < filter->status_min
          || (gint64)record->status > filter->status_max))
    return FALSE;

  if (filter->method) {
    if (!record->method)
      return FALSE;
    g_autofree char *method = g_ascii_strdown(record->method, -1);
    if (!g_str_equal(method, filter->method))
      return FALSE;
  }

  if (filter->url_substring) {
    if (!record->url)
      return FALSE;
    g_autofree char *url = g_ascii_strdown(record->url, -1);
    if (!strstr(url, filter->url_substring))
      return FALSE;
  }

  return TRUE;
}

static gboolean is_internal_uri(const char *uri)
{
  return uri && g_ascii_strncasecmp(uri, "wig:", 4) == 0;
}

typedef struct {
  WigMcpServer *server;
  McpCall *call;
  WebKitWebView *web_view;
  gulong load_changed_id;
  guint timeout_id;
} DeferredCall;

static void dispatch_tool_call(WigMcpServer *self, McpCall *call, JsonObject *params);

static void deferred_call_resume(DeferredCall *deferred)
{
  g_clear_signal_handler(&deferred->load_changed_id, deferred->web_view);
  g_clear_handle_id(&deferred->timeout_id, g_source_remove);

  if (deferred->server->stopped || mcp_call_is_completed(deferred->call)
      || g_cancellable_is_cancelled(mcp_call_get_cancellable(deferred->call)))
    g_debug("mcp: dropping a call that was waiting for its tab to load");
  else
    dispatch_tool_call(deferred->server, deferred->call, mcp_call_get_params(deferred->call));

  g_object_unref(deferred->web_view);
  mcp_call_unref(deferred->call);
  wig_mcp_server_unref(deferred->server);
  g_free(deferred);
}

static void on_deferred_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, DeferredCall *deferred)
{
  if (load_event == WEBKIT_LOAD_FINISHED)
    deferred_call_resume(deferred);
}

static gboolean on_deferred_timeout(gpointer user_data)
{
  DeferredCall *deferred = user_data;

  deferred->timeout_id = 0;
  g_debug("mcp: the tab did not finish loading in time, running the call anyway");
  deferred_call_resume(deferred);
  return G_SOURCE_REMOVE;
}

static void defer_until_loaded(WigMcpServer *self, McpCall *call, WebKitWebView *web_view)
{
  DeferredCall *deferred = g_new0(DeferredCall, 1);
  deferred->server = wig_mcp_server_ref(self);
  deferred->call = mcp_call_ref(call);
  deferred->web_view = g_object_ref(web_view);
  deferred->load_changed_id = g_signal_connect(web_view, "load-changed", G_CALLBACK(on_deferred_load_changed),
                                               deferred);
  deferred->timeout_id = g_timeout_add_seconds(MCP_NAVIGATION_TIMEOUT_SECONDS, on_deferred_timeout, deferred);
}

static void dispatch_tool_call(WigMcpServer *self, McpCall *call, JsonObject *params)
{
  const char *name = wig_mcp_json_get_string_member(params, "name");
  JsonObject *arguments = NULL;
  gboolean invalid_arguments = FALSE;
  if (params && json_object_has_member(params, "arguments")) {
    JsonNode *arguments_node = json_object_get_member(params, "arguments");
    if (JSON_NODE_HOLDS_OBJECT(arguments_node))
      arguments = json_node_get_object(arguments_node);
    else
      invalid_arguments = TRUE;
  }

  if (!name) {
    mcp_call_return_error(call, -32602, "tools/call requires a tool name", NULL);
    return;
  }
  if (!wig_mcp_tool_is_known(name)) {
    g_autofree char *text = g_strdup_printf("Unknown tool: %s", name);
    mcp_call_return_error(call, -32602, text, NULL);
    return;
  }
  if (invalid_arguments) {
    mcp_call_return_error(call, -32602, "Tool arguments must be an object", NULL);
    return;
  }
  if (wig_mcp_tool_reject_invalid_arguments(call, name, arguments))
    return;

  if (g_str_equal(name, "list_tabs")) {
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_array(builder);
    for (GList *l = gtk_application_get_windows(GTK_APPLICATION(self->application)); l; l = l->next) {
      WigWindow *window = WIG_WINDOW(l->data);
      WigTabList *list = wig_window_get_tab_list(window);
      WigTab *active = wig_tab_list_get_active(list);
      for (guint i = 0; i < wig_tab_list_get_n_tabs(list); i++) {
        WigTab *tab = wig_tab_list_get_nth(list, i);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "tab_handle");
        json_builder_add_int_value(builder, wig_tab_get_id(tab));
        json_builder_set_member_name(builder, "window_handle");
        json_builder_add_int_value(builder, wig_window_base_get_id(WIG_WINDOW_BASE(window)));
        wig_mcp_json_add_nullable_string(builder, "url", wig_tab_get_uri(tab));
        wig_mcp_json_add_nullable_string(builder, "title", wig_tab_get_title(tab));
        json_builder_set_member_name(builder, "active");
        json_builder_add_boolean_value(builder, tab == active);
        json_builder_end_object(builder);
      }
    }
    json_builder_end_array(builder);

    g_autoptr(JsonNode) node = wig_mcp_json_builder_take_root(builder);
    g_autofree char *json = wig_mcp_json_node_to_string(node);
    wig_mcp_result_send_text(call, json, FALSE);
    return;
  }

  if (g_str_equal(name, "create_tab")) {
    WigWindow *window = ensure_window(self);
    const char *url = wig_mcp_json_get_string_member(arguments, "url");
    if (is_internal_uri(url)) {
      wig_mcp_result_send_text(call, "Internal Wig pages are not available to MCP clients", TRUE);
      return;
    }
    WigTab *tab = create_tab(self, window, url ? url : "about:blank");

    g_autofree char *json = page_info_json(tab);
    wig_mcp_result_send_text(call, json, FALSE);
    return;
  }

  g_autofree char *tab_error = NULL;
  WigWindow *window = NULL;
  WigTab *tab = find_tab(self, arguments, &window, &tab_error);
  if (!tab) {
    wig_mcp_result_send_text(call, tab_error, TRUE);
    return;
  }
  gboolean internal_page = is_internal_uri(wig_tab_get_uri(tab));
  gboolean needs_page = !g_str_equal(name, "page_info") && !g_str_equal(name, "switch_tab")
      && !g_str_equal(name, "close_tab");
  if (internal_page && needs_page) {
    wig_mcp_result_send_text(call, "Internal Wig pages are not available to MCP clients", TRUE);
    return;
  }

  if (needs_page && wig_tab_get_discarded(tab)) {
    wig_tab_load_discarded(tab);
    WebKitWebView *loading_view = wig_tab_get_web_view(tab);
    if (loading_view && webkit_web_view_is_loading(loading_view)) {
      g_debug("mcp: loading discarded tab %u before running %s", wig_tab_get_id(tab), name);
      defer_until_loaded(self, call, loading_view);
      return;
    }
  }
  WebKitWebView *web_view = wig_tab_get_web_view(tab);

  if (g_str_equal(name, "page_info")) {
    g_autofree char *json = page_info_json(tab);
    wig_mcp_result_send_text(call, json, FALSE);
  } else if (g_str_equal(name, "switch_tab")) {
    wig_tab_list_set_active(wig_window_get_tab_list(window), tab);
    gtk_window_present(GTK_WINDOW(window));
    g_autofree char *json = page_info_json(tab);
    wig_mcp_result_send_text(call, json, FALSE);
  } else if (g_str_equal(name, "close_tab")) {
    guint handle = wig_tab_get_id(tab);
    wig_tab_list_close(wig_window_get_tab_list(window), tab);
    g_autofree char *text = g_strdup_printf("{\"closed\":true,\"tab_handle\":%u}", handle);
    wig_mcp_result_send_text(call, text, FALSE);
  } else if (g_str_equal(name, "evaluate_javascript")) {
    const char *script = wig_mcp_json_get_string_member(arguments, "script");
    if (!script)
      wig_mcp_result_send_text(call, "script is required", TRUE);
    else
      start_javascript_call(self, call, web_view, script, JS_RESULT_JSON, FALSE, TRUE);
  } else if (g_str_equal(name, "get_page_content")) {
    const char *format = wig_mcp_json_get_string_member(arguments, "format");
    if (!format)
      format = "text";
    if (g_str_equal(format, "textTree")) {
      start_tree_extraction(self, call, web_view, arguments);
    } else if (!g_str_equal(format, "text") && !g_str_equal(format, "html") && !g_str_equal(format, "markdown")
               && !g_str_equal(format, "json")) {
      wig_mcp_result_send_text(call, "format must be textTree, text, html, markdown, or json", TRUE);
    } else {
      g_autofree char *script = wig_mcp_scripts_content(format);
      start_javascript_call(self, call, web_view, script, JS_RESULT_STRING, FALSE, FALSE);
    }
  } else if (g_str_equal(name, "browser_console_messages")) {
    gboolean clear = FALSE;
    if (!get_optional_boolean(arguments, "clear", FALSE, &clear)) {
      wig_mcp_result_send_text(call, "clear must be a boolean", TRUE);
      return;
    }

    double limit = 0;
    if (!get_optional_number(arguments, "limit", 100, &limit) || limit < 1 || limit > 1000) {
      wig_mcp_result_send_text(call, "limit must be a number from 1 through 1000", TRUE);
      return;
    }

    g_autoptr(GString) levels = g_string_new(NULL);
    if (arguments && json_object_has_member(arguments, "level_filter")) {
      JsonNode *node = json_object_get_member(arguments, "level_filter");
      if (!JSON_NODE_HOLDS_ARRAY(node)) {
        wig_mcp_result_send_text(call, "level_filter must be an array", TRUE);
        return;
      }

      JsonArray *array = json_node_get_array(node);
      for (guint i = 0; i < json_array_get_length(array); i++) {
        const char *level = json_array_get_string_element(array, i);
        if (!level || !g_strv_contains((const char *[]) { "debug", "log", "info", "warn", "error", NULL }, level)) {
          wig_mcp_result_send_text(call, "level_filter entries must be debug, log, info, warn, or error", TRUE);
          return;
        }
        if (levels->len)
          g_string_append_c(levels, ',');
        g_string_append(levels, level);
      }
    }

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "levels", g_variant_new_string(levels->str));
    g_variant_builder_add(&options, "{sv}", "limit", g_variant_new_double(limit));
    g_variant_builder_add(&options, "{sv}", "clear", g_variant_new_double(clear ? 1 : 0));
    g_autofree char *native = wig_mcp_network_console_messages_json(wig_mcp_network_get_view_state(self, web_view));
    g_variant_builder_add(&options, "{sv}", "native", g_variant_new_string(native));

    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&args, "{sv}", "options", g_variant_builder_end(&options));

    g_autofree char *body = wig_mcp_scripts_console_read();
    start_javascript_function_call(self, call, web_view, body, g_variant_builder_end(&args), JS_RESULT_CONSOLE, FALSE,
                                   FALSE);
  } else if (g_str_equal(name, "page_interactions")) {
    JsonArray *actions = NULL;
    if (arguments && json_object_has_member(arguments, "actions")) {
      JsonNode *node = json_object_get_member(arguments, "actions");
      if (JSON_NODE_HOLDS_ARRAY(node))
        actions = json_node_get_array(node);
    }
    if (!actions) {
      wig_mcp_result_send_text(call, "actions must be an array", TRUE);
    } else {
      const char *return_content = wig_mcp_json_get_string_member(arguments, "return_content");
      if (!return_content)
        return_content = "textTree";
      const char *region = wig_mcp_json_get_string_member(arguments, "region");
      if (!region)
        region = "viewport";
      if (!g_str_equal(return_content, "textTree") && !g_str_equal(return_content, "none")) {
        wig_mcp_result_send_text(call, "return_content must be textTree or none", TRUE);
        return;
      }
      if (!g_str_equal(region, "viewport") && !g_str_equal(region, "document")) {
        wig_mcp_result_send_text(call, "region must be viewport or document", TRUE);
        return;
      }

      g_autoptr(JsonNode) actions_node = json_node_new(JSON_NODE_ARRAY);
      json_node_set_array(actions_node, actions);
      g_autofree char *actions_json = wig_mcp_json_node_to_string(actions_node);

      ViewState *state = wig_mcp_network_get_view_state(self, web_view);

      /* A post-action snapshot is a progress report, not a full extraction, so
       * it gets a smaller budget than get_page_content; a caller who wants the
       * whole tree asks for it directly. */
      GVariantBuilder options;
      g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&options, "{sv}", "epoch",
                            g_variant_new_double((double)(state ? state->committed_navigation_generation : 0)));
      g_variant_builder_add(&options, "{sv}", "returnContent", g_variant_new_string(return_content));
      g_variant_builder_add(&options, "{sv}", "region", g_variant_new_string(region));
      g_variant_builder_add(&options, "{sv}", "maxNodes", g_variant_new_double(MCP_TREE_MAX_NODES));
      g_variant_builder_add(&options, "{sv}", "maxWords", g_variant_new_double(MCP_TREE_MAX_WORDS_PER_PARAGRAPH));
      g_variant_builder_add(&options, "{sv}", "maxBytes", g_variant_new_double(MCP_TREE_INTERACTION_MAX_BYTES));

      GVariantBuilder args;
      g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&args, "{sv}", "actions", g_variant_new_string(actions_json));
      g_variant_builder_add(&args, "{sv}", "options", g_variant_builder_end(&options));

      g_autofree char *body = wig_mcp_scripts_interaction();
      start_javascript_function_call(self, call, web_view, body, g_variant_builder_end(&args), JS_RESULT_INTERACTION,
                                     TRUE, TRUE);
    }
  } else if (g_str_equal(name, "navigate_to_url")) {
    const char *url = wig_mcp_json_get_string_member(arguments, "url");
    if (!url) {
      wig_mcp_result_send_text(call, "url is required", TRUE);
    } else if (is_internal_uri(url)) {
      wig_mcp_result_send_text(call, "Internal Wig pages are not available to MCP clients", TRUE);
    } else {
      double timeout = 0;
      if (!get_optional_number(arguments, "timeout", MCP_NAVIGATION_TIMEOUT_SECONDS, &timeout) || timeout < 0.1
          || timeout > MCP_NAVIGATION_TIMEOUT_SECONDS) {
        wig_mcp_result_send_text(call, "timeout must be a number from 0.1 through 30", TRUE);
        return;
      }

      wig_mcp_navigation_start(self, call, tab, timeout, TRUE, TRUE);
      wig_application_suppress_next_history_navigation(self->application, web_view);
      webkit_web_view_load_uri(web_view, url);
    }
  } else if (g_str_equal(name, "wait_for_navigation")) {
    double timeout = 0;
    if (!get_optional_number(arguments, "timeout", MCP_NAVIGATION_TIMEOUT_SECONDS, &timeout) || timeout < 0.1
        || timeout > MCP_NAVIGATION_TIMEOUT_SECONDS) {
      wig_mcp_result_send_text(call, "timeout must be a number from 0.1 through 30", TRUE);
      return;
    }

    wig_mcp_navigation_start(self, call, tab, timeout, FALSE, FALSE);
  } else if (g_str_equal(name, "screenshot")) {
    gboolean full = FALSE;
    if (!get_optional_boolean(arguments, "full_document", FALSE, &full)) {
      wig_mcp_result_send_text(call, "full_document must be a boolean", TRUE);
      return;
    }

    WigMcpCall *snapshot_call = wig_mcp_call_new(self, call, web_view);
    GCancellable *cancellable = mcp_call_get_cancellable(call);
    webkit_web_view_get_snapshot(web_view, full ? WEBKIT_SNAPSHOT_REGION_FULL_DOCUMENT : WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, cancellable, on_snapshot_ready, snapshot_call);
  } else if (g_str_equal(name, "set_viewport_size")) {
    guint64 width = 0, height = 0;
    if (!wig_mcp_json_get_uint_member(arguments, "width", &width)
        || !wig_mcp_json_get_uint_member(arguments, "height", &height) || width < 1 || height < 1 || width > 16384
        || height > 16384) {
      wig_mcp_result_send_text(call, "width and height must be integers from 1 through 16384", TRUE);
    } else {
      WPEView *view = webkit_web_view_get_wpe_view(web_view);
      WPEToplevel *toplevel = view ? wpe_view_get_toplevel(view) : NULL;
      if (!toplevel) {
        wig_mcp_result_send_text(call, "The tab does not have a platform toplevel", TRUE);
        return;
      }

      int view_width = wpe_view_get_width(view);
      int view_height = wpe_view_get_height(view);
      int toplevel_width = 0;
      int toplevel_height = 0;
      wpe_toplevel_get_size(toplevel, &toplevel_width, &toplevel_height);

      int target_width = view_width > 0 ? toplevel_width + (int)width - view_width : (int)width;
      int target_height = view_height > 0 ? toplevel_height + (int)height - view_height : (int)height;
      gboolean resized = wpe_toplevel_resize(toplevel, MAX(1, target_width), MAX(1, target_height));
      if (!resized)
        gtk_window_set_default_size(GTK_WINDOW(window), MAX(1, target_width), MAX(1, target_height));

      g_autofree char *text = g_strdup_printf("{\"requested_width\":%" G_GUINT64_FORMAT
                                              ",\"requested_height\":%" G_GUINT64_FORMAT
                                              ",\"platform_resize_requested\":%s,\"best_effort\":%s}",
                                              width, height, resized ? "true" : "false", resized ? "false" : "true");
      wig_mcp_result_send_text(call, text, FALSE);
    }
  } else if (g_str_equal(name, "list_network_requests")) {
    NetworkFilter filter = { 0 };
    g_autofree char *url_substring = NULL;
    g_autofree char *method = NULL;

    JsonObject *filter_object = NULL;
    if (arguments && json_object_has_member(arguments, "filter")) {
      JsonNode *node = json_object_get_member(arguments, "filter");
      if (!JSON_NODE_HOLDS_OBJECT(node)) {
        wig_mcp_result_send_text(call, "filter must be an object", TRUE);
        return;
      }
      filter_object = json_node_get_object(node);
    }

    if (filter_object) {
      const char *value = wig_mcp_json_get_string_member(filter_object, "url_substring");
      if (value)
        filter.url_substring = url_substring = g_ascii_strdown(value, -1);
      value = wig_mcp_json_get_string_member(filter_object, "method");
      if (value)
        filter.method = method = g_ascii_strdown(value, -1);

      double status_min = 0;
      double status_max = 0;
      if (!get_optional_number(filter_object, "status_min", 0, &status_min)
          || !get_optional_number(filter_object, "status_max", 999, &status_max)) {
        wig_mcp_result_send_text(call, "status_min and status_max must be numbers", TRUE);
        return;
      }
      filter.has_status = json_object_has_member(filter_object, "status_min")
          || json_object_has_member(filter_object, "status_max");
      filter.status_min = (gint64)status_min;
      filter.status_max = (gint64)status_max;
    }

    if (!get_optional_number(arguments, "since", 0, &filter.since_ms)) {
      wig_mcp_result_send_text(call, "since must be a number matching the start field", TRUE);
      return;
    }

    double limit = 0;
    if (!get_optional_number(arguments, "limit", MCP_MAX_NETWORK_RECORDS, &limit) || limit < 1
        || limit > MCP_MAX_NETWORK_RECORDS) {
      wig_mcp_result_send_text(call, "limit must be a number from 1 through " G_STRINGIFY(MCP_MAX_NETWORK_RECORDS),
                               TRUE);
      return;
    }

    gboolean clear = FALSE;
    if (!get_optional_boolean(arguments, "clear", FALSE, &clear)) {
      wig_mcp_result_send_text(call, "clear must be a boolean", TRUE);
      return;
    }

    ViewState *state = wig_mcp_network_get_view_state(self, web_view);
    g_autoptr(GPtrArray) matched = g_ptr_array_new();
    if (state) {
      for (GList *l = state->records.head; l; l = l->next) {
        if (network_record_matches(l->data, &filter))
          g_ptr_array_add(matched, l->data);
      }
    }

    /* Keep the newest when more matched than asked for: a client chasing a
     * recent request wants the tail, not the head. */
    guint skipped = matched->len > (guint)limit ? matched->len - (guint)limit : 0;

    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "matched");
    json_builder_add_int_value(builder, matched->len);
    json_builder_set_member_name(builder, "returned");
    json_builder_add_int_value(builder, matched->len - skipped);
    json_builder_set_member_name(builder, "requests");
    json_builder_begin_array(builder);
    for (guint i = skipped; i < matched->len; i++)
      add_network_summary(builder, matched->pdata[i]);
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    /* Discard only what was handed over. Dropping the whole buffer would throw
     * away records a filter or limit never showed the caller. */
    if (clear && state) {
      for (guint i = skipped; i < matched->len; i++)
        wig_mcp_network_remove_record(state, matched->pdata[i]);
    }

    g_autoptr(JsonNode) node = wig_mcp_json_builder_take_root(builder);
    g_autofree char *json = wig_mcp_json_node_to_string(node);
    wig_mcp_result_send_text(call, json, FALSE);
  } else if (g_str_equal(name, "get_network_request")) {
    guint64 request_id = 0;
    NetworkRecord *record = wig_mcp_json_get_uint_member(arguments, "request_id", &request_id)
        ? wig_mcp_network_find_record(wig_mcp_network_get_view_state(self, web_view), request_id)
        : NULL;
    if (!record) {
      wig_mcp_result_send_text(call, "request_id was not found for this tab", TRUE);
    } else {
      NetworkBodyCall *body_call = g_new0(NetworkBodyCall, 1);
      body_call->call = wig_mcp_call_new(self, call, web_view);
      body_call->resource = g_object_ref(record->resource);
      body_call->id = record->id;
      body_call->url = g_strdup(record->url);
      body_call->method = g_strdup(record->method);
      body_call->mime_type = g_strdup(record->mime_type);
      body_call->request_headers = g_strdup(record->request_headers);
      body_call->response_headers = g_strdup(record->response_headers);
      body_call->record_error = g_strdup(record->error);
      body_call->status = record->status;
      body_call->start_us = record->start_us;
      body_call->end_us = record->end_us;

      GCancellable *cancellable = mcp_call_get_cancellable(call);
      webkit_web_resource_get_data(body_call->resource, cancellable, on_network_body_ready, body_call);
    }
  } else if (g_str_equal(name, "browser_dialogs")) {
    guint64 dialog_id = 0;
    const char *action = wig_mcp_json_get_string_member(arguments, "action");
    if (wig_mcp_json_get_uint_member(arguments, "dialog_id", &dialog_id) || action) {
      PendingDialog *found = NULL;
      GList *found_link = NULL;
      for (GList *l = self->dialogs.head; l; l = l->next) {
        PendingDialog *pending = l->data;
        if (pending->id == dialog_id && pending->web_view == web_view) {
          found = pending;
          found_link = l;
          break;
        }
      }
      if (!found || !action || (!g_str_equal(action, "accept") && !g_str_equal(action, "dismiss"))) {
        wig_mcp_result_send_text(call, "A valid dialog_id and action accept or dismiss are required", TRUE);
      } else {
        WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(found->dialog);
        gboolean accept = g_str_equal(action, "accept");
        if (type == WEBKIT_SCRIPT_DIALOG_CONFIRM || type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM)
          webkit_script_dialog_confirm_set_confirmed(found->dialog, accept);
        else if (type == WEBKIT_SCRIPT_DIALOG_PROMPT)
          webkit_script_dialog_prompt_set_text(found->dialog,
                                               accept ? wig_mcp_json_get_string_member(arguments, "text") : NULL);
        g_queue_delete_link(&self->dialogs, found_link);
        wig_mcp_server_pending_dialog_free(found);
        wig_mcp_result_send_text(call, "{\"responded\":true}", FALSE);
      }
    } else {
      g_autoptr(JsonBuilder) builder = json_builder_new();
      json_builder_begin_array(builder);
      for (GList *l = self->dialogs.head; l; l = l->next) {
        PendingDialog *pending = l->data;
        if (pending->web_view != web_view)
          continue;
        WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(pending->dialog);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "dialog_id");
        json_builder_add_int_value(builder, (gint64)pending->id);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, dialog_type_name(type));
        json_builder_set_member_name(builder, "message");
        json_builder_add_string_value(builder, webkit_script_dialog_get_message(pending->dialog));
        if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
          json_builder_set_member_name(builder, "default_text");
          json_builder_add_string_value(builder, webkit_script_dialog_prompt_get_default_text(pending->dialog));
        }
        json_builder_end_object(builder);
      }
      json_builder_end_array(builder);
      g_autoptr(JsonNode) node = wig_mcp_json_builder_take_root(builder);
      g_autofree char *json = wig_mcp_json_node_to_string(node);
      wig_mcp_result_send_text(call, json, FALSE);
    }
  }
}

McpMethodDisposition wig_mcp_tools_handle_list(McpCall *call, gpointer user_data)
{
  JsonObject *params = mcp_call_get_params(call);
  if (params && json_object_has_member(params, "cursor")) {
    JsonNode *cursor = json_object_get_member(params, "cursor");
    if (!JSON_NODE_HOLDS_VALUE(cursor) || json_node_get_value_type(cursor) != G_TYPE_STRING) {
      mcp_call_return_error(call, -32602, "Invalid tools/list cursor", NULL);
      return MCP_METHOD_COMPLETE;
    }
  }

  g_autoptr(JsonNode) result = wig_mcp_tool_list_result();
  mcp_call_return_result(call, result);
  return MCP_METHOD_COMPLETE;
}

McpMethodDisposition wig_mcp_tools_handle_call(McpCall *call, gpointer user_data)
{
  WigMcpServer *self = user_data;
  dispatch_tool_call(self, call, mcp_call_get_params(call));
  return mcp_call_is_completed(call) ? MCP_METHOD_COMPLETE : MCP_METHOD_PENDING;
}
