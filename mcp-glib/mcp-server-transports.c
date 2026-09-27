/* SPDX-License-Identifier: MIT */

#include "mcp-glib-private.h"

void _mcp_server_transports_init(McpServer *server)
{
  _mcp_stdio_init(server);
}

void _mcp_server_transports_clear(McpServer *server)
{
  _mcp_stdio_clear(server);
}

gboolean mcp_server_disconnect_session(McpServer *server, guint64 serial)
{
  g_return_val_if_fail(server != NULL, FALSE);
  g_return_val_if_fail(server->owner_thread == g_thread_self(), FALSE);
  return _mcp_stdio_disconnect_session(server, serial);
}

GPtrArray *mcp_server_dup_sessions(McpServer *server)
{
  g_return_val_if_fail(server != NULL, NULL);
  g_return_val_if_fail(server->owner_thread == g_thread_self(), NULL);

  GPtrArray *sessions = g_ptr_array_new_with_free_func(g_object_unref);
  _mcp_stdio_append_sessions(server, sessions);
  return sessions;
}

gboolean mcp_server_has_transport(McpServer *server, McpTransportKind kind)
{
  g_return_val_if_fail(server != NULL, FALSE);
  g_return_val_if_fail(server->owner_thread == g_thread_self(), FALSE);
  if (kind == MCP_TRANSPORT_STDIO)
    return server->stdio_connections->len > 0;
  return FALSE;
}

gboolean mcp_server_has_active_session(McpServer *server, McpTransportKind kind)
{
  g_return_val_if_fail(server != NULL, FALSE);
  g_return_val_if_fail(server->owner_thread == g_thread_self(), FALSE);
  if (kind == MCP_TRANSPORT_STDIO)
    return _mcp_stdio_has_active_session(server);
  return FALSE;
}

void mcp_server_stop(McpServer *server)
{
  g_return_if_fail(server != NULL);
  g_return_if_fail(server->owner_thread == g_thread_self());

  if (server->stopped)
    return;
  server->stopped = TRUE;

  _mcp_stdio_stop(server, 1);
}
