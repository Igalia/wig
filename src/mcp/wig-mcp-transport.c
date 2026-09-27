/* SPDX-License-Identifier: MIT */

#include "wig-mcp-server.h"

typedef struct {
  GApplication *application;
  GApplicationCommandLine *command_line;
  gboolean application_held;
} WigMcpStdio;

static void wig_mcp_stdio_free(WigMcpStdio *stdio)
{
  if (stdio->application_held)
    g_application_release(stdio->application);
  g_clear_object(&stdio->application);
  g_clear_object(&stdio->command_line);
  g_free(stdio);
}

static gboolean write_stdio(McpSession *session, const char *message, gsize length, gpointer user_data, GError **error)
{
  WigMcpStdio *stdio = user_data;
  g_application_command_line_print_literal(stdio->command_line, message);
  return TRUE;
}

static void on_stdio_closed(McpSession *session, int exit_status, const GError *error, gpointer user_data)
{
  WigMcpStdio *stdio = user_data;
  if (error)
    g_application_command_line_printerr(stdio->command_line, "MCP stdio failed: %s\n", error->message);
  g_application_command_line_set_exit_status(stdio->command_line, exit_status);

  if (stdio->application_held) {
    stdio->application_held = FALSE;
    g_application_release(stdio->application);
  }
}

gboolean wig_mcp_server_start_stdio(WigMcpServer *self, GApplicationCommandLine *command_line, GError **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(G_IS_APPLICATION_COMMAND_LINE(command_line), FALSE);
  if (self->stopped) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "The MCP server has stopped");
    return FALSE;
  }

  if (mcp_server_has_transport(self->protocol_server, MCP_TRANSPORT_STDIO)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "An MCP stdio client is already connected");
    return FALSE;
  }

  g_autoptr(GInputStream) input = g_application_command_line_get_stdin(command_line);
  if (!input) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "The invoking process did not provide stdin");
    return FALSE;
  }

  WigMcpStdio *stdio = g_new0(WigMcpStdio, 1);
  stdio->application = g_object_ref(G_APPLICATION(self->application));
  stdio->command_line = g_object_ref(command_line);
  g_application_hold(stdio->application);
  stdio->application_held = TRUE;

  g_autoptr(McpSession) session = mcp_server_add_stdio(self->protocol_server, input, write_stdio, on_stdio_closed,
                                                       stdio, (GDestroyNotify)wig_mcp_stdio_free, error);
  if (!session) {
    wig_mcp_stdio_free(stdio);
    return FALSE;
  }

  return TRUE;
}
