/* SPDX-License-Identifier: MIT */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Loaders for the JavaScript injected into pages by the MCP tools. The scripts
 * live under src/mcp/js and are compiled into the binary as GResources. */

char *wig_mcp_scripts_load(const char *name);
char *wig_mcp_scripts_content(const char *format);
char *wig_mcp_scripts_interaction(void);
char *wig_mcp_scripts_tree(void);
char *wig_mcp_scripts_console_read(void);

G_END_DECLS
