/* SPDX-License-Identifier: MIT */

#include "wig-mcp-result.h"

#include <string.h>

#include "wig-mcp-json.h"

static void add_text_block(JsonBuilder *builder, const char *text)
{
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "text");
  json_builder_set_member_name(builder, "text");
  json_builder_add_string_value(builder, text ? text : "");
  json_builder_end_object(builder);
}

JsonNode *wig_mcp_result_text_new(const char *text, gboolean is_error)
{
  return wig_mcp_result_text_pair_new(text, NULL, is_error);
}

/* Two text blocks in one result. Used to report what an interaction did
 * alongside the page it produced, keeping the machine-readable outcome summary
 * separate from the tree instead of embedding the tree in it as an escaped
 * string, which would double its tabs and newlines and be far harder to read. */
JsonNode *wig_mcp_result_text_pair_new(const char *first, const char *second, gboolean is_error)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "content");
  json_builder_begin_array(builder);
  add_text_block(builder, first);
  if (second)
    add_text_block(builder, second);
  json_builder_end_array(builder);

  if (is_error) {
    json_builder_set_member_name(builder, "isError");
    json_builder_add_boolean_value(builder, TRUE);
  }

  json_builder_end_object(builder);
  return wig_mcp_json_builder_take_root(builder);
}

void wig_mcp_result_send_text(McpCall *call, const char *text, gboolean is_error)
{
  g_autoptr(JsonNode) result = wig_mcp_result_text_new(text, is_error);
  mcp_call_return_result(call, result);
}

/* Bound a free-text result, cutting on a character boundary so the tail is not
 * a partial UTF-8 sequence, and record what was dropped. Takes ownership.
 *
 * Only valid for prose or markup. Results a client has to parse are checked
 * against the same ceiling but reported as errors, because truncating them
 * yields something that looks like data and is not. */
char *wig_mcp_result_truncate_text(char *text)
{
  gsize length = text ? strlen(text) : 0;
  if (length <= MCP_MAX_TEXT_RESULT_BYTES)
    return text;

  g_autofree char *owned = text;
  const char *end = NULL;
  if (g_utf8_validate_len(owned, MCP_MAX_TEXT_RESULT_BYTES, &end))
    end = owned + MCP_MAX_TEXT_RESULT_BYTES;

  gsize kept = (gsize)(end - owned);
  return g_strdup_printf("%.*s\n[truncated: %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " bytes shown]", (int)kept, owned,
                         kept, length);
}

/* Holds what an asynchronous tool needs once its WebKit operation completes.
 * The view is weak because a tab can close while the operation is in flight. */
WigMcpCall *wig_mcp_call_new(WigMcpServer *server, McpCall *protocol_call, WebKitWebView *web_view)
{
  WigMcpCall *call = g_new0(WigMcpCall, 1);
  call->server = wig_mcp_server_ref(server);
  call->protocol_call = mcp_call_ref(protocol_call);
  g_weak_ref_init(&call->web_view, web_view);
  return call;
}

void wig_mcp_call_free(WigMcpCall *call)
{
  g_weak_ref_clear(&call->web_view);
  mcp_call_unref(call->protocol_call);
  wig_mcp_server_unref(call->server);
  g_free(call);
}

void wig_mcp_call_finish_node_take(WigMcpCall *call, JsonNode *result)
{
  g_autoptr(JsonNode) owned_result = result;
  mcp_call_return_result(call->protocol_call, result);
  wig_mcp_call_free(call);
}

void wig_mcp_call_finish_text(WigMcpCall *call, const char *text, gboolean is_error)
{
  wig_mcp_call_finish_node_take(call, wig_mcp_result_text_new(text, is_error));
}
