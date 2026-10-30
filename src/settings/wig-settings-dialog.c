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

#include "wig-settings-dialog.h"

#include "wig-application.h"
#include "wig-settings-features.h"
#include "wig-settings-filters.h"
#include "wig-settings-mcp.h"
#include "wig-settings-memory.h"
#include "wig-settings-permissions.h"
#include "wig-settings-proxies.h"
#include "wig-settings-rows.h"
#include "wig-settings-search.h"
#include "wig-settings-user-content.h"
#include "wig-settings-web-apps.h"
#include "wig-settings-website-data.h"

#define WIG_SETTINGS_DIALOG_TITLE "Settings"

struct _WigSettingsDialog {
  AdwDialog parent;

  GtkWidget *split;
  AdwNavigationPage *content_page;
  GtkWidget *stack;
  GtkWidget *content;
  GtkWidget *search;
  GtkWidget *search_bar;
};

G_DEFINE_FINAL_TYPE(WigSettingsDialog, wig_settings_dialog, ADW_TYPE_DIALOG)

/* What a row needs to be put somewhere and found again. */
typedef struct {
  WigSettingsDialog *dialog;
  AdwPreferencesGroup *group;
  const char *name;
  const char *title;
} Pane;

/* Each pane is a page of its own in the switcher, so the group inside it carries
 * no title of its own until there is a second one to tell apart. A switcher
 * button falls back to the broken image icon, so every pane names one. */
static void wig_settings_dialog_new_pane(WigSettingsDialog *self, Pane *pane, const char *name, const char *title,
                                         const char *icon_name)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

  pane->dialog = self;
  pane->group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  pane->name = name;
  pane->title = title;

  adw_preferences_page_add(page, pane->group);
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), GTK_WIDGET(page), name, title, icon_name);
}

/* Searching looks through the settings wherever they are, so what each row says
 * is offered along with the pane it would be found in. */
static void pane_index(Pane *pane, GtkWidget *row)
{
  const char *description = ADW_IS_ACTION_ROW(row) ? adw_action_row_get_subtitle(ADW_ACTION_ROW(row)) : NULL;

  wig_settings_search_add(WIG_SETTINGS_SEARCH(pane->dialog->search),
                          adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), description, pane->name,
                          pane->title);
}

static void pane_add(Pane *pane, GtkWidget *row)
{
  adw_preferences_group_add(pane->group, row);
  pane_index(pane, row);
}

static void wig_settings_dialog_add_browsing_pane(WigSettingsDialog *self, GSettings *settings)
{
  static const char *tab_layout_nicks[] = { "horizontal", "vertical", NULL };
  static const char *const tab_layout_labels[] = { "Tab Bar", "Sidebar", NULL };
  static const char *unload_nicks[] = { "never", "15-minutes", "30-minutes", "1-hour", "6-hours", NULL };
  static const char *const unload_labels[] = {
    "Never", "After 15 Minutes", "After 30 Minutes", "After 1 Hour", "After 6 Hours", NULL,
  };

  Pane pane;
  wig_settings_dialog_new_pane(self, &pane, "browser", "Browsing", "web-browser-symbolic");

  pane_add(&pane,
           wig_settings_switch_row_new(settings, "restore-tabs", "Restore Tabs on Startup",
                                       "Open the windows and tabs that were open when wig was last closed."));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "new-windows-in-tabs", "Open New Windows in Tabs",
                                       "Show a window a page opens as a tab beside it. Pop-ups that set their "
                                       "own size or hide the toolbars still get a window."));
  pane_add(&pane,
           wig_settings_combo_row_new(settings, "tab-layout", "Tab Layout",
                                      "Show tabs in a bar above the page or in a sidebar beside it.", tab_layout_nicks,
                                      tab_layout_labels));
  pane_add(
      &pane,
      wig_settings_combo_row_new(
          settings, "unload-unused-tabs", "Unload Unused Tabs",
          "Reduce memory usage by unloading tabs. Ignores active and pinned tabs or ones with audio, microphone, etc.",
          unload_nicks, unload_labels));

#if HAVE_UPGRADE_TO_HTTPS_POLICY_SUPPORT
  static const char *https_nicks[] = { "keep-as-requested", "https-first", "https-only", NULL };
  static const char *const https_labels[] = { "Keep as Requested", "Automatic Fallback to HTTP", "Error on Failure",
                                              NULL };

  pane_add(&pane,
           wig_settings_combo_row_new(settings, "https-navigation-policy", "Upgrade to HTTPS",
                                      "Upgrade addresses typed as http to https. Automatic Fallback to HTTP "
                                      "quietly loads the http address when the secure load fails; Error on "
                                      "Failure shows an error instead.",
                                      https_nicks, https_labels));
#endif

  /* The template it reveals is part of the same setting, so only the row naming
   * the engine is worth finding. */
  pane_index(&pane, wig_settings_search_engine_rows_add(pane.group, settings));

  pane_add(&pane,
           wig_settings_switch_row_new(settings, "enable-developer-extras", "Developer Tools",
                                       "Offer the web inspector in the context menu of a page."));
  pane_add(&pane, wig_settings_entry_row_new(settings, "user-agent", "User Agent"));
}

static void wig_settings_dialog_add_content_pane(WigSettingsDialog *self, GSettings *settings)
{
  Pane pane;
  wig_settings_dialog_new_pane(self, &pane, "content", "Page Content", "text-x-generic-symbolic");

  pane_add(&pane,
           wig_settings_switch_row_new(settings, "enable-javascript", "JavaScript", "Run the scripts a page carries."));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "auto-load-images", "Load Images",
                                       "Fetch the images a page asks for. Turning this off saves data and "
                                       "leaves the rest of the page as it is."));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "javascript-can-open-windows-automatically", "Allow Pop-up Windows",
                                       "Let a page open a window on its own, rather than only when a click "
                                       "asks for one."));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "media-playback-requires-user-gesture",
                                       "Require a Click Before Media Plays",
                                       "Hold audio and video until they are started, instead of letting a "
                                       "page play them on its own."));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "zoom-text-only", "Zoom Text Only",
                                       "Resize only the text when zooming, leaving images and layout at "
                                       "their own size."));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "enable-webgl", "WebGL",
                                       "Let a page render WebGL, used for games and other 3D software typically."));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "enable-media", "Audio and Video",
                                       "Toggle support for all audio and video."));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "enable-encrypted-media", "Protected Content",
                                       "Play media requiring a DRM module (modules must be provided)"));
  pane_add(&pane,
           wig_settings_switch_row_new(settings, "enable-media-stream", "Camera and Microphone",
                                       "Let a page ask for the camera or the microphone. Each site still has "
                                       "to be allowed separately."));
  pane_add(&pane,
           wig_settings_switch_row_new(
               settings, "enable-webrtc", "WebRTC",
               "Enable WebRTC often used for conferencing software, exposes some network information."));
}

/* The feature lists are pages of their own rather than rows in a group, so they
 * go in beside the panes rather than through one. */
static void wig_settings_dialog_add_features_pane(WigSettingsDialog *self, WigFeaturesKind kind, const char *name,
                                                  const char *title, const char *icon_name)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_features_new(kind), name, title,
                                      icon_name);
  /* The rows wait to be looked at, but what they say can be searched at once. */
  wig_settings_features_index(kind, WIG_SETTINGS_SEARCH(self->search), name, title);
}

static void wig_settings_dialog_add_features_panes(WigSettingsDialog *self)
{
  wig_settings_dialog_add_features_pane(self, WIG_FEATURES_EXPERIMENTAL, "features", "Experimental Features",
                                        "applications-science-symbolic");
  wig_settings_dialog_add_features_pane(self, WIG_FEATURES_DEVELOPMENT, "developer-features", "Developer Features",
                                        "applications-engineering-symbolic");
}

static void wig_settings_dialog_add_memory_pane(WigSettingsDialog *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_memory_new(), "memory-limits",
                                      "Memory Limits", "memory-symbolic");
  wig_settings_memory_index(WIG_SETTINGS_SEARCH(self->search), "memory-limits", "Memory Limits");
}

static void wig_settings_dialog_add_filters_pane(WigSettingsDialog *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_filters_new(), "content-filters",
                                      "Content Filters", "security-high-symbolic");
}

static void wig_settings_dialog_add_user_content_panes(WigSettingsDialog *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack),
                                      wig_settings_user_content_new(WIG_USER_CONTENT_SCRIPTS), "user-scripts",
                                      "User Scripts", "application-x-executable-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack),
                                      wig_settings_user_content_new(WIG_USER_CONTENT_STYLES), "user-styles",
                                      "User Styles", "preferences-desktop-appearance-symbolic");
}

static void wig_settings_dialog_add_permissions_pane(WigSettingsDialog *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_permissions_new(), "permissions",
                                      "Permissions", "channel-secure-symbolic");
  wig_settings_permissions_index(WIG_SETTINGS_SEARCH(self->search), "permissions", "Permissions");
}

static void wig_settings_dialog_add_proxies_pane(WigSettingsDialog *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_proxies_new(), "proxies", "Proxies",
                                      "network-server-symbolic");
  wig_settings_proxies_index(WIG_SETTINGS_SEARCH(self->search), "proxies", "Proxies");
}

static void wig_settings_dialog_add_web_apps_pane(WigSettingsDialog *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_web_apps_new(), "web-apps", "Web Apps",
                                      "view-app-grid-symbolic");
  wig_settings_web_apps_index(WIG_SETTINGS_SEARCH(self->search), "web-apps", "Web Apps");
}

static void wig_settings_dialog_add_website_data_pane(WigSettingsDialog *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_website_data_new(), "website-data",
                                      "Website Data", "drive-harddisk-symbolic");
  wig_settings_website_data_index(WIG_SETTINGS_SEARCH(self->search), "website-data", "Website Data");
}

static void wig_settings_dialog_add_mcp_pane(WigSettingsDialog *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_mcp_new(), "mcp", "MCP Server",
                                      "network-transmit-receive-symbolic");
  wig_settings_mcp_index(WIG_SETTINGS_SEARCH(self->search), "mcp", "MCP Server");
}

static void wig_settings_dialog_visible_pane_changed(WigSettingsDialog *self)
{
  GtkWidget *child = adw_view_stack_get_visible_child(ADW_VIEW_STACK(self->stack));
  if (!child)
    return;

  AdwViewStackPage *page = adw_view_stack_get_page(ADW_VIEW_STACK(self->stack), child);
  adw_navigation_page_set_title(self->content_page, adw_view_stack_page_get_title(page));
}

static void wig_settings_dialog_set_searching(WigSettingsDialog *self, gboolean searching)
{
  gtk_stack_set_visible_child_name(GTK_STACK(self->content), searching ? "search" : "panes");
}

static void wig_settings_dialog_search_changed(WigSettingsDialog *self, GtkSearchEntry *entry)
{
  const char *terms = gtk_editable_get_text(GTK_EDITABLE(entry));

  /* An empty search is the settings as they were, not a search with everything
   * in it. */
  wig_settings_dialog_set_searching(self, *terms != '\0');
  wig_settings_search_set_terms(WIG_SETTINGS_SEARCH(self->search), terms);
}

static void wig_settings_dialog_search_mode_changed(WigSettingsDialog *self)
{
  if (gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(self->search_bar)))
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(self->split), TRUE);
  else
    wig_settings_dialog_set_searching(self, FALSE);
}

/* A result stands for a row in a pane, so following one goes to the pane and
 * leaves the search behind. */
static void wig_settings_dialog_search_activated(WigSettingsDialog *self, const char *pane)
{
  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar), FALSE);
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->stack), pane);
}

static void wig_settings_dialog_sidebar_activated(WigSettingsDialog *self)
{
  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar), FALSE);
  adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(self->split), TRUE);
}

static gboolean wig_settings_dialog_start_search(GtkWidget *widget, GVariant *args, gpointer user_data)
{
  WigSettingsDialog *self = WIG_SETTINGS_DIALOG(widget);

  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar), TRUE);
  return TRUE;
}

static AdwNavigationPage *wig_settings_dialog_build_sidebar(WigSettingsDialog *self, GtkWidget *sidebar)
{
  adw_view_switcher_sidebar_set_stack(ADW_VIEW_SWITCHER_SIDEBAR(sidebar), ADW_VIEW_STACK(self->stack));
  g_signal_connect_object(sidebar, "activated", G_CALLBACK(wig_settings_dialog_sidebar_activated), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), sidebar);

  return adw_navigation_page_new(toolbar, WIG_SETTINGS_DIALOG_TITLE);
}

static AdwNavigationPage *wig_settings_dialog_build_content(WigSettingsDialog *self)
{
  /* Searching takes the place of the panes rather than sitting beside them, so
   * what is on screen is either everything or what was asked for. */
  self->content = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->content), self->stack, "panes");
  gtk_stack_add_named(GTK_STACK(self->content), self->search, "search");

  GtkWidget *entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(entry, TRUE);
  g_signal_connect_object(entry, "search-changed", G_CALLBACK(wig_settings_dialog_search_changed), self,
                          G_CONNECT_SWAPPED);

  /* The rows are held to a readable width by a clamp of their own, and the
   * entry searching them lines up with them rather than with the window. */
  GtkWidget *entry_clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(entry_clamp), entry);

  self->search_bar = gtk_search_bar_new();
  gtk_search_bar_set_child(GTK_SEARCH_BAR(self->search_bar), entry_clamp);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(self->search_bar), GTK_EDITABLE(entry));
  gtk_search_bar_set_key_capture_widget(GTK_SEARCH_BAR(self->search_bar), GTK_WIDGET(self));
  g_signal_connect_object(self->search_bar, "notify::search-mode-enabled",
                          G_CALLBACK(wig_settings_dialog_search_mode_changed), self, G_CONNECT_SWAPPED);

  GtkWidget *search_button = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(search_button), "edit-find-symbolic");
  gtk_widget_set_tooltip_text(search_button, "Search");
  g_object_bind_property(search_button, "active", self->search_bar, "search-mode-enabled",
                         G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), search_button);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), self->search_bar);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->content);

  self->content_page = adw_navigation_page_new(toolbar, WIG_SETTINGS_DIALOG_TITLE);
  return self->content_page;
}

static void wig_settings_dialog_class_init(WigSettingsDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  gtk_widget_class_add_binding(widget_class, GDK_KEY_f, GDK_CONTROL_MASK, wig_settings_dialog_start_search, NULL);
}

static void wig_settings_dialog_init(WigSettingsDialog *self)
{
  GSettings *settings = wig_application_get_settings(wig_application_get());

  self->search = wig_settings_search_new();
  g_signal_connect_object(self->search, "activated", G_CALLBACK(wig_settings_dialog_search_activated), self,
                          G_CONNECT_SWAPPED);

  self->stack = adw_view_stack_new();
  wig_settings_dialog_add_browsing_pane(self, settings);
  wig_settings_dialog_add_content_pane(self, settings);
  wig_settings_dialog_add_features_panes(self);
  wig_settings_dialog_add_filters_pane(self);
  wig_settings_dialog_add_user_content_panes(self);
  wig_settings_dialog_add_permissions_pane(self);
  wig_settings_dialog_add_proxies_pane(self);
  wig_settings_dialog_add_web_apps_pane(self);
  wig_settings_dialog_add_website_data_pane(self);
  wig_settings_dialog_add_memory_pane(self);
  wig_settings_dialog_add_mcp_pane(self);
  g_signal_connect_object(self->stack, "notify::visible-child", G_CALLBACK(wig_settings_dialog_visible_pane_changed),
                          self, G_CONNECT_SWAPPED);

  GtkWidget *sidebar = adw_view_switcher_sidebar_new();
  self->split = adw_navigation_split_view_new();
  adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(self->split),
                                        wig_settings_dialog_build_sidebar(self, sidebar));
  adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(self->split),
                                        wig_settings_dialog_build_content(self));
  wig_settings_dialog_visible_pane_changed(self);

  AdwBreakpoint *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600sp"));
  adw_breakpoint_add_setters(breakpoint, G_OBJECT(self->split), "collapsed", TRUE, G_OBJECT(sidebar), "mode",
                             ADW_SIDEBAR_MODE_PAGE, NULL);
  adw_dialog_add_breakpoint(ADW_DIALOG(self), breakpoint);

  adw_dialog_set_title(ADW_DIALOG(self), WIG_SETTINGS_DIALOG_TITLE);
  adw_dialog_set_content_width(ADW_DIALOG(self), 900);
  adw_dialog_set_content_height(ADW_DIALOG(self), 640);
  gtk_widget_set_size_request(GTK_WIDGET(self), 360, 294);
  adw_dialog_set_child(ADW_DIALOG(self), self->split);
}

AdwDialog *wig_settings_dialog_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_DIALOG, NULL);
}
