/* SPDX-License-Identifier: MIT */

#pragma once

#include <gio/gio.h>

#include "mcp-session.h"

G_BEGIN_DECLS

typedef struct _McpCall McpCall;

typedef enum {
  MCP_METHOD_COMPLETE,
  MCP_METHOD_PENDING,
} McpMethodDisposition;

typedef McpMethodDisposition (*McpMethodFunc)(McpCall *call, gpointer user_data);

McpCall *mcp_call_ref(McpCall *call);
void mcp_call_unref(McpCall *call);
const char *mcp_call_get_method(McpCall *call);
JsonObject *mcp_call_get_params(McpCall *call);
McpSession *mcp_call_get_session(McpCall *call);
GCancellable *mcp_call_get_cancellable(McpCall *call);
gboolean mcp_call_is_completed(McpCall *call);
/* Result and error data are copied; the caller retains ownership. */
gboolean mcp_call_return_result(McpCall *call, JsonNode *result);
gboolean mcp_call_return_error(McpCall *call, gint code, const char *message, JsonNode *data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(McpCall, mcp_call_unref)

G_END_DECLS
