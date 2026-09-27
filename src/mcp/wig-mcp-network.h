/* SPDX-License-Identifier: MIT */

#pragma once

#include <wpe/webkit.h>

#include "wig-mcp-server.h"

G_BEGIN_DECLS

void wig_mcp_network_view_state_free(gpointer data);
ViewState *wig_mcp_network_get_view_state(WigMcpServer *self, WebKitWebView *web_view);
NetworkRecord *wig_mcp_network_find_record(ViewState *state, guint64 id);
void wig_mcp_network_remove_record(ViewState *state, NetworkRecord *record);
void wig_mcp_network_remove_console_message(ViewState *state, guint64 id);
char *wig_mcp_network_console_messages_json(ViewState *state);
gboolean wig_mcp_network_record_load_failure(GSignalInvocationHint *hint, guint n_params, const GValue *params,
                                             gpointer user_data);

G_END_DECLS
