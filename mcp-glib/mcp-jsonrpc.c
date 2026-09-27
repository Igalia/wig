/* SPDX-License-Identifier: MIT */

#include "mcp-jsonrpc-private.h"

#include <math.h>

/* MCP narrows JSON-RPC IDs, params, and results beyond base JSON-RPC.
 * https://modelcontextprotocol.io/specification/2025-11-25/basic */

static const char *get_string_member(JsonObject *object, const char *name)
{
  if (!json_object_has_member(object, name))
    return NULL;
  JsonNode *node = json_object_get_member(object, name);
  return JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node)
                                                                                        : NULL;
}

static gboolean id_is_valid(JsonNode *id)
{
  if (!id || !JSON_NODE_HOLDS_VALUE(id))
    return FALSE;
  GType type = json_node_get_value_type(id);
  if (type == G_TYPE_STRING)
    return TRUE;
  if (type == G_TYPE_INT64)
    return TRUE;
  if (type == G_TYPE_DOUBLE) {
    double value = json_node_get_double(id);
    return isfinite(value) && floor(value) == value;
  }
  return FALSE;
}

JsonNode *mcp_jsonrpc_parse(const char *data, gssize length)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!data || !g_utf8_validate(data, length, NULL) || !json_parser_load_from_data(parser, data, length, NULL))
    return NULL;
  return json_node_copy(json_parser_get_root(parser));
}

gboolean mcp_jsonrpc_parse_message(JsonNode *node, McpJsonrpcMessage *message)
{
  *message = (McpJsonrpcMessage) { .kind = MCP_JSONRPC_INVALID };
  if (!JSON_NODE_HOLDS_OBJECT(node))
    return FALSE;

  JsonObject *object = json_node_get_object(node);
  JsonNode *id = json_object_has_member(object, "id") ? json_object_get_member(object, "id") : NULL;
  if (id_is_valid(id))
    message->id = id;

  const char *jsonrpc = get_string_member(object, "jsonrpc");
  if (!jsonrpc || !g_str_equal(jsonrpc, "2.0"))
    return FALSE;

  const char *method = get_string_member(object, "method");
  if (method) {
    if (g_str_has_prefix(method, "rpc."))
      return FALSE;
    if (id && !id_is_valid(id))
      return FALSE;
    if (json_object_has_member(object, "params") && !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "params")))
      return FALSE;
    message->kind = id ? MCP_JSONRPC_REQUEST : MCP_JSONRPC_NOTIFICATION;
    message->id = id;
    message->method = method;
    message->params = json_object_has_member(object, "params") ? json_object_get_object_member(object, "params") : NULL;
    return TRUE;
  }

  if (!id_is_valid(id) && (!id || !JSON_NODE_HOLDS_NULL(id)))
    return FALSE;
  gboolean has_result = json_object_has_member(object, "result");
  gboolean has_error = json_object_has_member(object, "error");
  if (has_result == has_error)
    return FALSE;
  if (has_result && !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "result")))
    return FALSE;
  if (has_error) {
    JsonNode *error_node = json_object_get_member(object, "error");
    if (!JSON_NODE_HOLDS_OBJECT(error_node))
      return FALSE;
    JsonObject *error = json_node_get_object(error_node);
    if (!json_object_has_member(error, "code") || !json_object_has_member(error, "message"))
      return FALSE;
    JsonNode *code = json_object_get_member(error, "code");
    if (!JSON_NODE_HOLDS_VALUE(code) || json_node_get_value_type(code) != G_TYPE_INT64
        || !get_string_member(error, "message"))
      return FALSE;
  }
  message->kind = MCP_JSONRPC_RESPONSE;
  message->id = id;
  return TRUE;
}

static JsonNode *new_envelope(JsonNode *id, const char *member, JsonNode *payload)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "jsonrpc");
  json_builder_add_string_value(builder, "2.0");
  json_builder_set_member_name(builder, "id");
  if (id)
    json_builder_add_value(builder, json_node_copy(id));
  else
    json_builder_add_null_value(builder);
  json_builder_set_member_name(builder, member);
  json_builder_add_value(builder, payload);
  json_builder_end_object(builder);
  return json_builder_get_root(builder);
}

JsonNode *mcp_jsonrpc_new_result(JsonNode *id, JsonNode *result)
{
  return new_envelope(id, "result", result);
}

/* A notification carries no id at all, rather than a null one, so it cannot be
 * mistaken for a response awaiting correlation. Takes ownership of @params. */
JsonNode *mcp_jsonrpc_new_notification(const char *method, JsonNode *params)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "jsonrpc");
  json_builder_add_string_value(builder, "2.0");
  json_builder_set_member_name(builder, "method");
  json_builder_add_string_value(builder, method);
  if (params) {
    json_builder_set_member_name(builder, "params");
    json_builder_add_value(builder, params);
  }
  json_builder_end_object(builder);
  return json_builder_get_root(builder);
}

JsonNode *mcp_jsonrpc_new_error(JsonNode *id, gint code, const char *message, JsonNode *data)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "code");
  json_builder_add_int_value(builder, code);
  json_builder_set_member_name(builder, "message");
  json_builder_add_string_value(builder, message);
  if (data) {
    json_builder_set_member_name(builder, "data");
    json_builder_add_value(builder, json_node_copy(data));
  }
  json_builder_end_object(builder);
  g_autoptr(JsonNode) error = json_builder_get_root(builder);
  return new_envelope(id, "error", json_node_copy(error));
}

char *mcp_jsonrpc_id_key(JsonNode *id)
{
  if (!id_is_valid(id))
    return NULL;

  GType type = json_node_get_value_type(id);
  if (type == G_TYPE_STRING)
    return g_strdup_printf("s:%s", json_node_get_string(id));
  if (type == G_TYPE_INT64)
    return g_strdup_printf("i:%" G_GINT64_FORMAT, json_node_get_int(id));
  return g_strdup_printf("i:%.0f", json_node_get_double(id));
}
