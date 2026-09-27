/* SPDX-License-Identifier: MIT */

#pragma once

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _McpTransport McpTransport;

typedef enum {
  MCP_TRANSPORT_STDIO,
  MCP_TRANSPORT_CUSTOM,
} McpTransportKind;

typedef enum {
  MCP_TRANSPORT_OUTCOME_RESPONSE,
  MCP_TRANSPORT_OUTCOME_ACCEPTED,
  MCP_TRANSPORT_OUTCOME_INVALID_INPUT,
} McpTransportOutcome;

/* A transport represents one framed inbound exchange. The library calls
 * @set_pending while asynchronous methods retain that exchange, then calls
 * @complete exactly once with a complete JSON-RPC response or NULL when no
 * wire response is permitted.
 * https://modelcontextprotocol.io/specification/2025-11-25/basic/transports */
/* @response is borrowed and valid only for the duration of @complete. */
typedef void (*McpTransportCompleteFunc)(McpTransport *transport, McpTransportOutcome outcome, JsonNode *response,
                                         gpointer user_data);
typedef void (*McpTransportPendingFunc)(McpTransport *transport, gboolean pending, gpointer user_data);

McpTransport *mcp_transport_new(McpTransportKind kind, McpTransportCompleteFunc complete,
                                McpTransportPendingFunc set_pending, gpointer user_data, GDestroyNotify destroy);
McpTransport *mcp_transport_ref(McpTransport *transport);
void mcp_transport_unref(McpTransport *transport);
McpTransportKind mcp_transport_get_kind(McpTransport *transport);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(McpTransport, mcp_transport_unref)

G_END_DECLS
