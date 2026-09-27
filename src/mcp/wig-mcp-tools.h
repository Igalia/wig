/* SPDX-License-Identifier: MIT */

#pragma once

#include <mcp-glib.h>

#include "wig-mcp-server.h"

G_BEGIN_DECLS

McpMethodDisposition wig_mcp_tools_handle_list(McpCall *call, gpointer user_data);
McpMethodDisposition wig_mcp_tools_handle_call(McpCall *call, gpointer user_data);
void wig_mcp_tools_dialog_opened(WigMcpServer *self, WebKitWebView *web_view, PendingDialog *pending);

G_END_DECLS
