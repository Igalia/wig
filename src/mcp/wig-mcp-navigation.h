/* SPDX-License-Identifier: MIT */

#pragma once

#include <mcp-glib.h>

#include "wig-mcp-server.h"
#include "wig-tab.h"

G_BEGIN_DECLS

void wig_mcp_navigation_start(WigMcpServer *self, McpCall *protocol_call, WigTab *tab, double timeout,
                              gboolean include_text, gboolean starts_a_load);
void wig_mcp_navigation_cancel_all(WigMcpServer *self);
void wig_mcp_navigation_dialog_opened(WigMcpServer *self, WebKitWebView *web_view, const char *message);

G_END_DECLS
