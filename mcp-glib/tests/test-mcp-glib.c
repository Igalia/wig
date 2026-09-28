/* SPDX-License-Identifier: MIT */

#include "mcp-glib.h"

#include <string.h>

typedef struct {
  JsonNode *response;
  McpTransportOutcome outcome;
  gboolean pending;
} TestTransport;

typedef struct {
  McpCall *call;
  gulong cancelled_id;
} PendingMethod;

typedef struct {
  GMainLoop *loop;
  GString *output;
  guint added;
  guint changed;
  guint removed;
  int exit_status;
  gboolean closed;
  gboolean transport_present_at_remove;
  gboolean stop_on_remove;
  gboolean notify_when_active;
  gboolean notified;
} StdioFixture;

static void test_transport_clear(TestTransport *transport)
{
  g_clear_pointer(&transport->response, json_node_unref);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(TestTransport, test_transport_clear)

static gboolean write_stdio(McpSession *session, const char *message, gsize length, gpointer user_data, GError **error)
{
  StdioFixture *fixture = user_data;
  g_string_append_len(fixture->output, message, (gssize)length);
  return TRUE;
}

static void on_stdio_closed(McpSession *session, int exit_status, const GError *error, gpointer user_data)
{
  StdioFixture *fixture = user_data;
  g_assert_null(error);
  fixture->exit_status = exit_status;
  fixture->closed = TRUE;
  g_main_loop_quit(fixture->loop);
}

static void on_server_event(McpServer *server, McpServerEvent event, McpSession *session, gpointer user_data)
{
  StdioFixture *fixture = user_data;
  if (event == MCP_SERVER_EVENT_SESSION_ADDED)
    fixture->added++;
  else if (event == MCP_SERVER_EVENT_SESSION_CHANGED) {
    fixture->changed++;
    if (fixture->notify_when_active && !fixture->notified && mcp_session_get_state(session) == MCP_SESSION_ACTIVE) {
      fixture->notified = TRUE;
      g_autoptr(JsonBuilder) builder = json_builder_new();
      json_builder_begin_object(builder);
      json_builder_set_member_name(builder, "uri");
      json_builder_add_string_value(builder, "wig://console/1");
      json_builder_end_object(builder);
      g_autoptr(JsonNode) params = json_builder_get_root(builder);

      g_autoptr(GError) error = NULL;
      g_assert_true(mcp_session_send_notification(session, "notifications/resources/updated", params, &error));
      g_assert_no_error(error);
    }
  } else if (event == MCP_SERVER_EVENT_SESSION_REMOVED) {
    fixture->removed++;
    fixture->transport_present_at_remove = mcp_server_has_transport(server, MCP_TRANSPORT_STDIO);
    if (fixture->stop_on_remove)
      mcp_server_stop(server);
  }
}

static void on_complete(McpTransport *transport, McpTransportOutcome outcome, JsonNode *response, gpointer user_data)
{
  TestTransport *test = user_data;
  test->outcome = outcome;
  if (response)
    test->response = json_node_copy(response);
}

static void on_pending(McpTransport *transport, gboolean pending, gpointer user_data)
{
  TestTransport *test = user_data;
  test->pending = pending;
}

static JsonObject *new_capabilities(void)
{
  JsonObject *capabilities = json_object_new();
  JsonObject *tools = json_object_new();
  json_object_set_boolean_member(tools, "listChanged", FALSE);
  json_object_set_object_member(capabilities, "tools", tools);
  return capabilities;
}

static void send_message(McpServer *server, McpSession *session, const char *message, TestTransport *result)
{
  g_autoptr(McpTransport) transport = mcp_transport_new(mcp_session_get_transport_kind(session), on_complete,
                                                        on_pending, result, NULL);
  mcp_server_handle_message(server, session, transport, message, -1);
}

static void assert_error_code(JsonNode *response, gint code)
{
  g_assert_true(JSON_NODE_HOLDS_OBJECT(response));
  JsonObject *object = json_node_get_object(response);
  JsonObject *error = json_object_get_object_member(object, "error");
  g_assert_cmpint(json_object_get_int_member(error, "code"), ==, code);
}

static void initialize_session(McpServer *server, McpSession *session)
{
  g_auto(TestTransport) response = { 0 };
  send_message(server, session,
               "{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"initialize\",\"params\":{"
               "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{"
               "\"name\":\"test\",\"version\":\"1\"}}}",
               &response);
  g_assert_nonnull(response.response);
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_AWAITING_INITIALIZED);

  g_clear_pointer(&response.response, json_node_unref);
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", &response);
  g_assert_null(response.response);
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_ACTIVE);
}

static void test_lifecycle(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) invalid_session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);

  g_auto(TestTransport) response = { 0 };
  send_message(server, invalid_session, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", &response);
  assert_error_code(response.response, -32002);
  g_assert_cmpint(mcp_session_get_state(invalid_session), ==, MCP_SESSION_NEW);

  /* A rejected request before initialization leaves the session new, so a
   * client that probed first can still initialize. */
  g_clear_pointer(&response.response, json_node_unref);
  initialize_session(server, invalid_session);

  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  initialize_session(server, session);
  g_assert_cmpstr(mcp_session_get_client_name(session), ==, "test");
  g_assert_cmpstr(mcp_session_get_client_version(session), ==, "1");
}

static void test_pre_initialize_probe(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);

  /* A client negotiating a newer protocol revision probes with an unknown
   * method before falling back to initialization. */
  g_auto(TestTransport) probe = { 0 };
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\"}", &probe);
  assert_error_code(probe.response, -32002);
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_NEW);

  initialize_session(server, session);
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_ACTIVE);
}

typedef struct {
  guint state;
  guint protocol_version;
  guint client_name;
  guint client_version;
} NotifyCounts;

static void count_notify(GObject *object, GParamSpec *pspec, guint *count)
{
  (*count)++;
}

static void test_session_properties(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  NotifyCounts counts = { 0 };

  g_signal_connect(session, "notify::state", G_CALLBACK(count_notify), &counts.state);
  g_signal_connect(session, "notify::protocol-version", G_CALLBACK(count_notify), &counts.protocol_version);
  g_signal_connect(session, "notify::client-name", G_CALLBACK(count_notify), &counts.client_name);
  g_signal_connect(session, "notify::client-version", G_CALLBACK(count_notify), &counts.client_version);
  initialize_session(server, session);

  g_assert_cmpuint(counts.client_name, ==, 1);
  g_assert_cmpuint(counts.client_version, ==, 1);
  g_assert_cmpuint(counts.protocol_version, ==, 1);
  g_assert_cmpuint(counts.state, ==, 2);

  McpSessionState state = MCP_SESSION_NEW;
  g_autofree char *client_name = NULL;
  g_object_get(session, "state", &state, "client-name", &client_name, NULL);
  g_assert_cmpint(state, ==, MCP_SESSION_ACTIVE);
  g_assert_cmpstr(client_name, ==, "test");

  mcp_session_close(session);
  g_assert_cmpuint(counts.state, ==, 3);
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_CLOSED);
}

static void test_batch_rejected(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);

  /* A batch arriving before initialize also leaves the session new. */
  g_autoptr(McpSession) new_session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  g_auto(TestTransport) new_response = { 0 };
  send_message(server, new_session,
               "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
               "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{"
               "\"name\":\"test\",\"version\":\"1\"}}}]",
               &new_response);
  assert_error_code(new_response.response, -32600);
  g_assert_cmpint(new_response.outcome, ==, MCP_TRANSPORT_OUTCOME_INVALID_INPUT);
  g_assert_cmpint(mcp_session_get_state(new_session), ==, MCP_SESSION_NEW);

  /* An established session rejects the batch but stays usable. */
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  initialize_session(server, session);
  g_auto(TestTransport) response = { 0 };
  send_message(server, session, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]", &response);
  assert_error_code(response.response, -32600);
  g_assert_cmpint(response.outcome, ==, MCP_TRANSPORT_OUTCOME_INVALID_INPUT);
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_ACTIVE);
}

static void test_ping(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);

  g_auto(TestTransport) early = { 0 };
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"ping\"}", &early);
  JsonObject *early_object = json_node_get_object(early.response);
  g_assert_cmpint(json_object_get_int_member(early_object, "id"), ==, 41);
  g_assert_true(JSON_NODE_HOLDS_OBJECT(json_object_get_member(early_object, "result")));
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_NEW);

  initialize_session(server, session);

  g_auto(TestTransport) response = { 0 };
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"ping\"}", &response);
  JsonObject *object = json_node_get_object(response.response);
  g_assert_cmpint(json_object_get_int_member(object, "id"), ==, 42);
  g_assert_true(JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "result")));
}

static void test_invalid_initialize(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  g_auto(TestTransport) response = { 0 };

  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}", &response);
  assert_error_code(response.response, -32602);
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_NEW);
}

static McpMethodDisposition handle_pending(McpCall *call, gpointer user_data)
{
  PendingMethod *pending = user_data;
  pending->call = mcp_call_ref(call);
  return MCP_METHOD_PENDING;
}

static void complete_from_cancelled(GCancellable *cancellable, gpointer user_data)
{
  PendingMethod *pending = user_data;
  pending->cancelled_id = 0;

  g_autoptr(JsonNode) result = NULL;
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_end_object(builder);
  result = json_builder_get_root(builder);
  mcp_call_return_result(pending->call, result);
  g_clear_pointer(&pending->call, mcp_call_unref);
}

static McpMethodDisposition handle_cancel_reentrancy(McpCall *call, gpointer user_data)
{
  PendingMethod *pending = user_data;
  pending->call = mcp_call_ref(call);
  pending->cancelled_id = g_cancellable_connect(mcp_call_get_cancellable(call), G_CALLBACK(complete_from_cancelled),
                                                pending, NULL);
  return MCP_METHOD_PENDING;
}

static void test_async_pending(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  initialize_session(server, session);

  PendingMethod pending = { 0 };
  g_assert_true(mcp_server_add_method(server, "test/pending", handle_pending, &pending, NULL, NULL));

  g_auto(TestTransport) response = { 0 };
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"test/pending\"}", &response);

  g_assert_true(response.pending);
  g_assert_null(response.response);
  g_assert_nonnull(pending.call);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "done");
  json_builder_add_boolean_value(builder, TRUE);
  json_builder_end_object(builder);
  g_autoptr(JsonNode) result = json_builder_get_root(builder);
  g_assert_true(mcp_call_return_result(pending.call, result));
  g_clear_pointer(&pending.call, mcp_call_unref);

  g_assert_false(response.pending);
  g_assert_nonnull(response.response);
  g_assert_true(JSON_NODE_HOLDS_OBJECT(response.response));
}

static void test_cancellation_reentrancy(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  initialize_session(server, session);

  PendingMethod pending = { 0 };
  g_assert_true(mcp_server_add_method(server, "test/cancel", handle_cancel_reentrancy, &pending, NULL, NULL));

  g_auto(TestTransport) request = { 0 };
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"test/cancel\"}", &request);
  g_assert_true(request.pending);

  g_auto(TestTransport) notification = { 0 };
  send_message(server, session,
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":8}}",
               &notification);

  g_assert_false(request.pending);
  g_assert_null(request.response);
  g_assert_cmpint(request.outcome, ==, MCP_TRANSPORT_OUTCOME_ACCEPTED);
  g_assert_null(pending.call);
  g_assert_null(notification.response);
}

static void test_duplicate_request_id(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  initialize_session(server, session);

  g_auto(TestTransport) response = { 0 };
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}", &response);
  g_assert_nonnull(response.response);

  g_clear_pointer(&response.response, json_node_unref);
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}", &response);
  assert_error_code(response.response, -32600);
}

static void test_cancellation_sends_no_response(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  initialize_session(server, session);

  PendingMethod pending = { 0 };
  g_assert_true(mcp_server_add_method(server, "test/pending", handle_pending, &pending, NULL, NULL));

  g_auto(TestTransport) request = { 0 };
  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"test/pending\"}", &request);
  g_assert_true(request.pending);

  g_auto(TestTransport) notification = { 0 };
  send_message(server, session,
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":8}}",
               &notification);

  g_assert_false(request.pending);
  g_assert_cmpint(request.outcome, ==, MCP_TRANSPORT_OUTCOME_ACCEPTED);
  g_assert_null(request.response);
  g_assert_true(mcp_call_is_completed(pending.call));
  g_clear_pointer(&pending.call, mcp_call_unref);
}

static void test_invalid_request_preserves_id(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  g_auto(TestTransport) response = { 0 };

  send_message(server, session, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"initialize\",\"params\":[]}", &response);

  assert_error_code(response.response, -32600);
  g_assert_cmpint(json_object_get_int_member(json_node_get_object(response.response), "id"), ==, 7);
}

static void test_initialize_detection(void)
{
  g_assert_true(
      mcp_message_is_initialize("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"initialize\",\"params\":[]}", -1));
  g_assert_true(mcp_message_is_initialize("{\"jsonrpc\":\"2.0\",\"id\":\"request\",\"method\":\"initialize\"}", -1));
  g_assert_false(mcp_message_is_initialize("[{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"initialize\"}]", -1));
  g_assert_false(mcp_message_is_initialize("{\"jsonrpc\":\"2.0\",\"method\":\"initialize\"}", -1));
  g_assert_false(mcp_message_is_initialize("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}", -1));
}

static void test_request_metadata(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  initialize_session(server, session);

  const char *invalid_messages[] = {
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"_meta\":false}}",
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\",\"params\":{\"_meta\":{\"progressToken\":true}}}",
  };

  for (guint i = 0; i < G_N_ELEMENTS(invalid_messages); i++) {
    g_auto(TestTransport) response = { 0 };
    send_message(server, session, invalid_messages[i], &response);
    assert_error_code(response.response, -32602);
  }

  g_auto(TestTransport) valid_response = { 0 };
  send_message(server, session,
               "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\",\"params\":{"
               "\"_meta\":{\"progressToken\":\"progress\"}}}",
               &valid_response);
  g_assert_true(
      JSON_NODE_HOLDS_OBJECT(json_object_get_member(json_node_get_object(valid_response.response), "result")));

  g_autoptr(McpSession) notification_session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  g_auto(TestTransport) initialize_response = { 0 };
  send_message(server, notification_session,
               "{\"jsonrpc\":\"2.0\",\"id\":200,\"method\":\"initialize\",\"params\":{"
               "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{"
               "\"name\":\"test\",\"version\":\"1\"}}}",
               &initialize_response);

  g_auto(TestTransport) notification_response = { 0 };
  send_message(server, notification_session,
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{"
               "\"_meta\":{\"progressToken\":true}}}",
               &notification_response);
  g_assert_cmpint(mcp_session_get_state(notification_session), ==, MCP_SESSION_ACTIVE);
}

static void test_invalid_capabilities(void)
{
  const char *capabilities[] = {
    "{\"roots\":true}",
    "{\"roots\":{\"listChanged\":\"yes\"}}",
    "{\"sampling\":true}",
    "{\"experimental\":{\"feature\":true}}",
  };

  for (guint i = 0; i < G_N_ELEMENTS(capabilities); i++) {
    g_autoptr(JsonObject) server_capabilities = new_capabilities();
    g_autoptr(McpServer) server = mcp_server_new("test-server", "1", server_capabilities);
    g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
    g_auto(TestTransport) response = { 0 };
    g_autofree char *message = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
                                               "\"protocolVersion\":\"2025-11-25\",\"capabilities\":%s,\"clientInfo\":{"
                                               "\"name\":\"test\",\"version\":\"1\"}}}",
                                               capabilities[i]);

    send_message(server, session, message, &response);
    assert_error_code(response.response, -32602);
    g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_NEW);
  }
}

static void test_version_negotiation(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  g_auto(TestTransport) response = { 0 };

  send_message(server, session,
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
               "\"protocolVersion\":\"unsupported\",\"capabilities\":{},\"clientInfo\":{"
               "\"name\":\"test\",\"version\":\"1\"}}}",
               &response);

  JsonObject *result = json_object_get_object_member(json_node_get_object(response.response), "result");
  g_assert_cmpstr(json_object_get_string_member(result, "protocolVersion"), ==, MCP_PROTOCOL_VERSION);
}

static void test_invalid_ids(void)
{
  const char *messages[] = {
    "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"initialize\"}",
    "{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"initialize\"}",
    "{\"jsonrpc\":\"2.0\",\"id\":true,\"method\":\"initialize\"}",
  };

  for (guint i = 0; i < G_N_ELEMENTS(messages); i++) {
    g_autoptr(JsonObject) capabilities = new_capabilities();
    g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
    g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
    g_auto(TestTransport) response = { 0 };

    send_message(server, session, messages[i], &response);
    assert_error_code(response.response, -32600);
  }
}

static void test_parse_error(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(McpSession) session = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  g_auto(TestTransport) response = { 0 };

  send_message(server, session, "{", &response);
  assert_error_code(response.response, -32700);
  g_assert_cmpint(response.outcome, ==, MCP_TRANSPORT_OUTCOME_RESPONSE);
}

static void test_notification(void)
{
  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);

  /* A session with no transport behind it has nowhere to write. */
  g_autoptr(McpSession) detached = mcp_session_new(MCP_TRANSPORT_STDIO, NULL);
  initialize_session(server, detached);
  g_autoptr(GError) detached_error = NULL;
  g_assert_false(mcp_session_send_notification(detached, "notifications/resources/updated", NULL, &detached_error));
  g_assert_error(detached_error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);

  static const char messages[]
      = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":"
        "\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"stdio-test\",\"version\":\"1\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";

  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  g_autoptr(GString) output = g_string_new(NULL);
  StdioFixture fixture = { .loop = loop, .output = output, .stop_on_remove = TRUE, .notify_when_active = TRUE };
  mcp_server_set_event_func(server, on_server_event, &fixture, NULL);

  g_autoptr(GInputStream) input = g_memory_input_stream_new_from_data(messages, -1, NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(McpSession) session = mcp_server_add_stdio(server, input, write_stdio, on_stdio_closed, &fixture, NULL,
                                                       &error);
  g_assert_no_error(error);
  g_main_loop_run(loop);

  g_assert_true(fixture.notified);
  g_assert_nonnull(strstr(output->str, "\"method\":\"notifications/resources/updated\""));
  g_assert_nonnull(strstr(output->str, "wig://console/1"));

  /* A notification carries no id, or a client would try to correlate it. */
  const char *line = strstr(output->str, "notifications/resources/updated");
  const char *start = line;
  while (start > output->str && start[-1] != '\n')
    start--;
  g_autofree char *notification = g_strndup(start, (gsize)(strchr(start, '\n') - start));
  g_assert_null(strstr(notification, "\"id\""));

  /* The transport is gone once the session closes, so nothing is written to a
   * freed connection. */
  g_autoptr(GError) closed_error = NULL;
  g_assert_false(mcp_session_send_notification(session, "notifications/resources/updated", NULL, &closed_error));
  g_assert_error(closed_error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED);
}

static void test_managed_stdio(void)
{
  static const char messages[]
      = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":"
        "\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"stdio-test\",\"version\":\"1\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n";

  g_autoptr(JsonObject) capabilities = new_capabilities();
  g_autoptr(McpServer) server = mcp_server_new("test-server", "1", capabilities);
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  g_autoptr(GString) output = g_string_new(NULL);
  StdioFixture fixture = { .loop = loop, .output = output, .stop_on_remove = TRUE };
  mcp_server_set_event_func(server, on_server_event, &fixture, NULL);

  g_autoptr(GInputStream) input = g_memory_input_stream_new_from_data(messages, -1, NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(McpSession) session = mcp_server_add_stdio(server, input, write_stdio, on_stdio_closed, &fixture, NULL,
                                                       &error);
  g_assert_no_error(error);
  g_assert_nonnull(session);
  g_assert_cmpuint(mcp_session_get_serial(session), >, 0);
  g_assert_cmpint(mcp_session_get_transport_kind(session), ==, MCP_TRANSPORT_STDIO);

  g_main_loop_run(loop);

  g_assert_true(fixture.closed);
  g_assert_cmpint(fixture.exit_status, ==, 0);
  g_assert_cmpuint(fixture.added, ==, 1);
  g_assert_cmpuint(fixture.changed, ==, 2);
  g_assert_cmpuint(fixture.removed, ==, 1);
  g_assert_false(fixture.transport_present_at_remove);
  g_assert_cmpint(mcp_session_get_state(session), ==, MCP_SESSION_CLOSED);
  g_assert_nonnull(strstr(output->str, "\"id\":1"));
  g_assert_nonnull(strstr(output->str, "\"id\":2"));
  g_assert_null(strstr(output->str, "notifications/initialized"));

  g_autoptr(GPtrArray) sessions = mcp_server_dup_sessions(server);
  g_assert_cmpuint(sessions->len, ==, 0);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/mcp/lifecycle", test_lifecycle);
  g_test_add_func("/mcp/pre-initialize-probe", test_pre_initialize_probe);
  g_test_add_func("/mcp/session-properties", test_session_properties);
  g_test_add_func("/mcp/batch-rejected", test_batch_rejected);
  g_test_add_func("/mcp/ping", test_ping);
  g_test_add_func("/mcp/notification", test_notification);
  g_test_add_func("/mcp/invalid-initialize", test_invalid_initialize);
  g_test_add_func("/mcp/async-pending", test_async_pending);
  g_test_add_func("/mcp/cancellation-reentrancy", test_cancellation_reentrancy);
  g_test_add_func("/mcp/cancellation-no-response", test_cancellation_sends_no_response);
  g_test_add_func("/mcp/duplicate-request-id", test_duplicate_request_id);
  g_test_add_func("/mcp/invalid-request-preserves-id", test_invalid_request_preserves_id);
  g_test_add_func("/mcp/initialize-detection", test_initialize_detection);
  g_test_add_func("/mcp/request-metadata", test_request_metadata);
  g_test_add_func("/mcp/invalid-capabilities", test_invalid_capabilities);
  g_test_add_func("/mcp/version-negotiation", test_version_negotiation);
  g_test_add_func("/mcp/invalid-ids", test_invalid_ids);
  g_test_add_func("/mcp/parse-error", test_parse_error);
  g_test_add_func("/mcp/transports/stdio", test_managed_stdio);

  return g_test_run();
}
