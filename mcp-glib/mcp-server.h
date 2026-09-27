/* SPDX-License-Identifier: MIT */

#pragma once

#include "mcp-call.h"

G_BEGIN_DECLS

#define MCP_PROTOCOL_VERSION_2025_11_25 "2025-11-25"
#define MCP_PROTOCOL_VERSION MCP_PROTOCOL_VERSION_2025_11_25

typedef struct _McpServer McpServer;

typedef enum {
  MCP_SERVER_EVENT_TRANSPORT_CHANGED,
  MCP_SERVER_EVENT_SESSION_ADDED,
  MCP_SERVER_EVENT_SESSION_CHANGED,
  MCP_SERVER_EVENT_SESSION_REMOVED,
} McpServerEvent;

/* @session is borrowed and may be NULL for MCP_SERVER_EVENT_TRANSPORT_CHANGED.
 * Event callbacks may query the server or disconnect other sessions. */
typedef void (*McpServerEventFunc)(McpServer *server, McpServerEvent event, McpSession *session, gpointer user_data);
/* @message is one complete newline-terminated JSON-RPC message. */
typedef gboolean (*McpStdioWriteFunc)(McpSession *session, const char *message, gsize length, gpointer user_data,
                                      GError **error);
/* @error is non-NULL only when the connection closed due to an I/O failure. */
typedef void (*McpStdioClosedFunc)(McpSession *session, int exit_status, const GError *error, gpointer user_data);

McpServer *mcp_server_new(const char *name, const char *version, JsonObject *capabilities);
McpServer *mcp_server_ref(McpServer *server);
void mcp_server_unref(McpServer *server);
gboolean mcp_server_add_method(McpServer *server, const char *method, McpMethodFunc function, gpointer user_data,
                               GDestroyNotify destroy, GError **error);
void mcp_server_set_event_func(McpServer *server, McpServerEventFunc function, gpointer user_data,
                               GDestroyNotify destroy);

/* The returned session is owned by the caller. The server owns the connection,
 * input framing, and session until EOF or explicit disconnection. @user_data
 * is adopted only on success. @closed runs before @destroy when the connection
 * ends. */
McpSession *mcp_server_add_stdio(McpServer *server, GInputStream *input, McpStdioWriteFunc write,
                                 McpStdioClosedFunc closed, gpointer user_data, GDestroyNotify destroy, GError **error);
gboolean mcp_server_disconnect_session(McpServer *server, guint64 serial);
/* Returns owned references to every managed session. */
GPtrArray *mcp_server_dup_sessions(McpServer *server);
/* Stdio means at least one live connection. */
gboolean mcp_server_has_transport(McpServer *server, McpTransportKind kind);
gboolean mcp_server_has_active_session(McpServer *server, McpTransportKind kind);
/* Permanently stops all listeners and connections. */
void mcp_server_stop(McpServer *server);

/* Recognizes a standalone JSON-RPC initialize request before validating its
 * MCP parameters, so transports can create provisional sessions. */
gboolean mcp_message_is_initialize(const char *data, gssize length);

/* Handles one UTF-8 message with transport framing already removed. Method
 * callbacks may complete synchronously or retain McpCall for async work.
 * McpServer, McpSession, McpTransport, and McpCall are confined to the thread
 * on which they are created; atomic reference counts provide lifetime safety,
 * not concurrent mutation. */
void mcp_server_handle_message(McpServer *server, McpSession *session, McpTransport *transport, const char *data,
                               gssize length);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(McpServer, mcp_server_unref)

G_END_DECLS
