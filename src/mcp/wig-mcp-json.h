/* SPDX-License-Identifier: MIT */

#pragma once

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Small JSON-glib conveniences shared across the MCP server translation units. */

char *wig_mcp_json_node_to_string(JsonNode *node);
JsonNode *wig_mcp_json_builder_take_root(JsonBuilder *builder);
JsonNode *wig_mcp_json_parse(const char *data);

JsonObject *wig_mcp_json_get_params_object(JsonObject *request);
gboolean wig_mcp_json_get_uint_member(JsonObject *object, const char *name, guint64 *value);
const char *wig_mcp_json_get_string_member(JsonObject *object, const char *name);
void wig_mcp_json_add_nullable_string(JsonBuilder *builder, const char *name, const char *value);

G_END_DECLS
