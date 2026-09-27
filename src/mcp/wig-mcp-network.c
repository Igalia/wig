/* SPDX-License-Identifier: MIT */

#include "wig-mcp-network.h"
#include "wig-mcp-json.h"
#include "wig-mcp-tools.h"
#include "wig-web-process-extension.h"

static void add_header_to_builder(const char *name, const char *value, gpointer user_data)
{
  JsonBuilder *builder = JSON_BUILDER(user_data);
  json_builder_set_member_name(builder, name);
  json_builder_add_string_value(builder, value);
}

static char *headers_to_json(SoupMessageHeaders *headers)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  if (headers)
    soup_message_headers_foreach(headers, add_header_to_builder, builder);
  json_builder_end_object(builder);

  g_autoptr(JsonNode) node = wig_mcp_json_builder_take_root(builder);
  return wig_mcp_json_node_to_string(node);
}

static void network_record_disconnect(NetworkRecord *record)
{
  if (!record->resource)
    return;

  if (record->sent_request_id)
    g_signal_handler_disconnect(record->resource, record->sent_request_id);
  if (record->response_id)
    g_signal_handler_disconnect(record->resource, record->response_id);
  if (record->failed_id)
    g_signal_handler_disconnect(record->resource, record->failed_id);
  if (record->finished_id)
    g_signal_handler_disconnect(record->resource, record->finished_id);

  record->sent_request_id = record->response_id = record->failed_id = record->finished_id = 0;
}

static void network_record_free(NetworkRecord *record)
{
  network_record_disconnect(record);
  g_clear_object(&record->resource);
  g_free(record->url);
  g_free(record->method);
  g_free(record->mime_type);
  g_free(record->request_headers);
  g_free(record->response_headers);
  g_free(record->error);
  g_free(record);
}

static void view_state_clear_records(ViewState *state)
{
  g_queue_clear_full(&state->records, (GDestroyNotify)network_record_free);
}

static void console_message_free(ConsoleMessage *message)
{
  g_free(message->source);
  g_free(message->level);
  g_free(message->text);
  g_free(message->source_id);
  g_free(message);
}

void wig_mcp_network_remove_console_message(ViewState *state, guint64 id)
{
  for (GList *l = state->console_messages.head; l; l = l->next) {
    ConsoleMessage *message = l->data;
    if (message->id == id) {
      g_queue_delete_link(&state->console_messages, l);
      console_message_free(message);
      return;
    }
  }
}

char *wig_mcp_network_console_messages_json(ViewState *state)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);
  for (GList *l = state ? state->console_messages.head : NULL; l; l = l->next) {
    ConsoleMessage *message = l->data;
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_int_value(builder, (gint64)message->id);
    json_builder_set_member_name(builder, "level");
    json_builder_add_string_value(builder, message->level);
    json_builder_set_member_name(builder, "source");
    json_builder_add_string_value(builder, message->source);
    json_builder_set_member_name(builder, "timestamp");
    json_builder_add_double_value(builder, (double)message->time_us / 1000.0);
    json_builder_set_member_name(builder, "text");
    json_builder_add_string_value(builder, message->text);
    json_builder_set_member_name(builder, "url");
    json_builder_add_string_value(builder, message->source_id ? message->source_id : "");
    json_builder_set_member_name(builder, "line");
    json_builder_add_int_value(builder, message->line);
    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);

  g_autoptr(JsonNode) node = wig_mcp_json_builder_take_root(builder);
  return wig_mcp_json_node_to_string(node);
}

static gboolean on_user_message_received(WebKitWebView *web_view, WebKitUserMessage *message, ViewState *state)
{
  if (!g_str_equal(webkit_user_message_get_name(message), WIG_CONSOLE_MESSAGE_NAME))
    return FALSE;

  GVariant *parameters = webkit_user_message_get_parameters(message);
  if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE(WIG_CONSOLE_MESSAGE_FORMAT)))
    return FALSE;

  const char *source = NULL;
  const char *level = NULL;
  const char *text = NULL;
  const char *source_id = NULL;
  guint32 line = 0;
  g_variant_get(parameters, WIG_CONSOLE_MESSAGE_FORMAT, &source, &level, &text, &source_id, &line);

  ConsoleMessage *entry = g_new0(ConsoleMessage, 1);
  entry->id = ++state->next_console_id;
  entry->source = g_strdup(source);
  entry->level = g_strdup(level);
  entry->text = g_strdup(text);
  entry->source_id = source_id && *source_id ? g_strdup(source_id) : NULL;
  entry->line = line;
  entry->time_us = g_get_real_time();
  g_queue_push_tail(&state->console_messages, entry);
  g_debug("mcp: console message from the web process: %s/%s %s", entry->source, entry->level, entry->text);

  while (state->console_messages.length > MCP_MAX_CONSOLE_MESSAGES)
    console_message_free(g_queue_pop_head(&state->console_messages));

  return TRUE;
}

static void network_record_update_response(NetworkRecord *record)
{
  WebKitURIResponse *response = webkit_web_resource_get_response(record->resource);
  if (!response)
    return;

  g_set_str(&record->url, webkit_uri_response_get_uri(response));
  record->status = webkit_uri_response_get_status_code(response);
  g_set_str(&record->mime_type, webkit_uri_response_get_mime_type(response));
  g_free(record->response_headers);
  record->response_headers = headers_to_json(webkit_uri_response_get_http_headers(response));
}

static void on_resource_response(WebKitWebResource *resource, GParamSpec *pspec, NetworkRecord *record)
{
  network_record_update_response(record);
}

static void on_resource_sent_request(WebKitWebResource *resource, WebKitURIRequest *request,
                                     WebKitURIResponse *redirected_response, NetworkRecord *record)
{
  g_set_str(&record->url, webkit_uri_request_get_uri(request));
  g_set_str(&record->method, webkit_uri_request_get_http_method(request));
  g_free(record->request_headers);
  record->request_headers = headers_to_json(webkit_uri_request_get_http_headers(request));
}

static void on_resource_failed(WebKitWebResource *resource, GError *error, NetworkRecord *record)
{
  g_set_str(&record->error, error->message);
}

static void on_resource_finished(WebKitWebResource *resource, NetworkRecord *record)
{
  record->end_us = g_get_real_time();
  network_record_update_response(record);
}

static void on_resource_load_started(WebKitWebView *web_view, WebKitWebResource *resource, WebKitURIRequest *request,
                                     ViewState *state)
{
  NetworkRecord *record = g_new0(NetworkRecord, 1);
  record->id = ++state->server->next_request_id;
  record->generation = state->navigation_generation;
  record->resource = g_object_ref(resource);
  record->url = g_strdup(webkit_uri_request_get_uri(request));
  record->method = g_strdup(webkit_uri_request_get_http_method(request));
  record->request_headers = headers_to_json(webkit_uri_request_get_http_headers(request));
  record->response_headers = g_strdup("{}");
  record->start_us = g_get_real_time();

  record->sent_request_id = g_signal_connect(resource, "sent-request", G_CALLBACK(on_resource_sent_request), record);
  record->response_id = g_signal_connect(resource, "notify::response", G_CALLBACK(on_resource_response), record);
  record->failed_id = g_signal_connect(resource, "failed", G_CALLBACK(on_resource_failed), record);
  record->finished_id = g_signal_connect(resource, "finished", G_CALLBACK(on_resource_finished), record);

  g_queue_push_tail(&state->records, record);
  while (g_queue_get_length(&state->records) > MCP_MAX_NETWORK_RECORDS)
    network_record_free(g_queue_pop_head(&state->records));
}

gboolean wig_mcp_network_record_load_failure(GSignalInvocationHint *hint, guint n_params, const GValue *params,
                                             gpointer user_data)
{
  WigMcpServer *self = user_data;
  WebKitWebView *web_view = WEBKIT_WEB_VIEW(g_value_get_object(&params[0]));
  ViewState *state = wig_mcp_network_get_view_state(self, web_view);
  const char *failing_uri = g_value_get_string(&params[2]);
  const GError *error = g_value_get_boxed(&params[3]);

  if (state) {
    g_free(state->failure);
    state->failure = g_strdup_printf("Navigation to %s failed: %s", failing_uri ? failing_uri : "(unknown)",
                                     error ? error->message : "unknown error");
    state->failed_navigation_generation = state->navigation_generation;
    g_debug("mcp: %s", state->failure);
  }
  return TRUE;
}

static void on_view_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, ViewState *state)
{
  /* Records outlive the navigation that issued them, since those are usually
   * the ones worth inspecting. The ring buffer bounds the queue instead. */
  if (load_event == WEBKIT_LOAD_STARTED)
    state->navigation_generation++;
  else if (load_event == WEBKIT_LOAD_COMMITTED)
    state->committed_navigation_generation = state->navigation_generation;
}

static gboolean on_script_dialog(WebKitWebView *web_view, WebKitScriptDialog *dialog, ViewState *state)
{
  if (!wig_mcp_server_handles_dialogs(state->server))
    return FALSE;

  PendingDialog *pending = g_new0(PendingDialog, 1);
  pending->id = ++state->server->next_dialog_id;
  pending->dialog = webkit_script_dialog_ref(dialog);
  pending->web_view = web_view;
  g_queue_push_tail(&state->server->dialogs, pending);
  g_debug("mcp: retained JavaScript dialog %" G_GUINT64_FORMAT, pending->id);
  wig_mcp_tools_dialog_opened(state->server, web_view, pending);
  return TRUE;
}

static void remove_dialogs_for_view(WigMcpServer *self, WebKitWebView *web_view)
{
  for (GList *l = self->dialogs.head; l;) {
    GList *next = l->next;
    PendingDialog *pending = l->data;
    if (pending->web_view == web_view) {
      g_queue_delete_link(&self->dialogs, l);
      wig_mcp_server_pending_dialog_free(pending);
    }
    l = next;
  }
}

static void view_state_free(ViewState *state)
{
  view_state_clear_records(state);
  g_queue_clear_full(&state->console_messages, (GDestroyNotify)console_message_free);
  g_free(state->failure);
  g_free(state);
}

static void on_view_finalized(gpointer data, GObject *where_the_view_was)
{
  ViewState *state = data;
  WigMcpServer *self = state->server;
  remove_dialogs_for_view(self, (WebKitWebView *)where_the_view_was);
  g_hash_table_steal(self->views, where_the_view_was);
  state->web_view = NULL;
  view_state_free(state);
}

void wig_mcp_network_view_state_free(gpointer data)
{
  ViewState *state = data;
  if (state->web_view)
    g_object_weak_unref(G_OBJECT(state->web_view), on_view_finalized, state);
  if (state->web_view) {
    if (state->load_changed_id)
      g_signal_handler_disconnect(state->web_view, state->load_changed_id);
    if (state->resource_started_id)
      g_signal_handler_disconnect(state->web_view, state->resource_started_id);
    if (state->script_dialog_id)
      g_signal_handler_disconnect(state->web_view, state->script_dialog_id);
    if (state->user_message_id)
      g_signal_handler_disconnect(state->web_view, state->user_message_id);
  }

  view_state_clear_records(state);
  g_queue_clear_full(&state->console_messages, (GDestroyNotify)console_message_free);
  g_free(state->failure);
  g_free(state);
}

void wig_mcp_server_watch_view(WigMcpServer *self, WebKitWebView *web_view)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(WEBKIT_IS_WEB_VIEW(web_view));
  if (self->stopped || !self->instrumentation_active || g_hash_table_contains(self->views, web_view))
    return;

  ViewState *state = g_new0(ViewState, 1);
  state->server = self;
  state->web_view = web_view;
  g_queue_init(&state->records);
  g_queue_init(&state->console_messages);
  state->user_message_id = g_signal_connect(web_view, "user-message-received", G_CALLBACK(on_user_message_received),
                                            state);
  state->load_changed_id = g_signal_connect(web_view, "load-changed", G_CALLBACK(on_view_load_changed), state);
  state->resource_started_id = g_signal_connect(web_view, "resource-load-started", G_CALLBACK(on_resource_load_started),
                                                state);
  state->script_dialog_id = g_signal_connect(web_view, "script-dialog", G_CALLBACK(on_script_dialog), state);

  g_hash_table_insert(self->views, web_view, state);
  g_object_weak_ref(G_OBJECT(web_view), on_view_finalized, state);
}

ViewState *wig_mcp_network_get_view_state(WigMcpServer *self, WebKitWebView *web_view)
{
  return g_hash_table_lookup(self->views, web_view);
}

void wig_mcp_network_remove_record(ViewState *state, NetworkRecord *record)
{
  if (g_queue_remove(&state->records, record))
    network_record_free(record);
}

NetworkRecord *wig_mcp_network_find_record(ViewState *state, guint64 id)
{
  if (!state)
    return NULL;
  for (GList *l = state->records.head; l; l = l->next) {
    NetworkRecord *record = l->data;
    if (record->id == id)
      return record;
  }
  return NULL;
}
