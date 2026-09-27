/* SPDX-License-Identifier: MIT */

#pragma once

#include <json-glib/json-glib.h>

typedef enum {
  MCP_JSONRPC_REQUEST,
  MCP_JSONRPC_NOTIFICATION,
  MCP_JSONRPC_RESPONSE,
  MCP_JSONRPC_INVALID,
} McpJsonrpcKind;

typedef struct {
  McpJsonrpcKind kind;
  JsonNode *id; /* borrowed */
  const char *method; /* borrowed */
  JsonObject *params; /* borrowed */
} McpJsonrpcMessage;

JsonNode *mcp_jsonrpc_parse(const char *data, gssize length);
gboolean mcp_jsonrpc_parse_message(JsonNode *node, McpJsonrpcMessage *message);
JsonNode *mcp_jsonrpc_new_result(JsonNode *id, JsonNode *result);
JsonNode *mcp_jsonrpc_new_notification(const char *method, JsonNode *params);
JsonNode *mcp_jsonrpc_new_error(JsonNode *id, gint code, const char *message, JsonNode *data);
char *mcp_jsonrpc_id_key(JsonNode *id);
