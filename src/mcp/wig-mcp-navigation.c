/* SPDX-License-Identifier: MIT */

#include "wig-mcp-navigation.h"

#include <math.h>

#include "wig-mcp-json.h"
#include "wig-mcp-network.h"
#include "wig-mcp-result.h"

typedef struct {
  WigMcpServer *server;
  WigMcpCall *call;
  GCancellable *cancellable;
  GWeakRef web_view;
  guint tab_handle;
  gulong load_changed_id;
  gulong load_failed_id;
  gulong cancelled_id;
  guint timeout_id;
  guint64 expected_generation;
  /* While bind_on_start is set, expected_generation is not yet meaningful: it
   * is adopted from the load this call is actually waiting for. skip_inflight
   * says that an unrelated load was already running when the wait began and
   * must be allowed to finish first. See wig_mcp_navigation_start(). */
  gboolean bind_on_start;
  gboolean skip_inflight;
  gboolean follow_latest;
  gboolean include_text;
  gboolean completed;
} NavigationCall;

static void navigation_call_free(NavigationCall *navigation)
{
  navigation->server->navigation_calls = g_list_remove(navigation->server->navigation_calls, navigation);
  WebKitWebView *web_view = g_weak_ref_get(&navigation->web_view);
  if (web_view) {
    if (navigation->load_changed_id)
      g_signal_handler_disconnect(web_view, navigation->load_changed_id);
    if (navigation->load_failed_id)
      g_signal_handler_disconnect(web_view, navigation->load_failed_id);
    g_object_unref(web_view);
  }

  if (navigation->timeout_id)
    g_source_remove(navigation->timeout_id);

  if (navigation->cancelled_id) {
    g_cancellable_disconnect(navigation->cancellable, navigation->cancelled_id);
  }

  g_clear_object(&navigation->cancellable);
  g_weak_ref_clear(&navigation->web_view);
  wig_mcp_server_unref(navigation->server);
  g_free(navigation);
}

void wig_mcp_navigation_cancel_all(WigMcpServer *self)
{
  while (TRUE) {
    NavigationCall *navigation = NULL;
    for (GList *l = self->navigation_calls; l; l = l->next) {
      NavigationCall *candidate = l->data;
      if (!candidate->completed) {
        navigation = candidate;
        break;
      }
    }
    if (!navigation)
      return;

    navigation->completed = TRUE;
    wig_mcp_call_finish_text(navigation->call, "The MCP transport was closed", TRUE);
    navigation_call_free(navigation);
  }
}

static void on_navigation_text_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  NavigationCall *navigation = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
  if (!value) {
    wig_mcp_call_finish_text(navigation->call, error->message, TRUE);
    navigation_call_free(navigation);
    return;
  }

  g_autoptr(JSCValue) title_value = jsc_value_object_get_property_at_index(value, 0);
  g_autoptr(JSCValue) text_value = jsc_value_object_get_property_at_index(value, 1);
  g_autofree char *title = jsc_value_is_string(title_value) ? jsc_value_to_string(title_value) : NULL;
  g_autofree char *text = jsc_value_is_string(text_value) ? jsc_value_to_string(text_value) : NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "tab_handle");
  json_builder_add_int_value(builder, navigation->tab_handle);
  wig_mcp_json_add_nullable_string(builder, "url", webkit_web_view_get_uri(WEBKIT_WEB_VIEW(source)));
  wig_mcp_json_add_nullable_string(builder, "title", title && *title ? title : NULL);
  json_builder_set_member_name(builder, "loading");
  json_builder_add_boolean_value(builder, webkit_web_view_is_loading(WEBKIT_WEB_VIEW(source)));
  json_builder_set_member_name(builder, "progress");
  json_builder_add_double_value(builder, webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(source)));
  if (navigation->include_text) {
    json_builder_set_member_name(builder, "text");
    json_builder_add_string_value(builder, text ? text : "");
  }
  json_builder_end_object(builder);

  g_autoptr(JsonNode) node = wig_mcp_json_builder_take_root(builder);
  g_autofree char *json = wig_mcp_json_node_to_string(node);
  wig_mcp_call_finish_text(navigation->call, json, FALSE);
  navigation_call_free(navigation);
}

static void navigation_call_complete(NavigationCall *navigation)
{
  if (navigation->completed)
    return;

  navigation->completed = TRUE;
  WebKitWebView *web_view = g_weak_ref_get(&navigation->web_view);
  if (!web_view) {
    wig_mcp_call_finish_text(navigation->call, "The tab was closed while waiting for navigation", TRUE);
    navigation_call_free(navigation);
    return;
  }

  g_clear_signal_handler(&navigation->load_changed_id, web_view);
  g_clear_signal_handler(&navigation->load_failed_id, web_view);
  g_clear_handle_id(&navigation->timeout_id, g_source_remove);

  GCancellable *cancellable = mcp_call_get_cancellable(navigation->call->protocol_call);
  const char *script = navigation->include_text ? "[document.title, document.body ? document.body.innerText : '']"
                                                : "[document.title, null]";
  webkit_web_view_evaluate_javascript(web_view, script, -1, NULL, "wig-mcp://navigation", cancellable,
                                      on_navigation_text_ready, navigation);
  g_object_unref(web_view);
}

static void on_navigation_timeout(gpointer user_data)
{
  NavigationCall *navigation = user_data;
  navigation->timeout_id = 0;
  if (!navigation->completed) {
    navigation->completed = TRUE;
    wig_mcp_call_finish_text(navigation->call, "Navigation timed out", TRUE);
    navigation_call_free(navigation);
  }
}

static void on_navigation_cancelled(GCancellable *cancellable, gpointer user_data)
{
  NavigationCall *navigation = user_data;
  navigation->cancelled_id = 0;
  if (navigation->completed)
    return;

  navigation->completed = TRUE;
  wig_mcp_call_free(navigation->call);
  navigation_call_free(navigation);
}

static gboolean navigation_call_matches_generation(NavigationCall *navigation, WebKitWebView *web_view)
{
  ViewState *state = wig_mcp_network_get_view_state(navigation->server, web_view);
  if (!state || state->navigation_generation < navigation->expected_generation)
    return FALSE;
  if (state->navigation_generation == navigation->expected_generation)
    return TRUE;
  if (navigation->follow_latest)
    return FALSE;

  navigation->completed = TRUE;
  wig_mcp_call_finish_text(navigation->call, "Navigation was superseded by a newer load", TRUE);
  navigation_call_free(navigation);
  return FALSE;
}

static void on_navigation_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, NavigationCall *navigation)
{
  if (navigation->completed)
    return;

  if (navigation->follow_latest && load_event == WEBKIT_LOAD_STARTED) {
    ViewState *state = wig_mcp_network_get_view_state(navigation->server, web_view);
    navigation->expected_generation = state ? state->navigation_generation : 0;
    g_debug("mcp: waiting on the load that just started, generation %" G_GUINT64_FORMAT,
            navigation->expected_generation);
    return;
  }

  /* The generation is adopted from the load being waited on. Which load that is
   * cannot be settled in advance: a tab created to serve this call is still
   * loading about:blank, and satisfying the wait on that one reports a blank
   * page.
   *
   * Nor can the load be recognised by URI. At WEBKIT_LOAD_STARTED the view
   * still reports the previous document's URI, and the new one appears only at
   * WEBKIT_LOAD_COMMITTED, by which point a redirected load no longer carries
   * the URI that was requested. What remains is order: let a running load
   * finish, and take the next one to start. */
  if (navigation->bind_on_start) {
    if (navigation->skip_inflight) {
      if (load_event == WEBKIT_LOAD_FINISHED)
        navigation->skip_inflight = FALSE;
      return;
    }
    if (load_event != WEBKIT_LOAD_STARTED)
      return;

    ViewState *state = wig_mcp_network_get_view_state(navigation->server, web_view);
    navigation->expected_generation = state ? state->navigation_generation : 0;
    navigation->bind_on_start = FALSE;
    g_debug("mcp: navigation bound to generation %" G_GUINT64_FORMAT, navigation->expected_generation);
    return;
  }

  if (!navigation_call_matches_generation(navigation, web_view))
    return;
  if (load_event != WEBKIT_LOAD_FINISHED)
    return;

  ViewState *state = wig_mcp_network_get_view_state(navigation->server, web_view);
  if (!state || state->committed_navigation_generation == navigation->expected_generation) {
    navigation_call_complete(navigation);
    return;
  }

  gboolean recorded = state->failure && state->failed_navigation_generation == navigation->expected_generation;
  navigation->completed = TRUE;
  wig_mcp_call_finish_text(navigation->call, recorded ? state->failure : "Navigation did not load a page", TRUE);
  navigation_call_free(navigation);
}

static gboolean on_navigation_load_failed(WebKitWebView *web_view, WebKitLoadEvent load_event, const char *failing_uri,
                                          GError *error, NavigationCall *navigation)
{
  if (navigation->completed)
    return FALSE;

  /* A load that was already running when the wait began is not this call's to
   * report, but it does clear the way for the one that is. */
  if (navigation->bind_on_start) {
    navigation->skip_inflight = FALSE;
    return FALSE;
  }

  if (!navigation_call_matches_generation(navigation, web_view))
    return FALSE;
  navigation->completed = TRUE;
  g_autofree char *message = g_strdup_printf("Navigation to %s failed: %s", failing_uri, error->message);
  wig_mcp_call_finish_text(navigation->call, message, TRUE);
  navigation_call_free(navigation);
  return FALSE;
}

void wig_mcp_navigation_dialog_opened(WigMcpServer *self, WebKitWebView *web_view, const char *message)
{
  for (GList *l = self->navigation_calls; l;) {
    NavigationCall *navigation = l->data;
    l = l->next;
    if (navigation->completed)
      continue;

    g_autoptr(WebKitWebView) waiting_on = g_weak_ref_get(&navigation->web_view);
    if (waiting_on != web_view)
      continue;

    navigation->completed = TRUE;
    wig_mcp_call_finish_text(navigation->call, message, FALSE);
    navigation_call_free(navigation);
  }
}

/* Set `starts_a_load` when the caller is about to begin a navigation, so the
 * wait binds to that load once it starts. Leave it clear to wait on whatever
 * load is already in flight. */
void wig_mcp_navigation_start(WigMcpServer *self, McpCall *protocol_call, WigTab *tab, double timeout,
                              gboolean include_text, gboolean starts_a_load)
{
  WebKitWebView *web_view = wig_tab_get_web_view(tab);
  NavigationCall *navigation = g_new0(NavigationCall, 1);
  navigation->server = wig_mcp_server_ref(self);
  navigation->call = wig_mcp_call_new(self, protocol_call, web_view);
  navigation->tab_handle = wig_tab_get_id(tab);
  navigation->include_text = include_text;
  navigation->bind_on_start = starts_a_load;
  navigation->follow_latest = !starts_a_load;
  navigation->skip_inflight = starts_a_load && webkit_web_view_is_loading(web_view);
  ViewState *state = wig_mcp_network_get_view_state(self, web_view);
  navigation->expected_generation = state ? state->navigation_generation : 0;
  g_weak_ref_init(&navigation->web_view, web_view);
  self->navigation_calls = g_list_prepend(self->navigation_calls, navigation);
  if (!webkit_web_view_is_loading(web_view) && !starts_a_load) {
    navigation_call_complete(navigation);
    return;
  }

  navigation->load_changed_id = g_signal_connect(web_view, "load-changed", G_CALLBACK(on_navigation_load_changed),
                                                 navigation);
  navigation->load_failed_id = g_signal_connect(web_view, "load-failed", G_CALLBACK(on_navigation_load_failed),
                                                navigation);
  navigation->timeout_id = g_timeout_add_once((guint)ceil(timeout * 1000.0), on_navigation_timeout, navigation);

  navigation->cancellable = g_object_ref(mcp_call_get_cancellable(protocol_call));
  gulong cancelled_id = g_cancellable_connect(navigation->cancellable, G_CALLBACK(on_navigation_cancelled), navigation,
                                              NULL);
  if (cancelled_id)
    navigation->cancelled_id = cancelled_id;
}
