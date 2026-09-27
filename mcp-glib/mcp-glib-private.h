/* SPDX-License-Identifier: MIT */

#pragma once

#include "mcp-glib.h"

typedef struct _McpStdioConnection McpStdioConnection;

struct _McpServer {
  gatomicrefcount ref_count;
  GThread *owner_thread; /* weak */
  char *name;
  char *version;
  JsonObject *capabilities;
  GHashTable *methods; /* owned char* -> MethodEntry* */

  GPtrArray *stdio_connections; /* owned McpStdioConnection* */
  guint64 next_session_serial;

  McpServerEventFunc event_func;
  gpointer event_data;
  GDestroyNotify event_destroy;
  gboolean stopped;
};

/* Writes a complete JSON-RPC message that is not a response to anything. The
 * transport owning the session installs this; a transport with no outbound
 * channel leaves it unset and notifications are refused. */
typedef gboolean (*McpSessionSendFunc)(McpSession *session, JsonNode *message, gpointer user_data, GError **error);

void _mcp_session_set_send_func(McpSession *session, McpSessionSendFunc send, gpointer user_data);

McpSession *_mcp_server_create_session(McpServer *server, McpTransportKind kind, const char *id);
void _mcp_server_emit_event(McpServer *server, McpServerEvent event, McpSession *session);
void _mcp_session_close_managed(McpSession *session);

void _mcp_server_transports_init(McpServer *server);
void _mcp_server_transports_clear(McpServer *server);
void _mcp_stdio_init(McpServer *server);
void _mcp_stdio_clear(McpServer *server);
gboolean _mcp_stdio_disconnect_session(McpServer *server, guint64 serial);
void _mcp_stdio_append_sessions(McpServer *server, GPtrArray *sessions);
gboolean _mcp_stdio_has_active_session(McpServer *server);
void _mcp_stdio_stop(McpServer *server, int exit_status);
