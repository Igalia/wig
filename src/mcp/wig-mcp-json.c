/* SPDX-License-Identifier: MIT */

#include "wig-mcp-json.h"

#include <math.h>

char *wig_mcp_json_node_to_string(JsonNode *node)
{
  return json_to_string(node, FALSE);
}

JsonNode *wig_mcp_json_builder_take_root(JsonBuilder *builder)
{
  return json_builder_get_root(builder);
}

JsonObject *wig_mcp_json_get_params_object(JsonObject *request)
{
  if (!json_object_has_member(request, "params"))
    return NULL;

  JsonNode *params = json_object_get_member(request, "params");
  return JSON_NODE_HOLDS_OBJECT(params) ? json_node_get_object(params) : NULL;
}

gboolean wig_mcp_json_get_uint_member(JsonObject *object, const char *name, guint64 *value)
{
  if (!object || !json_object_has_member(object, name))
    return FALSE;

  JsonNode *node = json_object_get_member(object, name);
  if (!JSON_NODE_HOLDS_VALUE(node))
    return FALSE;

  GType type = json_node_get_value_type(node);
  if (type == G_TYPE_INT64) {
    gint64 number = json_node_get_int(node);
    if (number < 0)
      return FALSE;
    *value = (guint64)number;
    return TRUE;
  }

  if (type == G_TYPE_DOUBLE) {
    double number = json_node_get_double(node);
    if (!isfinite(number) || number < 0 || floor(number) != number || number >= 0x1p64)
      return FALSE;
    *value = (guint64)number;
    return TRUE;
  }
  return FALSE;
}

const char *wig_mcp_json_get_string_member(JsonObject *object, const char *name)
{
  if (!object || !json_object_has_member(object, name))
    return NULL;

  JsonNode *node = json_object_get_member(object, name);
  return JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node)
                                                                                        : NULL;
}

JsonNode *wig_mcp_json_parse(const char *data)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, -1, NULL))
    return NULL;

  return json_node_copy(json_parser_get_root(parser));
}

void wig_mcp_json_add_nullable_string(JsonBuilder *builder, const char *name, const char *value)
{
  json_builder_set_member_name(builder, name);
  if (value)
    json_builder_add_string_value(builder, value);
  else
    json_builder_add_null_value(builder);
}
