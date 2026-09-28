/* SPDX-License-Identifier: MIT */

#include "mcp-glib-private.h"

#include "mcp-jsonrpc-private.h"

typedef struct {
  McpMethodFunc function;
  gpointer user_data;
  GDestroyNotify destroy;
} MethodEntry;

typedef struct _McpDispatch McpDispatch;

struct _McpSession {
  GObject parent;

  GThread *owner_thread; /* weak */
  McpTransportKind transport_kind;
  McpSessionState state;
  char *id;
  char *protocol_version;
  char *client_name;
  char *client_version;
  JsonObject *client_capabilities;
  GHashTable *seen_request_ids; /* owned char* set */
  GHashTable *active_calls; /* owned ID key -> weak McpCall* */
  McpServer *managed_server; /* weak */
  McpSessionSendFunc send_func;
  gpointer send_data; /* weak; owned by the transport, which outlives the session */
  guint64 serial;
};

struct _McpTransport {
  gatomicrefcount ref_count;
  McpTransportKind kind;
  McpTransportCompleteFunc complete;
  McpTransportPendingFunc set_pending;
  gpointer user_data;
  GDestroyNotify destroy;
  gboolean pending;
  gboolean completed;
};

struct _McpDispatch {
  gatomicrefcount ref_count;
  McpTransport *transport;
  JsonNode *response; /* nullable */
  guint pending_calls;
  gboolean dispatch_complete;
};

struct _McpCall {
  gatomicrefcount ref_count;
  McpDispatch *dispatch;
  McpSession *session;
  JsonNode *id;
  char *id_key;
  char *method;
  JsonObject *params;
  GCancellable *cancellable;
  gboolean completed;
  gboolean cancelled;
  gboolean library_pending_ref;
};

G_DEFINE_FINAL_TYPE(McpSession, mcp_session, G_TYPE_OBJECT)

typedef enum {
  PROP_SESSION_STATE = 1,
  PROP_SESSION_PROTOCOL_VERSION,
  PROP_SESSION_CLIENT_NAME,
  PROP_SESSION_CLIENT_VERSION,
  N_SESSION_PROPERTIES,
} McpSessionProperty;

static GParamSpec *session_properties[N_SESSION_PROPERTIES];

static void session_set_state(McpSession *session, McpSessionState state)
{
  if (session->state == state)
    return;

  session->state = state;
  g_object_notify_by_pspec(G_OBJECT(session), session_properties[PROP_SESSION_STATE]);
}

static void method_entry_free(MethodEntry *entry)
{
  if (entry->destroy)
    entry->destroy(entry->user_data);
  g_free(entry);
}

static McpDispatch *dispatch_ref(McpDispatch *dispatch)
{
  g_atomic_ref_count_inc(&dispatch->ref_count);
  return dispatch;
}

static void dispatch_unref(McpDispatch *dispatch)
{
  if (!g_atomic_ref_count_dec(&dispatch->ref_count))
    return;
  g_clear_pointer(&dispatch->response, json_node_unref);
  g_clear_pointer(&dispatch->transport, mcp_transport_unref);
  g_free(dispatch);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(McpDispatch, dispatch_unref)

static McpDispatch *dispatch_new(McpTransport *transport)
{
  McpDispatch *dispatch = g_new0(McpDispatch, 1);
  g_atomic_ref_count_init(&dispatch->ref_count);
  dispatch->transport = mcp_transport_ref(transport);
  return dispatch;
}

static void transport_set_pending(McpTransport *transport, gboolean pending)
{
  if (transport->pending == pending)
    return;
  transport->pending = pending;
  if (transport->set_pending)
    transport->set_pending(transport, pending, transport->user_data);
}

static void transport_complete(McpTransport *transport, McpTransportOutcome outcome, JsonNode *response)
{
  g_return_if_fail(!transport->completed);
  transport->completed = TRUE;
  transport->complete(transport, outcome, response, transport->user_data);
  transport_set_pending(transport, FALSE);
}

static void dispatch_maybe_finish(McpDispatch *dispatch)
{
  if (!dispatch->dispatch_complete || dispatch->pending_calls > 0 || dispatch->transport->completed)
    return;

  transport_complete(dispatch->transport,
                     dispatch->response ? MCP_TRANSPORT_OUTCOME_RESPONSE : MCP_TRANSPORT_OUTCOME_ACCEPTED,
                     dispatch->response);
}

static void dispatch_store_response(McpDispatch *dispatch, JsonNode *response)
{
  g_assert(dispatch->response == NULL);
  dispatch->response = json_node_ref(response);
}

static void call_remove_from_session(McpCall *call)
{
  if (call->id_key)
    g_hash_table_remove(call->session->active_calls, call->id_key);
}

static gboolean call_complete(McpCall *call, JsonNode *response)
{
  if (call->completed)
    return FALSE;

  call->completed = TRUE;
  call_remove_from_session(call);
  if (response)
    dispatch_store_response(call->dispatch, response);

  g_assert(call->dispatch->pending_calls > 0);
  call->dispatch->pending_calls--;
  dispatch_maybe_finish(call->dispatch);

  if (call->library_pending_ref) {
    call->library_pending_ref = FALSE;
    mcp_call_unref(call);
  }
  return TRUE;
}

static void call_cancel(McpCall *call)
{
  if (call->completed)
    return;

  /* GCancellable emits synchronously. Keep the call alive in case a handler
   * completes and drops its own reference from the cancelled callback. */
  mcp_call_ref(call);
  call->cancelled = TRUE;
  g_cancellable_cancel(call->cancellable);
  /* A cancelled request gets no reply: the client already said it no longer
   * wants one. */
  if (!call->completed)
    call_complete(call, NULL);
  mcp_call_unref(call);
}

static McpCall *call_new(McpDispatch *dispatch, McpSession *session, const McpJsonrpcMessage *message)
{
  McpCall *call = g_new0(McpCall, 1);
  g_atomic_ref_count_init(&call->ref_count);
  call->dispatch = dispatch_ref(dispatch);
  call->session = g_object_ref(session);
  call->id = json_node_copy(message->id);
  call->id_key = mcp_jsonrpc_id_key(message->id);
  call->method = g_strdup(message->method);
  call->params = message->params ? json_object_ref(message->params) : NULL;
  call->cancellable = g_cancellable_new();
  g_hash_table_insert(session->active_calls, g_strdup(call->id_key), call);
  return call;
}

static JsonNode *empty_object_node(void)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_end_object(builder);
  return json_builder_get_root(builder);
}

static void validate_experimental_capability(JsonObject *object, const char *member_name, JsonNode *member_node,
                                             gpointer user_data)
{
  gboolean *valid = user_data;
  *valid &= JSON_NODE_HOLDS_OBJECT(member_node);
}

static gboolean validate_client_capabilities(JsonObject *capabilities)
{
  if (json_object_has_member(capabilities, "roots")) {
    JsonNode *roots_node = json_object_get_member(capabilities, "roots");
    if (!JSON_NODE_HOLDS_OBJECT(roots_node))
      return FALSE;

    JsonObject *roots = json_node_get_object(roots_node);
    if (json_object_has_member(roots, "listChanged")) {
      JsonNode *list_changed = json_object_get_member(roots, "listChanged");
      if (!JSON_NODE_HOLDS_VALUE(list_changed) || json_node_get_value_type(list_changed) != G_TYPE_BOOLEAN)
        return FALSE;
    }
  }

  if (json_object_has_member(capabilities, "sampling")
      && !JSON_NODE_HOLDS_OBJECT(json_object_get_member(capabilities, "sampling")))
    return FALSE;

  if (json_object_has_member(capabilities, "experimental")) {
    JsonNode *experimental_node = json_object_get_member(capabilities, "experimental");
    if (!JSON_NODE_HOLDS_OBJECT(experimental_node))
      return FALSE;

    gboolean valid = TRUE;
    json_object_foreach_member(json_node_get_object(experimental_node), validate_experimental_capability, &valid);
    if (!valid)
      return FALSE;
  }

  return TRUE;
}

static gboolean validate_metadata_object(JsonObject *params)
{
  if (!params || !json_object_has_member(params, "_meta"))
    return TRUE;

  JsonNode *metadata_node = json_object_get_member(params, "_meta");
  return JSON_NODE_HOLDS_OBJECT(metadata_node);
}

static gboolean validate_request_metadata(JsonObject *params)
{
  if (!validate_metadata_object(params))
    return FALSE;

  if (!params || !json_object_has_member(params, "_meta"))
    return TRUE;

  JsonNode *metadata_node = json_object_get_member(params, "_meta");
  JsonObject *metadata = json_node_get_object(metadata_node);
  if (!json_object_has_member(metadata, "progressToken"))
    return TRUE;

  JsonNode *token = json_object_get_member(metadata, "progressToken");
  if (!JSON_NODE_HOLDS_VALUE(token))
    return FALSE;

  GType token_type = json_node_get_value_type(token);
  return token_type == G_TYPE_STRING || token_type == G_TYPE_INT64 || token_type == G_TYPE_DOUBLE;
}

static gboolean validate_initialize(McpJsonrpcMessage *message, const char **protocol_version,
                                    JsonObject **capabilities, const char **client_name, const char **client_version)
{
  if (!message->params)
    return FALSE;

  JsonObject *params = message->params;
  JsonNode *protocol = json_object_get_member(params, "protocolVersion");
  JsonNode *client_capabilities = json_object_get_member(params, "capabilities");
  JsonNode *client_info = json_object_get_member(params, "clientInfo");
  if (!protocol || !JSON_NODE_HOLDS_VALUE(protocol) || json_node_get_value_type(protocol) != G_TYPE_STRING
      || !client_capabilities || !JSON_NODE_HOLDS_OBJECT(client_capabilities) || !client_info
      || !JSON_NODE_HOLDS_OBJECT(client_info))
    return FALSE;

  JsonObject *capability_object = json_node_get_object(client_capabilities);
  if (!validate_client_capabilities(capability_object))
    return FALSE;

  JsonObject *info = json_node_get_object(client_info);
  JsonNode *name = json_object_get_member(info, "name");
  JsonNode *version = json_object_get_member(info, "version");
  if (!name || !JSON_NODE_HOLDS_VALUE(name) || json_node_get_value_type(name) != G_TYPE_STRING || !version
      || !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_STRING)
    return FALSE;

  *protocol_version = json_node_get_string(protocol);
  *capabilities = capability_object;
  *client_name = json_node_get_string(name);
  *client_version = json_node_get_string(version);
  return TRUE;
}

static JsonNode *initialize_result(McpServer *server, const char *protocol_version)
{
  const char *selected = g_str_equal(protocol_version, MCP_PROTOCOL_VERSION) ? protocol_version : MCP_PROTOCOL_VERSION;
  g_autoptr(JsonBuilder) builder = json_builder_new();

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "protocolVersion");
  json_builder_add_string_value(builder, selected);
  json_builder_set_member_name(builder, "capabilities");

  JsonNode *capabilities = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(capabilities, server->capabilities);
  json_builder_add_value(builder, capabilities);
  json_builder_set_member_name(builder, "serverInfo");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "name");
  json_builder_add_string_value(builder, server->name);
  json_builder_set_member_name(builder, "version");
  json_builder_add_string_value(builder, server->version);
  json_builder_end_object(builder);
  json_builder_end_object(builder);

  return json_builder_get_root(builder);
}

static void dispatch_error(McpDispatch *dispatch, JsonNode *id, gint code, const char *message)
{
  g_autoptr(JsonNode) response = mcp_jsonrpc_new_error(id, code, message, NULL);
  dispatch_store_response(dispatch, response);
}

static void handle_cancellation(McpSession *session, JsonObject *params)
{
  if (!params || !json_object_has_member(params, "requestId"))
    return;

  if (json_object_has_member(params, "reason")) {
    JsonNode *reason = json_object_get_member(params, "reason");
    if (!JSON_NODE_HOLDS_VALUE(reason) || json_node_get_value_type(reason) != G_TYPE_STRING)
      return;
  }

  JsonNode *id = json_object_get_member(params, "requestId");
  g_autofree char *key = mcp_jsonrpc_id_key(id);
  McpCall *call = key ? g_hash_table_lookup(session->active_calls, key) : NULL;
  if (call && !g_str_equal(call->method, "initialize"))
    call_cancel(call);
}

static void dispatch_request(McpServer *server, McpSession *session, McpDispatch *dispatch, McpJsonrpcMessage *message)
{
  g_autofree char *id_key = mcp_jsonrpc_id_key(message->id);
  if (g_hash_table_contains(session->seen_request_ids, id_key)) {
    dispatch_error(dispatch, message->id, -32600, "Request ID has already been used");
    return;
  }
  g_hash_table_add(session->seen_request_ids, g_steal_pointer(&id_key));

  if (!validate_request_metadata(message->params)) {
    dispatch_error(dispatch, message->id, -32602, "Invalid request metadata");
    return;
  }

  /* Initialization is the first interaction.
   * https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle */
  if (g_str_equal(message->method, "initialize")) {
    if (session->state != MCP_SESSION_NEW) {
      dispatch_error(dispatch, message->id, -32600, "Initialize must be the first request");
      return;
    }

    const char *protocol_version = NULL;
    const char *client_name = NULL;
    const char *client_version = NULL;
    JsonObject *capabilities = NULL;
    if (!validate_initialize(message, &protocol_version, &capabilities, &client_name, &client_version)) {
      dispatch_error(dispatch, message->id, -32602, "Invalid initialize parameters");
      return;
    }

    g_object_freeze_notify(G_OBJECT(session));
    if (g_set_str(&session->protocol_version,
                  g_str_equal(protocol_version, MCP_PROTOCOL_VERSION) ? protocol_version : MCP_PROTOCOL_VERSION))
      g_object_notify_by_pspec(G_OBJECT(session), session_properties[PROP_SESSION_PROTOCOL_VERSION]);
    if (g_set_str(&session->client_name, client_name))
      g_object_notify_by_pspec(G_OBJECT(session), session_properties[PROP_SESSION_CLIENT_NAME]);
    if (g_set_str(&session->client_version, client_version))
      g_object_notify_by_pspec(G_OBJECT(session), session_properties[PROP_SESSION_CLIENT_VERSION]);
    g_clear_pointer(&session->client_capabilities, json_object_unref);
    session->client_capabilities = json_object_ref(capabilities);
    session_set_state(session, MCP_SESSION_AWAITING_INITIALIZED);
    g_object_thaw_notify(G_OBJECT(session));
    g_autoptr(JsonNode) result = initialize_result(server, protocol_version);
    g_autoptr(JsonNode) response = mcp_jsonrpc_new_result(message->id, json_node_copy(result));
    dispatch_store_response(dispatch, response);
    return;
  }

  /* A ping is the one request a client may send before initialization completes,
   * and answering it must not disturb the session state. */
  if (g_str_equal(message->method, "ping")) {
    if (session->state == MCP_SESSION_CLOSED) {
      dispatch_error(dispatch, message->id, -32002, "Server is not initialized");
      return;
    }
    g_autoptr(JsonNode) result = empty_object_node();
    g_autoptr(JsonNode) response = mcp_jsonrpc_new_result(message->id, json_node_copy(result));
    dispatch_store_response(dispatch, response);
    return;
  }

  /* Rejecting a request sent before initialization must not close the session:
   * clients that probe for a newer protocol revision (for example
   * `server/discover`) fall back to `initialize` on the same connection. */
  if (session->state == MCP_SESSION_NEW) {
    dispatch_error(dispatch, message->id, -32002, "Initialize must be the first interaction");
    return;
  }

  if (session->state != MCP_SESSION_ACTIVE) {
    dispatch_error(dispatch, message->id, -32002, "Server is not initialized");
    return;
  }

  MethodEntry *entry = g_hash_table_lookup(server->methods, message->method);
  if (!entry) {
    dispatch_error(dispatch, message->id, -32601, "Method not found");
    return;
  }

  dispatch->pending_calls++;
  g_autoptr(McpCall) call = call_new(dispatch, session, message);
  McpMethodDisposition disposition = entry->function(call, entry->user_data);

  if (disposition == MCP_METHOD_COMPLETE && !call->completed) {
    mcp_call_return_error(call, -32603, "Method completed without a response", NULL);
  } else if (disposition == MCP_METHOD_PENDING && !call->completed) {
    call->library_pending_ref = TRUE;
    mcp_call_ref(call);
    transport_set_pending(dispatch->transport, TRUE);
  } else if (disposition != MCP_METHOD_COMPLETE && disposition != MCP_METHOD_PENDING && !call->completed) {
    mcp_call_return_error(call, -32603, "Method returned an invalid disposition", NULL);
  }
}

static void dispatch_notification(McpSession *session, McpJsonrpcMessage *message)
{
  if (session->state == MCP_SESSION_NEW)
    return;

  if (!validate_metadata_object(message->params))
    return;

  if (g_str_equal(message->method, "notifications/initialized")) {
    if (session->state == MCP_SESSION_AWAITING_INITIALIZED)
      session_set_state(session, MCP_SESSION_ACTIVE);
    return;
  }
  if (g_str_equal(message->method, "notifications/cancelled"))
    handle_cancellation(session, message->params);
}

static void dispatch_message(McpServer *server, McpSession *session, McpDispatch *dispatch, JsonNode *node)
{
  McpJsonrpcMessage message;
  if (!mcp_jsonrpc_parse_message(node, &message)) {
    dispatch_error(dispatch, message.id, -32600, "Invalid Request");
    return;
  }

  switch (message.kind) {
  case MCP_JSONRPC_REQUEST:
    dispatch_request(server, session, dispatch, &message);
    break;
  case MCP_JSONRPC_NOTIFICATION:
    dispatch_notification(session, &message);
    break;
  case MCP_JSONRPC_RESPONSE:
    /* Server-originated requests are not exposed yet; valid unsolicited responses are ignored. */
    break;
  case MCP_JSONRPC_INVALID:
    g_assert_not_reached();
  }
}

McpServer *mcp_server_new(const char *name, const char *version, JsonObject *capabilities)
{
  g_return_val_if_fail(name != NULL, NULL);
  g_return_val_if_fail(version != NULL, NULL);
  g_return_val_if_fail(capabilities != NULL, NULL);
  McpServer *server = g_new0(McpServer, 1);
  g_atomic_ref_count_init(&server->ref_count);
  server->owner_thread = g_thread_self();
  server->name = g_strdup(name);
  server->version = g_strdup(version);
  server->capabilities = json_object_ref(capabilities);
  server->methods = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)method_entry_free);
  _mcp_server_transports_init(server);
  return server;
}

McpServer *mcp_server_ref(McpServer *server)
{
  g_atomic_ref_count_inc(&server->ref_count);
  return server;
}

void mcp_server_unref(McpServer *server)
{
  if (!g_atomic_ref_count_dec(&server->ref_count))
    return;

  mcp_server_stop(server);
  _mcp_server_transports_clear(server);
  if (server->event_destroy)
    server->event_destroy(server->event_data);
  g_clear_pointer(&server->methods, g_hash_table_unref);
  g_clear_pointer(&server->capabilities, json_object_unref);
  g_clear_pointer(&server->name, g_free);
  g_clear_pointer(&server->version, g_free);
  g_free(server);
}

gboolean mcp_server_add_method(McpServer *server, const char *method, McpMethodFunc function, gpointer user_data,
                               GDestroyNotify destroy, GError **error)
{
  g_return_val_if_fail(server != NULL, FALSE);
  g_return_val_if_fail(method != NULL, FALSE);
  g_return_val_if_fail(function != NULL, FALSE);
  g_return_val_if_fail(server->owner_thread == g_thread_self(), FALSE);

  if (g_str_has_prefix(method, "rpc.") || g_str_equal(method, "initialize") || g_str_equal(method, "ping")) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Method '%s' is reserved", method);
    return FALSE;
  }

  if (g_hash_table_contains(server->methods, method)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Method '%s' is already registered", method);
    return FALSE;
  }

  MethodEntry *entry = g_new0(MethodEntry, 1);
  entry->function = function;
  entry->user_data = user_data;
  entry->destroy = destroy;
  g_hash_table_insert(server->methods, g_strdup(method), entry);

  return TRUE;
}

void mcp_server_set_event_func(McpServer *server, McpServerEventFunc function, gpointer user_data,
                               GDestroyNotify destroy)
{
  g_return_if_fail(server != NULL);
  g_return_if_fail(server->owner_thread == g_thread_self());

  if (server->event_data == user_data) {
    server->event_func = function;
    server->event_destroy = destroy;
    return;
  }

  GDestroyNotify old_destroy = server->event_destroy;
  gpointer old_data = server->event_data;
  server->event_func = NULL;
  server->event_data = NULL;
  server->event_destroy = NULL;
  if (old_destroy)
    old_destroy(old_data);

  server->event_func = function;
  server->event_data = user_data;
  server->event_destroy = destroy;
}

McpSession *_mcp_server_create_session(McpServer *server, McpTransportKind kind, const char *id)
{
  McpSession *session = mcp_session_new(kind, id);
  session->managed_server = server;
  session->serial = ++server->next_session_serial;
  return session;
}

void _mcp_server_emit_event(McpServer *server, McpServerEvent event, McpSession *session)
{
  if (server->event_func)
    server->event_func(server, event, session, server->event_data);
}

gboolean mcp_message_is_initialize(const char *data, gssize length)
{
  g_autoptr(JsonNode) root = mcp_jsonrpc_parse(data, length);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root))
    return FALSE;

  JsonObject *object = json_node_get_object(root);
  JsonNode *jsonrpc = json_object_get_member(object, "jsonrpc");
  JsonNode *method = json_object_get_member(object, "method");
  JsonNode *id = json_object_get_member(object, "id");
  if (!jsonrpc || !JSON_NODE_HOLDS_VALUE(jsonrpc) || json_node_get_value_type(jsonrpc) != G_TYPE_STRING
      || !g_str_equal(json_node_get_string(jsonrpc), "2.0") || !method || !JSON_NODE_HOLDS_VALUE(method)
      || json_node_get_value_type(method) != G_TYPE_STRING || !g_str_equal(json_node_get_string(method), "initialize"))
    return FALSE;

  g_autofree char *id_key = mcp_jsonrpc_id_key(id);
  return id_key != NULL;
}

void mcp_server_handle_message(McpServer *server, McpSession *session, McpTransport *transport, const char *data,
                               gssize length)
{
  g_return_if_fail(server != NULL);
  g_return_if_fail(session != NULL);
  g_return_if_fail(transport != NULL);
  g_return_if_fail(server->owner_thread == g_thread_self());
  g_return_if_fail(session->owner_thread == g_thread_self());
  g_return_if_fail(!transport->completed);

  g_autoptr(JsonNode) root = mcp_jsonrpc_parse(data, length);
  if (!root) {
    g_autoptr(JsonNode) response = mcp_jsonrpc_new_error(NULL, -32700, "Parse error", NULL);
    transport_complete(transport, MCP_TRANSPORT_OUTCOME_RESPONSE, response);
    return;
  }

  /* An array is a JSON-RPC batch, which MCP does not use. Concurrency comes
   * from sending independent messages and correlating them by id, so nothing
   * is lost by refusing the grouped form. */
  if (JSON_NODE_HOLDS_ARRAY(root)) {
    g_autoptr(JsonNode) response = mcp_jsonrpc_new_error(NULL, -32600, "JSON-RPC batches are not supported", NULL);
    transport_complete(transport, MCP_TRANSPORT_OUTCOME_INVALID_INPUT, response);
    return;
  }

  g_autoptr(McpDispatch) dispatch = dispatch_new(transport);
  dispatch_message(server, session, dispatch, root);
  dispatch->dispatch_complete = TRUE;
  dispatch_maybe_finish(dispatch);
}

McpSession *mcp_session_new(McpTransportKind transport_kind, const char *id)
{
  McpSession *session = g_object_new(MCP_TYPE_SESSION, NULL);

  session->transport_kind = transport_kind;
  session->id = g_strdup(id);
  return session;
}

static void mcp_session_finalize(GObject *object)
{
  McpSession *session = MCP_SESSION(object);

  _mcp_session_close_managed(session);
  g_clear_pointer(&session->active_calls, g_hash_table_unref);
  g_clear_pointer(&session->seen_request_ids, g_hash_table_unref);
  g_clear_pointer(&session->client_capabilities, json_object_unref);
  g_clear_pointer(&session->id, g_free);
  g_clear_pointer(&session->protocol_version, g_free);
  g_clear_pointer(&session->client_name, g_free);
  g_clear_pointer(&session->client_version, g_free);

  G_OBJECT_CLASS(mcp_session_parent_class)->finalize(object);
}

static void mcp_session_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  McpSession *session = MCP_SESSION(object);

  switch ((McpSessionProperty)prop_id) {
  case PROP_SESSION_STATE:
    g_value_set_enum(value, session->state);
    break;
  case PROP_SESSION_PROTOCOL_VERSION:
    g_value_set_string(value, session->protocol_version);
    break;
  case PROP_SESSION_CLIENT_NAME:
    g_value_set_string(value, session->client_name);
    break;
  case PROP_SESSION_CLIENT_VERSION:
    g_value_set_string(value, session->client_version);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void mcp_session_class_init(McpSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->finalize = mcp_session_finalize;
  object_class->get_property = mcp_session_get_property;

  session_properties[PROP_SESSION_STATE] = g_param_spec_enum(
      "state", NULL, NULL, MCP_TYPE_SESSION_STATE, MCP_SESSION_NEW, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  session_properties[PROP_SESSION_PROTOCOL_VERSION] = g_param_spec_string("protocol-version", NULL, NULL, NULL,
                                                                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  session_properties[PROP_SESSION_CLIENT_NAME] = g_param_spec_string("client-name", NULL, NULL, NULL,
                                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  session_properties[PROP_SESSION_CLIENT_VERSION] = g_param_spec_string("client-version", NULL, NULL, NULL,
                                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_SESSION_PROPERTIES, session_properties);
}

static void mcp_session_init(McpSession *session)
{
  session->owner_thread = g_thread_self();
  session->state = MCP_SESSION_NEW;
  session->seen_request_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  session->active_calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static void session_close(McpSession *session)
{
  if (session->state == MCP_SESSION_CLOSED)
    return;

  session_set_state(session, MCP_SESSION_CLOSED);

  g_autoptr(GPtrArray) calls = g_ptr_array_new_with_free_func((GDestroyNotify)mcp_call_unref);
  GHashTableIter iter;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, session->active_calls);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    g_ptr_array_add(calls, mcp_call_ref(value));

  for (guint i = 0; i < calls->len; i++)
    call_cancel(g_ptr_array_index(calls, i));
}

void _mcp_session_close_managed(McpSession *session)
{
  session->managed_server = NULL;
  /* A session can outlive the transport that created it, so the outbound hook
   * goes with the transport rather than being left pointing at freed data. */
  session->send_func = NULL;
  session->send_data = NULL;
  session_close(session);
}

void mcp_session_close(McpSession *session)
{
  g_return_if_fail(session->owner_thread == g_thread_self());
  if (session->managed_server) {
    mcp_server_disconnect_session(session->managed_server, session->serial);
    return;
  }
  session_close(session);
}

McpSessionState mcp_session_get_state(McpSession *session)
{
  return session->state;
}

guint64 mcp_session_get_serial(McpSession *session)
{
  return session->serial;
}

McpTransportKind mcp_session_get_transport_kind(McpSession *session)
{
  return session->transport_kind;
}

const char *mcp_session_get_id(McpSession *session)
{
  return session->id;
}

const char *mcp_session_get_protocol_version(McpSession *session)
{
  return session->protocol_version;
}

const char *mcp_session_get_client_name(McpSession *session)
{
  return session->client_name;
}

const char *mcp_session_get_client_version(McpSession *session)
{
  return session->client_version;
}

JsonObject *mcp_session_get_client_capabilities(McpSession *session)
{
  return session->client_capabilities;
}

void _mcp_session_set_send_func(McpSession *session, McpSessionSendFunc send, gpointer user_data)
{
  session->send_func = send;
  session->send_data = user_data;
}

/* Send a server-initiated notification, such as a resource update.
 *
 * Only an active session accepts one: before the client's initialized
 * notification it has not agreed to anything yet, and after close there is
 * nowhere to write. */
gboolean mcp_session_send_notification(McpSession *session, const char *method, JsonNode *params, GError **error)
{
  g_return_val_if_fail(session != NULL, FALSE);
  g_return_val_if_fail(method != NULL, FALSE);
  g_return_val_if_fail(session->owner_thread == g_thread_self(), FALSE);

  if (session->state != MCP_SESSION_ACTIVE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "The session is not active");
    return FALSE;
  }

  if (!session->send_func) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "The transport has no outbound channel");
    return FALSE;
  }

  g_autoptr(JsonNode) message = mcp_jsonrpc_new_notification(method, params ? json_node_copy(params) : NULL);
  return session->send_func(session, message, session->send_data, error);
}

McpTransport *mcp_transport_new(McpTransportKind kind, McpTransportCompleteFunc complete,
                                McpTransportPendingFunc set_pending, gpointer user_data, GDestroyNotify destroy)
{
  g_return_val_if_fail(complete != NULL, NULL);
  McpTransport *transport = g_new0(McpTransport, 1);
  g_atomic_ref_count_init(&transport->ref_count);
  transport->kind = kind;
  transport->complete = complete;
  transport->set_pending = set_pending;
  transport->user_data = user_data;
  transport->destroy = destroy;
  return transport;
}

McpTransport *mcp_transport_ref(McpTransport *transport)
{
  g_atomic_ref_count_inc(&transport->ref_count);
  return transport;
}

void mcp_transport_unref(McpTransport *transport)
{
  if (!g_atomic_ref_count_dec(&transport->ref_count))
    return;

  if (transport->destroy)
    transport->destroy(transport->user_data);
  g_free(transport);
}

McpTransportKind mcp_transport_get_kind(McpTransport *transport)
{
  return transport->kind;
}

McpCall *mcp_call_ref(McpCall *call)
{
  g_atomic_ref_count_inc(&call->ref_count);
  return call;
}

void mcp_call_unref(McpCall *call)
{
  if (!g_atomic_ref_count_dec(&call->ref_count))
    return;

  call_remove_from_session(call);
  g_clear_object(&call->cancellable);
  g_clear_pointer(&call->params, json_object_unref);
  g_clear_pointer(&call->method, g_free);
  g_clear_pointer(&call->id_key, g_free);
  g_clear_pointer(&call->id, json_node_unref);
  g_clear_object(&call->session);
  g_clear_pointer(&call->dispatch, dispatch_unref);
  g_free(call);
}

const char *mcp_call_get_method(McpCall *call)
{
  return call->method;
}

JsonObject *mcp_call_get_params(McpCall *call)
{
  return call->params;
}

McpSession *mcp_call_get_session(McpCall *call)
{
  return call->session;
}

GCancellable *mcp_call_get_cancellable(McpCall *call)
{
  return call->cancellable;
}

gboolean mcp_call_is_completed(McpCall *call)
{
  return call->completed;
}

gboolean mcp_call_return_result(McpCall *call, JsonNode *result)
{
  g_return_val_if_fail(call->session->owner_thread == g_thread_self(), FALSE);
  if (call->cancelled)
    return FALSE;
  if (!result || !JSON_NODE_HOLDS_OBJECT(result))
    return mcp_call_return_error(call, -32603, "Method returned a non-object result", NULL);

  g_autoptr(JsonNode) response = mcp_jsonrpc_new_result(call->id, json_node_copy(result));
  return call_complete(call, response);
}

gboolean mcp_call_return_error(McpCall *call, gint code, const char *message, JsonNode *data)
{
  g_return_val_if_fail(call->session->owner_thread == g_thread_self(), FALSE);
  g_return_val_if_fail(message != NULL, FALSE);
  if (call->cancelled)
    return FALSE;
  g_autoptr(JsonNode) response = mcp_jsonrpc_new_error(call->id, code, message, data);
  return call_complete(call, response);
}
