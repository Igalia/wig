/* SPDX-License-Identifier: MIT */

#include "mcp-glib-private.h"

#include <string.h>

struct _McpStdioConnection {
  gatomicrefcount ref_count;
  McpServer *server; /* weak; the server owns live connections */
  McpSession *session;
  GDataInputStream *input;
  GCancellable *cancellable;
  McpStdioWriteFunc write;
  McpStdioClosedFunc closed_func;
  gpointer user_data;
  GDestroyNotify destroy;
  gboolean read_pending;
  gboolean closed;
};

typedef struct {
  McpServer *server; /* weak; pending requests are cancelled before server destruction */
  McpStdioConnection *connection;
  McpSessionState initial_state;
} McpStdioRequest;

static void stdio_connection_close(McpStdioConnection *connection, int exit_status, const GError *error);
static void stdio_read_next(McpStdioConnection *connection);

static McpStdioConnection *stdio_connection_ref(McpStdioConnection *connection)
{
  g_atomic_ref_count_inc(&connection->ref_count);
  return connection;
}

static void stdio_connection_unref(McpStdioConnection *connection)
{
  if (!g_atomic_ref_count_dec(&connection->ref_count))
    return;

  _mcp_session_close_managed(connection->session);
  g_clear_object(&connection->session);
  g_clear_object(&connection->input);
  g_clear_object(&connection->cancellable);
  if (connection->destroy)
    connection->destroy(connection->user_data);
  g_free(connection);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(McpStdioConnection, stdio_connection_unref)

static McpStdioRequest *stdio_request_new(McpStdioConnection *connection)
{
  McpStdioRequest *request = g_new0(McpStdioRequest, 1);
  request->server = connection->server;
  request->connection = stdio_connection_ref(connection);
  request->initial_state = mcp_session_get_state(connection->session);
  return request;
}

static void stdio_request_free(McpStdioRequest *request)
{
  g_clear_pointer(&request->connection, stdio_connection_unref);
  g_free(request);
}

static void on_stdio_protocol_complete(McpTransport *transport, McpTransportOutcome outcome, JsonNode *response,
                                       gpointer user_data)
{
  McpStdioRequest *request = user_data;
  McpStdioConnection *connection = request->connection;

  if (request->initial_state != mcp_session_get_state(connection->session))
    _mcp_server_emit_event(request->server, MCP_SERVER_EVENT_SESSION_CHANGED, connection->session);

  if (!response || connection->closed)
    return;

  g_autofree char *json = json_to_string(response, FALSE);
  g_autofree char *line = g_strconcat(json, "\n", NULL);
  g_autoptr(GError) error = NULL;
  if (!connection->write(connection->session, line, strlen(line), connection->user_data, &error)) {
    if (!error)
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "The stdio output callback failed");
    stdio_connection_close(connection, 1, error);
  }
}

static gboolean stdio_send_message(McpSession *session, JsonNode *message, gpointer user_data, GError **error)
{
  McpStdioConnection *connection = user_data;
  if (connection->closed) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "The stdio connection is closed");
    return FALSE;
  }

  g_autofree char *json = json_to_string(message, FALSE);
  g_autofree char *line = g_strconcat(json, "\n", NULL);
  return connection->write(session, line, strlen(line), connection->user_data, error);
}

static void handle_stdio_protocol_message(McpStdioConnection *connection, const char *data, gssize length)
{
  McpStdioRequest *request = stdio_request_new(connection);
  g_autoptr(McpTransport) transport = mcp_transport_new(MCP_TRANSPORT_STDIO, on_stdio_protocol_complete, NULL, request,
                                                        (GDestroyNotify)stdio_request_free);
  mcp_server_handle_message(connection->server, connection->session, transport, data, length);
}

static void stdio_connection_close(McpStdioConnection *connection, int exit_status, const GError *error)
{
  if (connection->closed)
    return;

  connection->closed = TRUE;
  g_cancellable_cancel(connection->cancellable);

  McpServer *server = connection->server;
  for (guint i = 0; i < server->stdio_connections->len; i++) {
    if (g_ptr_array_index(server->stdio_connections, i) == connection) {
      g_ptr_array_remove_index(server->stdio_connections, i);
      break;
    }
  }

  if (!connection->read_pending)
    g_input_stream_close(G_INPUT_STREAM(connection->input), NULL, NULL);
  _mcp_session_close_managed(connection->session);
  _mcp_server_emit_event(server, MCP_SERVER_EVENT_SESSION_REMOVED, connection->session);

  if (connection->closed_func)
    connection->closed_func(connection->session, exit_status, error, connection->user_data);
  if (connection->destroy)
    connection->destroy(connection->user_data);
  connection->write = NULL;
  connection->closed_func = NULL;
  connection->user_data = NULL;
  connection->destroy = NULL;
}

static void on_stdio_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(McpStdioConnection) connection = user_data;
  g_autoptr(GError) error = NULL;
  gsize length = 0;
  g_autofree char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, &error);
  connection->read_pending = FALSE;

  if (connection->closed) {
    g_input_stream_close(G_INPUT_STREAM(connection->input), NULL, NULL);
    return;
  }

  if (!line) {
    gboolean failed = error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    stdio_connection_close(connection, failed ? 1 : 0, failed ? error : NULL);
    return;
  }

  handle_stdio_protocol_message(connection, line, (gssize)length);
  if (!connection->closed && !connection->read_pending)
    stdio_read_next(connection);
}

static void stdio_read_next(McpStdioConnection *connection)
{
  connection->read_pending = TRUE;
  g_data_input_stream_read_line_async(connection->input, G_PRIORITY_DEFAULT, connection->cancellable, on_stdio_line,
                                      stdio_connection_ref(connection));
}

void _mcp_stdio_init(McpServer *server)
{
  server->stdio_connections = g_ptr_array_new_with_free_func((GDestroyNotify)stdio_connection_unref);
}

void _mcp_stdio_clear(McpServer *server)
{
  g_clear_pointer(&server->stdio_connections, g_ptr_array_unref);
}

McpSession *mcp_server_add_stdio(McpServer *server, GInputStream *input, McpStdioWriteFunc write,
                                 McpStdioClosedFunc closed, gpointer user_data, GDestroyNotify destroy, GError **error)
{
  g_return_val_if_fail(server != NULL, NULL);
  g_return_val_if_fail(G_IS_INPUT_STREAM(input), NULL);
  g_return_val_if_fail(write != NULL, NULL);
  g_return_val_if_fail(server->owner_thread == g_thread_self(), NULL);

  if (server->stopped) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "The MCP server has stopped");
    return NULL;
  }

  McpStdioConnection *connection = g_new0(McpStdioConnection, 1);
  g_atomic_ref_count_init(&connection->ref_count);
  connection->server = server;
  connection->session = _mcp_server_create_session(server, MCP_TRANSPORT_STDIO, NULL);
  connection->input = g_data_input_stream_new(input);
  connection->cancellable = g_cancellable_new();
  connection->write = write;
  connection->closed_func = closed;
  connection->user_data = user_data;
  connection->destroy = destroy;
  _mcp_session_set_send_func(connection->session, stdio_send_message, connection);

  McpSession *session = g_object_ref(connection->session);
  g_ptr_array_add(server->stdio_connections, connection);
  stdio_read_next(connection);
  _mcp_server_emit_event(server, MCP_SERVER_EVENT_SESSION_ADDED, connection->session);
  return session;
}

gboolean _mcp_stdio_disconnect_session(McpServer *server, guint64 serial)
{
  for (guint i = 0; i < server->stdio_connections->len; i++) {
    McpStdioConnection *connection = g_ptr_array_index(server->stdio_connections, i);
    if (mcp_session_get_serial(connection->session) == serial) {
      g_autoptr(McpStdioConnection) owned = stdio_connection_ref(connection);
      stdio_connection_close(owned, 0, NULL);
      return TRUE;
    }
  }
  return FALSE;
}

void _mcp_stdio_append_sessions(McpServer *server, GPtrArray *sessions)
{
  for (guint i = 0; i < server->stdio_connections->len; i++) {
    McpStdioConnection *connection = g_ptr_array_index(server->stdio_connections, i);
    if (!connection->closed)
      g_ptr_array_add(sessions, g_object_ref(connection->session));
  }
}

gboolean _mcp_stdio_has_active_session(McpServer *server)
{
  for (guint i = 0; i < server->stdio_connections->len; i++) {
    McpStdioConnection *connection = g_ptr_array_index(server->stdio_connections, i);
    if (!connection->closed && mcp_session_get_state(connection->session) == MCP_SESSION_ACTIVE)
      return TRUE;
  }
  return FALSE;
}

void _mcp_stdio_stop(McpServer *server, int exit_status)
{
  while (server->stdio_connections->len) {
    McpStdioConnection *connection = g_ptr_array_index(server->stdio_connections, 0);
    g_autoptr(McpStdioConnection) owned = stdio_connection_ref(connection);
    stdio_connection_close(owned, exit_status, NULL);
  }
}
