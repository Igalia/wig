/* SPDX-License-Identifier: MIT */

#include "wig-mcp-scripts.h"

/* Load an embedded JavaScript resource as a newly allocated, NUL-terminated
 * string. The scripts live under src/mcp/js and are compiled into the binary. */
char *wig_mcp_scripts_load(const char *name)
{
  g_autofree char *path = g_strconcat("/com/igalia/wig/mcp/", name, NULL);
  g_autoptr(GBytes) bytes = g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  if (!bytes)
    g_error("mcp: missing embedded script %s", name);

  gsize size = 0;
  const char *data = g_bytes_get_data(bytes, &size);
  return g_strndup(data, size);
}

char *wig_mcp_scripts_content(const char *format)
{
  if (g_str_equal(format, "html"))
    return wig_mcp_scripts_load("content-html.js");
  if (g_str_equal(format, "json"))
    return wig_mcp_scripts_load("content-json.js");
  if (g_str_equal(format, "markdown"))
    return wig_mcp_scripts_load("content-markdown.js");
  return wig_mcp_scripts_load("content-text.js");
}

/* Compose the page-tree extractor as a function body for
 * webkit_web_view_call_async_javascript_function(). The pieces install
 * themselves on a namespace in the "wig-mcp" script world and are idempotent,
 * so re-sending them on every call costs a parse and avoids having to track
 * whether they are already present. The trailing statement calls them with the
 * `options` argument supplied by the caller. */
char *wig_mcp_scripts_tree(void)
{
  g_autofree char *registry = wig_mcp_scripts_load("node-registry.js");
  g_autofree char *walk = wig_mcp_scripts_load("tree-walk.js");
  g_autofree char *text = wig_mcp_scripts_load("tree-text.js");

  return g_strconcat(registry, "\n", walk, "\n", text, "\nreturn __wig.text(__wig.walk(options), options);\n", NULL);
}

char *wig_mcp_scripts_console_read(void)
{
  g_autofree char *read = wig_mcp_scripts_load("console-read.js");

  return g_strconcat(read, "\nreturn __wig.consoleRead(options);\n", NULL);
}

/* Compose the interaction runner as a function body. The node registry comes
 * along because interactions resolve the uids the extractor issued, and the
 * walker and renderer because an interaction can report the page it produced
 * without a second round trip. The actions arrive as a JSON string argument:
 * call_async_javascript_function() accepts only numbers, strings, and
 * dictionaries, so an array cannot be passed directly. */
char *wig_mcp_scripts_interaction(void)
{
  g_autofree char *registry = wig_mcp_scripts_load("node-registry.js");
  g_autofree char *walk = wig_mcp_scripts_load("tree-walk.js");
  g_autofree char *text = wig_mcp_scripts_load("tree-text.js");
  g_autofree char *interaction = wig_mcp_scripts_load("interaction.js");

  return g_strconcat(registry, "\n", walk, "\n", text, "\n", interaction,
                     "\nreturn __wig.interact(JSON.parse(actions), options);\n", NULL);
}
