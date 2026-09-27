/* SPDX-License-Identifier: MIT */

#include "wig-mcp-server.h"

#include "wig-application.h"
#include "wig-mcp-navigation.h"
#include "wig-mcp-network.h"
#include "wig-mcp-scripts.h"
#include "wig-mcp-tools.h"
#include "wig-tab-list.h"
#include "wig-tab.h"
#include "wig-window.h"

void wig_mcp_server_pending_dialog_free(PendingDialog *pending)
{
  webkit_script_dialog_close(pending->dialog);
  webkit_script_dialog_unref(pending->dialog);
  g_free(pending);
}

static void on_console_cleanup_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
  if (!value && error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug("mcp: failed to remove console instrumentation: %s", error->message);
}

static void watch_existing_views(WigMcpServer *self)
{
  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(self->application)); l; l = l->next) {
    WigTabList *tab_list = wig_window_get_tab_list(WIG_WINDOW(l->data));
    for (guint i = 0; i < wig_tab_list_get_n_tabs(tab_list); i++) {
      WebKitWebView *web_view = wig_tab_get_web_view(wig_tab_list_get_nth(tab_list, i));
      wig_mcp_server_watch_view(self, web_view);
    }
  }
}

static void remove_instrumentation(WigMcpServer *self)
{
  self->instrumentation_active = FALSE;
  g_debug("mcp: removing instrumentation from %u views", g_hash_table_size(self->views));

  if (self->content_manager && self->console_script) {
    webkit_user_content_manager_remove_script(self->content_manager, self->console_script);
    g_clear_pointer(&self->console_script, webkit_user_script_unref);
  }

  g_autofree char *cleanup_source = wig_mcp_scripts_load("console-uninstall.js");
  GHashTableIter iter;
  gpointer web_view = NULL;
  g_hash_table_iter_init(&iter, self->views);
  while (g_hash_table_iter_next(&iter, &web_view, NULL)) {
    webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(web_view), cleanup_source, -1, NULL, NULL, NULL,
                                        on_console_cleanup_done, NULL);
  }

  wig_mcp_navigation_cancel_all(self);
  g_queue_clear_full(&self->dialogs, (GDestroyNotify)wig_mcp_server_pending_dialog_free);
  g_hash_table_remove_all(self->views);

  g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  if (!self->stopped)
    self->cancellable = g_cancellable_new();
}

void wig_mcp_server_update_instrumentation(WigMcpServer *self)
{
  g_return_if_fail(self != NULL);

  gboolean should_be_active = !self->stopped && mcp_server_has_transport(self->protocol_server, MCP_TRANSPORT_STDIO);
  if (!wig_mcp_server_handles_dialogs(self))
    g_queue_clear_full(&self->dialogs, (GDestroyNotify)wig_mcp_server_pending_dialog_free);
  if (should_be_active == self->instrumentation_active)
    return;

  if (!should_be_active) {
    remove_instrumentation(self);
    return;
  }

  self->instrumentation_active = TRUE;
  g_autofree char *console_source = wig_mcp_scripts_load("console.js");
  self->console_script = webkit_user_script_new(console_source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
  webkit_user_content_manager_add_script(self->content_manager, self->console_script);
  watch_existing_views(self);
  g_debug("mcp: instrumented %u views", g_hash_table_size(self->views));
}

gboolean wig_mcp_server_handles_dialogs(WigMcpServer *self)
{
  g_return_val_if_fail(self != NULL, FALSE);
  return !self->stopped && mcp_server_has_active_session(self->protocol_server, MCP_TRANSPORT_STDIO);
}

void wig_mcp_server_stop(WigMcpServer *self)
{
  g_return_if_fail(self != NULL);
  if (self->stopped)
    return;

  self->stopped = TRUE;
  g_cancellable_cancel(self->cancellable);
  mcp_server_stop(self->protocol_server);
  g_list_store_remove_all(self->sessions);
  wig_mcp_server_update_instrumentation(self);
}

WigMcpServer *wig_mcp_server_ref(WigMcpServer *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_atomic_ref_count_inc(&self->ref_count);
  return self;
}

void wig_mcp_server_unref(WigMcpServer *self)
{
  g_return_if_fail(self != NULL);
  if (!g_atomic_ref_count_dec(&self->ref_count))
    return;

  wig_mcp_server_stop(self);
  if (self->load_failed_hook_id)
    g_signal_remove_emission_hook(g_signal_lookup("load-failed", WEBKIT_TYPE_WEB_VIEW), self->load_failed_hook_id);
  g_clear_pointer(&self->protocol_server, mcp_server_unref);
  g_clear_object(&self->content_manager);
  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->views, g_hash_table_unref);
  g_clear_object(&self->sessions);
  g_free(self);
}

GListModel *wig_mcp_server_get_sessions(WigMcpServer *self)
{
  return G_LIST_MODEL(self->sessions);
}

static void on_protocol_server_event(McpServer *server, McpServerEvent event, McpSession *session, gpointer user_data)
{
  WigMcpServer *self = user_data;
  guint position = 0;

  if (event == MCP_SERVER_EVENT_SESSION_ADDED)
    g_list_store_append(self->sessions, session);
  else if (event == MCP_SERVER_EVENT_SESSION_REMOVED && g_list_store_find(self->sessions, session, &position))
    g_list_store_remove(self->sessions, position);

  wig_mcp_server_update_instrumentation(self);
}

WigMcpServer *wig_mcp_server_new(WigApplication *application, WebKitUserContentManager *content_manager)
{
  g_return_val_if_fail(WIG_IS_APPLICATION(application), NULL);
  g_return_val_if_fail(WEBKIT_IS_USER_CONTENT_MANAGER(content_manager), NULL);

  WigMcpServer *self = g_new0(WigMcpServer, 1);
  g_atomic_ref_count_init(&self->ref_count);
  self->application = application;
  self->content_manager = g_object_ref(content_manager);
  self->views = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, wig_mcp_network_view_state_free);
  self->sessions = g_list_store_new(MCP_TYPE_SESSION);

  g_autoptr(JsonObject) capabilities = json_object_new();
  JsonObject *tools = json_object_new();
  json_object_set_boolean_member(tools, "listChanged", FALSE);
  json_object_set_object_member(capabilities, "tools", tools);
  self->protocol_server = mcp_server_new("wig", "0.0.1", capabilities);

  g_autoptr(GError) error = NULL;
  if (!mcp_server_add_method(self->protocol_server, "tools/list", wig_mcp_tools_handle_list, self, NULL, &error))
    g_error("Failed to register tools/list: %s", error->message);

  if (!mcp_server_add_method(self->protocol_server, "tools/call", wig_mcp_tools_handle_call, self, NULL, &error))
    g_error("Failed to register tools/call: %s", error->message);

  self->cancellable = g_cancellable_new();
  g_queue_init(&self->dialogs);
  mcp_server_set_event_func(self->protocol_server, on_protocol_server_event, self, NULL);

  g_autoptr(GTypeClass) web_view_class = g_type_class_ref(WEBKIT_TYPE_WEB_VIEW);
  self->load_failed_hook_id = g_signal_add_emission_hook(g_signal_lookup("load-failed", WEBKIT_TYPE_WEB_VIEW), 0,
                                                         wig_mcp_network_record_load_failure, self, NULL);
  return self;
}
