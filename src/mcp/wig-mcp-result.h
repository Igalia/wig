/* SPDX-License-Identifier: MIT */

#pragma once

#include <json-glib/json-glib.h>
#include <mcp-glib.h>
#include <wpe/webkit.h>

#include "wig-mcp-server.h"

G_BEGIN_DECLS

typedef struct _WigMcpCall WigMcpCall;

struct _WigMcpCall {
  WigMcpServer *server;
  McpCall *protocol_call;
  GWeakRef web_view;
};

JsonNode *wig_mcp_result_text_new(const char *text, gboolean is_error);
JsonNode *wig_mcp_result_text_pair_new(const char *first, const char *second, gboolean is_error);
void wig_mcp_result_send_text(McpCall *call, const char *text, gboolean is_error);
char *wig_mcp_result_truncate_text(char *text);

WigMcpCall *wig_mcp_call_new(WigMcpServer *server, McpCall *protocol_call, WebKitWebView *web_view);
void wig_mcp_call_free(WigMcpCall *call);
void wig_mcp_call_finish_node_take(WigMcpCall *call, JsonNode *result);
void wig_mcp_call_finish_text(WigMcpCall *call, const char *text, gboolean is_error);

G_END_DECLS
