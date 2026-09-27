/* SPDX-License-Identifier: MIT */

#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <mcp-glib.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

#define MCP_NAVIGATION_TIMEOUT_SECONDS 30
#define MCP_MAX_NETWORK_RECORDS 500
#define MCP_MAX_CONSOLE_MESSAGES 500
#define MCP_MAX_NETWORK_BODY_BYTES (10 * 1024 * 1024)

/* Ceilings on a single tool result. Without these, a page's full HTML, a script
 * returning a large structure, or a full-document screenshot can each return
 * more than a client can hold. Text is truncated with a marker; results that
 * must stay parseable report an error instead, since a JSON document cut in
 * half is worse than none. */
#define MCP_MAX_TEXT_RESULT_BYTES (256 * 1024)
#define MCP_MAX_SCREENSHOT_BYTES (4 * 1024 * 1024)

/* Defaults for get_page_content's textTree format. The renderer stops at
 * MCP_TREE_MAX_BYTES and reports how many nodes it kept, which tells a client
 * far more than the blunt ceiling above. The gap leaves room for the header
 * line so the rendered result always lands under MCP_MAX_TEXT_RESULT_BYTES and
 * a client never sees two truncation notices for one result. */
#define MCP_TREE_MAX_NODES 1500
#define MCP_TREE_MAX_WORDS_PER_PARAGRAPH 30
#define MCP_TREE_MAX_BYTES (MCP_MAX_TEXT_RESULT_BYTES - 1024)

/* page_interactions reports the page it produced. That is a progress report
 * rather than a deliberate extraction, so it is held to a smaller budget and a
 * caller who wants the whole tree asks get_page_content for it. */
#define MCP_TREE_INTERACTION_MAX_BYTES (64 * 1024)

typedef struct _WigApplication WigApplication;
typedef struct _WigMcpServer WigMcpServer;

typedef struct {
  guint64 id;
  WebKitScriptDialog *dialog;
  WebKitWebView *web_view; /* weak */
} PendingDialog;

typedef struct _NetworkRecord NetworkRecord;

struct _NetworkRecord {
  guint64 id;
  guint64 generation; /* the navigation that issued it; the buffer outlives loads */
  WebKitWebResource *resource;
  char *url;
  char *method;
  char *mime_type;
  char *request_headers;
  char *response_headers;
  char *error;
  guint status;
  gint64 start_us;
  gint64 end_us;
  gulong sent_request_id;
  gulong response_id;
  gulong failed_id;
  gulong finished_id;
};

/* A console message the page could not have seen: a CSP violation, a mixed
 * content warning, a failed resource load. The web process extension forwards
 * these, since only the browser itself produces them. */
typedef struct {
  guint64 id;
  char *source;
  char *level;
  char *text;
  char *source_id;
  guint line;
  gint64 time_us;
} ConsoleMessage;

typedef struct _ViewState ViewState;

struct _ViewState {
  WigMcpServer *server; /* weak */
  WebKitWebView *web_view; /* weak */
  GQueue records;
  GQueue console_messages; /* owned ConsoleMessage* */
  guint64 next_console_id;
  guint64 navigation_generation;
  guint64 committed_navigation_generation;
  guint64 failed_navigation_generation;
  char *failure;
  gulong load_changed_id;
  gulong resource_started_id;
  gulong script_dialog_id;
  gulong user_message_id;
};

struct _WigMcpServer {
  gatomicrefcount ref_count;

  WigApplication *application; /* weak; application owns the server */
  McpServer *protocol_server;
  WebKitUserContentManager *content_manager;
  WebKitUserScript *console_script;
  GCancellable *cancellable;
  GHashTable *views; /* weak WebKitWebView* -> ViewState* */
  GListStore *sessions;
  GQueue dialogs;
  GList *navigation_calls; /* weak NavigationCall* */
  GList *javascript_calls; /* weak JavascriptCall* */
  gulong load_failed_hook_id;
  guint64 next_request_id;
  guint64 next_dialog_id;
  gboolean instrumentation_active;
  gboolean stopped;
};

WigMcpServer *wig_mcp_server_new(WigApplication *application, WebKitUserContentManager *content_manager);
WigMcpServer *wig_mcp_server_ref(WigMcpServer *self);
void wig_mcp_server_unref(WigMcpServer *self);
gboolean wig_mcp_server_start_stdio(WigMcpServer *self, GApplicationCommandLine *command_line, GError **error);
void wig_mcp_server_stop(WigMcpServer *self);
void wig_mcp_server_watch_view(WigMcpServer *self, WebKitWebView *web_view);
void wig_mcp_server_update_instrumentation(WigMcpServer *self);
gboolean wig_mcp_server_handles_dialogs(WigMcpServer *self);

GListModel *wig_mcp_server_get_sessions(WigMcpServer *self);

void wig_mcp_server_pending_dialog_free(PendingDialog *pending);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WigMcpServer, wig_mcp_server_unref)

G_END_DECLS
