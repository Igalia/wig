/* SPDX-License-Identifier: MIT */

#pragma once

#include <json-glib/json-glib.h>
#include <mcp-glib.h>

G_BEGIN_DECLS

gboolean wig_mcp_tool_is_known(const char *name);
gboolean wig_mcp_tool_reject_invalid_arguments(McpCall *call, const char *name, JsonObject *arguments);
JsonNode *wig_mcp_tool_list_result(void);

G_END_DECLS
