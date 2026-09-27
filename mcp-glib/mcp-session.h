/* SPDX-License-Identifier: MIT */

#pragma once

#include "mcp-enums.h"
#include "mcp-transport.h"

G_BEGIN_DECLS

typedef enum {
  MCP_SESSION_NEW,
  MCP_SESSION_AWAITING_INITIALIZED,
  MCP_SESSION_ACTIVE,
  MCP_SESSION_CLOSED,
} McpSessionState;

#define MCP_TYPE_SESSION (mcp_session_get_type())
G_DECLARE_FINAL_TYPE(McpSession, mcp_session, MCP, SESSION, GObject)

McpSession *mcp_session_new(McpTransportKind transport_kind, const char *id);
/* Server-managed sessions are disconnected from their transport as well. */
void mcp_session_close(McpSession *session);
McpSessionState mcp_session_get_state(McpSession *session);
guint64 mcp_session_get_serial(McpSession *session);
McpTransportKind mcp_session_get_transport_kind(McpSession *session);
const char *mcp_session_get_id(McpSession *session);
const char *mcp_session_get_protocol_version(McpSession *session);
const char *mcp_session_get_client_name(McpSession *session);
const char *mcp_session_get_client_version(McpSession *session);
/* Returns a borrowed object owned by @session. */
JsonObject *mcp_session_get_client_capabilities(McpSession *session);
/* @params is borrowed and may be NULL. */
gboolean mcp_session_send_notification(McpSession *session, const char *method, JsonNode *params, GError **error);

G_END_DECLS
