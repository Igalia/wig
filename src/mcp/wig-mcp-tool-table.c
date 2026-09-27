/* SPDX-License-Identifier: MIT */

#include "wig-mcp-tool-table.h"

#include "wig-mcp-json.h"
#include "wig-mcp-result.h"

typedef struct {
  const char *name;
  const char *description;
  const char *schema;
} ToolDefinition;

/* Stringize a JSON schema so it can be written inline as real JSON rather than
 * an escaped C string literal. The preprocessor collapses whitespace and
 * escapes the embedded quotes. */
#define SCHEMA(...) #__VA_ARGS__

// clang-format off
static const ToolDefinition tools[] = {
  { "browser_console_messages", "Get buffered console messages for a tab, oldest first. Each entry has a source of "
                                "console, exception, or rejection for what the page produced, or network, security, "
                                "or other for messages the browser produced itself, such as failed loads and CSP "
                                "violations. clear discards only the messages actually returned, so a level filter or "
                                "limit never silently drops the rest.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "clear": { "type": "boolean", "default": false },
             "limit": { "type": "integer", "minimum": 1, "maximum": 1000, "default": 100 },
             "level_filter": { "type": "array",
                               "items": { "enum": ["debug", "log", "info", "warn", "error"] } } },
             "additionalProperties": false }) },
  { "browser_dialogs", "List or respond to pending JavaScript dialogs.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "dialog_id": { "type": "integer", "minimum": 1 },
             "action": { "enum": ["accept", "dismiss"] },
             "text": { "type": "string" } },
             "additionalProperties": false }) },
  { "close_tab", "Close a browser tab.",
    SCHEMA({ "type": "object", "properties": { "tab_handle": { "type": "integer", "minimum": 1 } },
             "required": ["tab_handle"], "additionalProperties": false }) },
  { "create_tab", "Create and select a tab, optionally loading a URL.",
    SCHEMA({ "type": "object", "properties": { "url": { "type": "string" } },
             "additionalProperties": false }) },
  { "evaluate_javascript", "Evaluate JavaScript in a tab and return its JSON representation.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "script": { "type": "string" } },
             "required": ["script"], "additionalProperties": false }) },
  { "get_network_request", "Get full captured network request details and response body.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "request_id": { "type": "integer", "minimum": 1 } },
             "required": ["request_id"], "additionalProperties": false }) },
  { "get_page_content", "Extract page content. The textTree format is an indented outline of the rendered page "
                        "in which every interactive element carries a uid that page_interactions can target; the "
                        "uid stays valid until the element leaves the document, so a page need not be re-extracted "
                        "before acting on it. The region, max_nodes, max_words_per_paragraph, and include_containers "
                        "options apply to textTree only.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "format": { "enum": ["textTree", "text", "html", "markdown", "json"], "default": "text" },
             "region": { "enum": ["viewport", "document"], "default": "viewport" },
             "max_nodes": { "type": "integer", "minimum": 1, "maximum": 20000, "default": 1500 },
             "max_words_per_paragraph": { "type": "integer", "minimum": 1, "maximum": 2000, "default": 30 },
             "include_containers": { "type": "boolean", "default": false } },
             "additionalProperties": false }) },
  { "list_network_requests", "List recent network request summaries for a tab, newest last. The buffer survives "
                             "navigations, and each summary carries the navigation that issued it. since and the "
                             "start field share units. clear discards only the requests actually returned, so a "
                             "filter or limit never silently drops the rest.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "clear": { "type": "boolean", "default": false },
             "limit": { "type": "integer", "minimum": 1, "maximum": 500 },
             "since": { "type": "number" },
             "filter": { "type": "object", "properties": {
               "url_substring": { "type": "string" },
               "method": { "type": "string" },
               "status_min": { "type": "integer", "minimum": 0 },
               "status_max": { "type": "integer", "minimum": 0 } },
               "additionalProperties": false } },
             "additionalProperties": false }) },
  { "list_tabs", "List all browser tabs. Each carries the window_handle of the window holding it, and active marks "
                 "the selected tab within that window, so one tab per window is active.",
    SCHEMA({ "type": "object", "additionalProperties": false }) },
  { "navigate_to_url", "Load a URL, wait for navigation, and return page information and text.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "url": { "type": "string" },
             "timeout": { "type": "number", "minimum": 0.1, "maximum": 30 } },
             "required": ["url"], "additionalProperties": false }) },
  { "page_info", "Return URL, title, loading state, progress, and tab handle.",
    SCHEMA({ "type": "object", "properties": { "tab_handle": { "type": "integer", "minimum": 1 } },
             "additionalProperties": false }) },
  { "page_interactions", "Run sequential click, type, focus, scroll, hover, keyPress, and selectOption actions, "
                         "stopping at the first failure. Target an element with node (a uid from get_page_content's "
                         "textTree format, which is preferred because it reports precisely why it failed), selector "
                         "(CSS, which cannot reach inside a shadow root), or text (visible text or accessible name). "
                         "scroll and keyPress act on the window or "
                         "the focused element when no target is given. The text to enter for a type action goes in "
                         "value. Unless return_content is none, the resulting page is returned as a second textTree "
                         "block, so there is no need to extract again to see what changed.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "return_content": { "enum": ["textTree", "none"], "default": "textTree" },
             "region": { "enum": ["viewport", "document"], "default": "viewport" },
             "actions": { "type": "array", "items": {
               "type": "object", "properties": {
                 "type": { "enum": ["click", "type", "focus", "scroll", "hover", "keyPress", "selectOption"] },
                 "node": { "type": "string" },
                 "selector": { "type": "string" },
                 "text": { "type": "string" },
                 "value": {},
                 "key": { "type": "string" },
                 "scrollToVisible": { "type": "boolean", "default": false },
                 "x": { "type": "number" },
                 "y": { "type": "number" } },
               "required": ["type"] } } },
             "required": ["actions"], "additionalProperties": false }) },
  { "screenshot", "Capture the visible viewport or full document as PNG.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "full_document": { "type": "boolean", "default": false } },
             "additionalProperties": false }) },
  { "set_viewport_size", "Resize the owning toplevel to approximate a requested CSS viewport.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "width": { "type": "integer", "minimum": 1, "maximum": 16384 },
             "height": { "type": "integer", "minimum": 1, "maximum": 16384 } },
             "required": ["width", "height"], "additionalProperties": false }) },
  { "switch_tab", "Select a browser tab.",
    SCHEMA({ "type": "object", "properties": { "tab_handle": { "type": "integer", "minimum": 1 } },
             "required": ["tab_handle"], "additionalProperties": false }) },
  { "wait_for_navigation", "Wait for the current navigation, or return immediately if idle.",
    SCHEMA({ "type": "object", "properties": {
             "tab_handle": { "type": "integer", "minimum": 1 },
             "timeout": { "type": "number", "minimum": 0.1, "maximum": 30 } },
             "additionalProperties": false }) },
};
// clang-format on

#undef SCHEMA

static const ToolDefinition *find_tool(const char *name)
{
  for (guint i = 0; i < G_N_ELEMENTS(tools); i++) {
    if (g_str_equal(name, tools[i].name))
      return &tools[i];
  }
  return NULL;
}

gboolean wig_mcp_tool_is_known(const char *name)
{
  return find_tool(name) != NULL;
}

/* FIXME: Only the top level is checked. */
gboolean wig_mcp_tool_reject_invalid_arguments(McpCall *call, const char *name, JsonObject *arguments)
{
  const ToolDefinition *tool = find_tool(name);
  g_autoptr(JsonNode) schema = wig_mcp_json_parse(tool->schema);
  if (!schema || !JSON_NODE_HOLDS_OBJECT(schema))
    return FALSE;

  JsonNode *required_node = json_object_get_member(json_node_get_object(schema), "required");
  JsonArray *required = required_node && JSON_NODE_HOLDS_ARRAY(required_node) ? json_node_get_array(required_node)
                                                                              : NULL;
  for (guint i = 0; required && i < json_array_get_length(required); i++) {
    const char *member = json_array_get_string_element(required, i);
    if (arguments && json_object_has_member(arguments, member))
      continue;

    g_autofree char *text = g_strdup_printf("%s requires \"%s\"", name, member);
    wig_mcp_result_send_text(call, text, TRUE);
    return TRUE;
  }

  if (!arguments)
    return FALSE;

  /* A schema with no properties at all, such as list_tabs, accepts nothing. */
  JsonNode *properties_node = json_object_get_member(json_node_get_object(schema), "properties");
  JsonObject *properties = properties_node && JSON_NODE_HOLDS_OBJECT(properties_node)
      ? json_node_get_object(properties_node)
      : NULL;

  g_autoptr(GList) members = json_object_get_members(arguments);
  for (GList *l = members; l; l = l->next) {
    const char *member = l->data;
    if (properties && json_object_has_member(properties, member))
      continue;

    g_autoptr(GList) known = properties ? json_object_get_members(properties) : NULL;
    g_autoptr(GString) accepted = g_string_new(NULL);
    for (GList *k = known; k; k = k->next) {
      if (accepted->len)
        g_string_append(accepted, ", ");
      g_string_append(accepted, k->data);
    }

    g_autofree char *text = g_strdup_printf("%s does not accept \"%s\". Accepted arguments: %s", name, member,
                                            accepted->len ? accepted->str : "none");
    wig_mcp_result_send_text(call, text, TRUE);
    return TRUE;
  }

  return FALSE;
}

JsonNode *wig_mcp_tool_list_result(void)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "tools");

  json_builder_begin_array(builder);
  for (guint i = 0; i < G_N_ELEMENTS(tools); i++) {
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, tools[i].name);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(builder, tools[i].description);
    json_builder_set_member_name(builder, "inputSchema");
    g_autoptr(JsonNode) schema = wig_mcp_json_parse(tools[i].schema);
    json_builder_add_value(builder, g_steal_pointer(&schema));
    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);

  json_builder_end_object(builder);
  return wig_mcp_json_builder_take_root(builder);
}
