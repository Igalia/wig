/*
 * Copyright (c) 2026 Igalia S.L.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wig-settings-mcp.h"

#include "wig-application.h"
#include "wig-flatpak.h"
#include "wig-mcp-server.h"

#include <adwaita.h>

#define CLIENT_TITLE "MCP Client"
#define CLIENT_DESCRIPTION "The program that will connect to wig, which decides the command to run."

static const char *const client_labels[] = { "Claude Code", "Codex", "OpenCode", "Other", NULL };

static const struct {
  const char *prefix;
  const char *description;
} clients[] = {
  { "claude mcp add -s user wig -- ", "Run this in a terminal to add wig to Claude Code for every project." },
  { "codex mcp add wig -- ", "Run this in a terminal to add wig to Codex." },
  { "opencode mcp add wig -- ", "Run this in a terminal to add wig to OpenCode's global configuration." },
  { "", "Configure the MCP client to run this command." },
};

struct _WigSettingsMcp {
  GtkWidget parent;

  GtkWidget *page;
  AdwPreferencesGroup *connecting;
  GtkWidget *client;
  GtkWidget *command;
  char *wig_command;
};

G_DEFINE_FINAL_TYPE(WigSettingsMcp, wig_settings_mcp, GTK_TYPE_WIDGET)

static const char *transport_name(McpTransportKind kind)
{
  if (kind == MCP_TRANSPORT_STDIO)
    return "stdio";
  return "custom";
}

static const char *session_state_name(McpSessionState state)
{
  switch (state) {
  case MCP_SESSION_NEW:
    return "Connecting";
  case MCP_SESSION_AWAITING_INITIALIZED:
    return "Initializing";
  case MCP_SESSION_ACTIVE:
    return "Active";
  case MCP_SESSION_CLOSED:
    return "Closed";
  }
  g_assert_not_reached();
}

static void mcp_disconnect_clicked(GtkButton *button, McpSession *session)
{
  g_debug("mcp: disconnecting session %" G_GUINT64_FORMAT " from settings", mcp_session_get_serial(session));
  mcp_session_close(session);
}

static void mcp_session_row_sync(McpSession *session, GParamSpec *pspec, AdwActionRow *row)
{
  const char *name = mcp_session_get_client_name(session);
  const char *version = mcp_session_get_client_version(session);
  const char *protocol_id = mcp_session_get_id(session);

  g_autoptr(GStrvBuilder) details = g_strv_builder_new();
  if (version)
    g_strv_builder_add(details, version);
  g_strv_builder_add(details, transport_name(mcp_session_get_transport_kind(session)));
  g_strv_builder_add(details, session_state_name(mcp_session_get_state(session)));
  if (protocol_id)
    g_strv_builder_add(details, protocol_id);
  g_auto(GStrv) parts = g_strv_builder_end(details);
  g_autofree char *subtitle = g_strjoinv(" · ", parts);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name ? name : "stdio client");
  adw_action_row_set_subtitle(row, subtitle);
}

static GtkWidget *mcp_session_row_new(gpointer item, gpointer user_data)
{
  McpSession *session = MCP_SESSION(item);

  g_debug("mcp: settings row for session %" G_GUINT64_FORMAT, mcp_session_get_serial(session));

  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);

  GtkWidget *disconnect = gtk_button_new_with_label("Disconnect");
  gtk_widget_set_valign(disconnect, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(disconnect, "destructive-action");
  g_signal_connect_object(disconnect, "clicked", G_CALLBACK(mcp_disconnect_clicked), session, G_CONNECT_DEFAULT);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), disconnect);

  g_signal_connect_object(session, "notify", G_CALLBACK(mcp_session_row_sync), row, G_CONNECT_DEFAULT);
  mcp_session_row_sync(session, NULL, ADW_ACTION_ROW(row));

  return row;
}

static char *mcp_file_id(const char *path)
{
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_ID_FILE, G_FILE_QUERY_INFO_NONE, NULL, NULL);

  return info ? g_strdup(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILE)) : NULL;
}

static gboolean mcp_is_same_file(const char *first, const char *second)
{
  g_autofree char *first_id = mcp_file_id(first);
  g_autofree char *second_id = mcp_file_id(second);

  return first_id && g_strcmp0(first_id, second_id) == 0;
}

static char *mcp_wig_command(void)
{
  if (wig_in_flatpak()) {
    g_autofree char *flatpak_id = wig_flatpak_dup_id();
    return flatpak_id ? g_strdup_printf("flatpak run %s --mcp-stdio", flatpak_id) : g_strdup("wig --mcp-stdio");
  }

  g_autofree char *executable = g_file_read_link("/proc/self/exe", NULL);
  if (!executable)
    return g_strdup("wig --mcp-stdio");

  g_autofree char *in_path = g_find_program_in_path("wig");
  if (in_path && mcp_is_same_file(in_path, executable))
    return g_strdup("wig --mcp-stdio");

  g_autofree char *quoted = strpbrk(executable, " \t'\"\\$`!&;|<>()*?[]{}#~") ? g_shell_quote(executable)
                                                                              : g_strdup(executable);
  return g_strconcat(quoted, " --mcp-stdio", NULL);
}

static void mcp_sync_command(WigSettingsMcp *self)
{
  guint client = adw_combo_row_get_selected(ADW_COMBO_ROW(self->client));
  if (client >= G_N_ELEMENTS(clients))
    client = G_N_ELEMENTS(clients) - 1;

  g_autofree char *command = g_strconcat(clients[client].prefix, self->wig_command, NULL);

  g_debug("mcp: suggesting '%s'", command);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->command), command);
  adw_preferences_group_set_description(self->connecting, clients[client].description);
}

static void mcp_copy_command(WigSettingsMcp *self)
{
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)),
                         adw_preferences_row_get_title(ADW_PREFERENCES_ROW(self->command)));
}

void wig_settings_mcp_index(WigSettingsSearch *search, const char *pane, const char *pane_title)
{
  wig_settings_search_add(search, CLIENT_TITLE, CLIENT_DESCRIPTION, pane, pane_title);
  wig_settings_search_add(search, "Sessions", "The MCP clients connected to this browser.", pane, pane_title);
}

static void wig_settings_mcp_dispose(GObject *object)
{
  WigSettingsMcp *self = WIG_SETTINGS_MCP(object);

  g_clear_pointer(&self->page, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_settings_mcp_parent_class)->dispose(object);
}

static void wig_settings_mcp_finalize(GObject *object)
{
  WigSettingsMcp *self = WIG_SETTINGS_MCP(object);

  g_clear_pointer(&self->wig_command, g_free);

  G_OBJECT_CLASS(wig_settings_mcp_parent_class)->finalize(object);
}

static void wig_settings_mcp_class_init(WigSettingsMcpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_mcp_dispose;
  object_class->finalize = wig_settings_mcp_finalize;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-mcp");
}

static void wig_settings_mcp_init(WigSettingsMcp *self)
{
  WigMcpServer *server = wig_application_get_mcp_server(wig_application_get());

  self->wig_command = mcp_wig_command();

  self->page = adw_preferences_page_new();
  adw_preferences_page_set_description(ADW_PREFERENCES_PAGE(self->page),
                                       "Control the Model Context Protocol server that exposes this browser for "
                                       "automation.");
  gtk_widget_set_parent(self->page, GTK_WIDGET(self));

  AdwPreferencesGroup *connecting = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  self->connecting = connecting;
  adw_preferences_group_set_title(connecting, "Connecting");

  g_autoptr(GtkStringList) client_model = gtk_string_list_new(client_labels);
  self->client = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->client), CLIENT_TITLE);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(self->client), CLIENT_DESCRIPTION);
  adw_combo_row_set_model(ADW_COMBO_ROW(self->client), G_LIST_MODEL(client_model));
  adw_combo_row_set_selected(ADW_COMBO_ROW(self->client), G_N_ELEMENTS(clients) - 1);
  adw_preferences_group_add(connecting, self->client);

  self->command = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->command), FALSE);
  adw_preferences_row_set_title_selectable(ADW_PREFERENCES_ROW(self->command), TRUE);
  gtk_widget_add_css_class(self->command, "monospace");

  GtkWidget *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
  gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(copy, "Copy Command");
  gtk_widget_add_css_class(copy, "flat");
  g_signal_connect_object(copy, "clicked", G_CALLBACK(mcp_copy_command), self, G_CONNECT_SWAPPED);
  adw_action_row_add_suffix(ADW_ACTION_ROW(self->command), copy);

  adw_preferences_group_add(connecting, self->command);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), connecting);

  g_signal_connect_object(self->client, "notify::selected", G_CALLBACK(mcp_sync_command), self, G_CONNECT_SWAPPED);
  mcp_sync_command(self);

  GtkWidget *empty = gtk_label_new("No MCP sessions are connected");
  gtk_widget_add_css_class(empty, "dim-label");
  gtk_widget_set_margin_top(empty, 12);
  gtk_widget_set_margin_bottom(empty, 12);

  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_list_box_set_placeholder(GTK_LIST_BOX(list), empty);
  gtk_widget_add_css_class(list, "boxed-list");
  if (server)
    gtk_list_box_bind_model(GTK_LIST_BOX(list), wig_mcp_server_get_sessions(server), mcp_session_row_new, NULL, NULL);

  AdwPreferencesGroup *sessions = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(sessions, "Sessions");
  adw_preferences_group_add(sessions, list);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), sessions);
}

GtkWidget *wig_settings_mcp_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_MCP, NULL);
}
