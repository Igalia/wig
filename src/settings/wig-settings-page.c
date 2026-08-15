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

#include "wig-settings-page.h"

#include "wig-application.h"
#include "wig-settings-rows.h"

#include <adwaita.h>

struct _WigSettingsPage {
  WigNativePage parent;

  /* Holds the one page for now, and is what a settings row drills into. */
  GtkWidget *navigation;
  GtkWidget *stack;
  /* Set while an address is choosing the pane, so the pane it lands on does not
   * rewrite the address that asked for it. */
  gboolean applying_uri;
};

G_DEFINE_FINAL_TYPE(WigSettingsPage, wig_settings_page, WIG_TYPE_NATIVE_PAGE)

/* Returns where the pane name starts within the path, empty for the settings
 * page as a whole, or NULL if the path is not the settings page at all. */
static const char *settings_path_pane(const char *path)
{
  if (!g_str_has_prefix(path, "settings"))
    return NULL;

  const char *rest = path + strlen("settings");
  if (!*rest)
    return rest;

  return *rest == '/' ? rest + 1 : NULL;
}

static const char *settings_uri_pane(const char *uri, GUri **parsed)
{
  if (!uri)
    return NULL;

  *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  if (!*parsed || g_strcmp0(g_uri_get_scheme(*parsed), "wig") != 0)
    return NULL;

  return settings_path_pane(g_uri_get_path(*parsed));
}

gboolean uri_is_settings_page(const char *uri)
{
  g_autoptr(GUri) parsed = NULL;
  return settings_uri_pane(uri, &parsed) != NULL;
}

static char *wig_settings_page_pane_for_uri(const char *uri)
{
  g_autoptr(GUri) parsed = NULL;
  const char *pane = settings_uri_pane(uri, &parsed);

  return pane && *pane ? g_strdup(pane) : NULL;
}

/* Each pane is a page of its own in the switcher, so the group inside it carries
 * no title of its own until there is a second one to tell apart. A switcher
 * button falls back to the broken image icon, so every pane names one. */
static void wig_settings_page_new_pane(AdwViewStack *stack, const char *name, const char *title, const char *icon_name,
                                       AdwPreferencesGroup **group)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

  *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_page_add(page, *group);
  adw_view_stack_add_titled_with_icon(stack, GTK_WIDGET(page), name, title, icon_name);
}

static void wig_settings_page_add_browsing_pane(AdwViewStack *stack, GSettings *settings)
{
  static const char *tab_layout_nicks[] = { "horizontal", "vertical", NULL };
  static const char *const tab_layout_labels[] = { "Tab Bar", "Sidebar", NULL };

  AdwPreferencesGroup *group = NULL;
  wig_settings_page_new_pane(stack, "browser", "Browsing", "web-browser-symbolic", &group);

  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "restore-tabs", "Restore Tabs on Startup",
                                                        "Open the windows and tabs that were open when wig was "
                                                        "last closed."));
  adw_preferences_group_add(group,
                            wig_settings_combo_row_new(settings, "tab-layout", "Tab Layout",
                                                       "Show tabs in a bar above the page or in a sidebar beside it.",
                                                       tab_layout_nicks, tab_layout_labels));

#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  static const char *https_nicks[] = { "keep-as-requested", "https-first", "https-only", NULL };
  static const char *const https_labels[] = { "Off", "HTTPS-First", "HTTPS-Only", NULL };

  adw_preferences_group_add(group,
                            wig_settings_combo_row_new(settings, "https-navigation-policy", "HTTPS Navigation",
                                                       "Upgrade addresses typed as http to https. HTTPS-First "
                                                       "quietly falls back to http when the secure load fails; "
                                                       "HTTPS-Only shows an error instead.",
                                                       https_nicks, https_labels));
#endif

  wig_settings_search_engine_rows_add(group, settings);
}

static void wig_settings_page_add_content_pane(AdwViewStack *stack, GSettings *settings)
{
  AdwPreferencesGroup *group = NULL;
  wig_settings_page_new_pane(stack, "content", "Content", "text-x-generic-symbolic", &group);

  adw_preferences_group_add(
      group,
      wig_settings_switch_row_new(settings, "enable-javascript", "JavaScript", "Run the scripts a page carries."));
  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "auto-load-images", "Load Images",
                                                        "Fetch the images a page asks for. Turning this off saves "
                                                        "data and leaves the rest of the page as it is."));
  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "javascript-can-open-windows-automatically",
                                                        "Allow Pop-up Windows",
                                                        "Let a page open a window on its own, rather than only when "
                                                        "a click asks for one."));
  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "media-playback-requires-user-gesture",
                                                        "Require a Click Before Media Plays",
                                                        "Hold audio and video until they are started, instead of "
                                                        "letting a page play them on its own."));
  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "zoom-text-only", "Zoom Text Only",
                                                        "Resize only the text when zooming, leaving images and "
                                                        "layout at their own size."));
}

static char *wig_settings_page_first_pane(WigSettingsPage *self)
{
  g_autoptr(GtkSelectionModel) pages = adw_view_stack_get_pages(ADW_VIEW_STACK(self->stack));
  g_autoptr(AdwViewStackPage) first = g_list_model_get_item(G_LIST_MODEL(pages), 0);

  return first ? g_strdup(adw_view_stack_page_get_name(first)) : NULL;
}

/* The address the page is at decides the pane, so a bare wig:settings keeps the
 * address it was asked for and simply shows the first one. */
static void wig_settings_page_uri_changed(WigSettingsPage *self)
{
  g_autofree char *pane = wig_settings_page_pane_for_uri(wig_native_page_get_uri(WIG_NATIVE_PAGE(self)));
  g_autofree char *first = NULL;

  if (!pane) {
    first = wig_settings_page_first_pane(self);
    pane = g_steal_pointer(&first);
  }

  /* An address naming a pane that is not there still opens the settings. */
  if (!pane || !adw_view_stack_get_child_by_name(ADW_VIEW_STACK(self->stack), pane)) {
    g_debug("settings: no pane named '%s'", pane ? pane : "(null)");
    return;
  }

  self->applying_uri = TRUE;
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->stack), pane);
  self->applying_uri = FALSE;
}

static void wig_settings_page_visible_pane_changed(WigSettingsPage *self)
{
  if (self->applying_uri)
    return;

  const char *pane = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(self->stack));
  if (!pane)
    return;

  g_autofree char *uri = g_strconcat(WIG_SETTINGS_PAGE_URI "/", pane, NULL);
  wig_native_page_set_uri(WIG_NATIVE_PAGE(self), uri);
}

static void wig_settings_page_dispose(GObject *object)
{
  WigSettingsPage *self = WIG_SETTINGS_PAGE(object);

  g_clear_pointer(&self->navigation, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_settings_page_parent_class)->dispose(object);
}

static void wig_settings_page_class_init(WigSettingsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_page_dispose;

  gtk_widget_class_set_css_name(widget_class, "wig-settings-page");
}

static void wig_settings_page_init(WigSettingsPage *self)
{
  GSettings *settings = wig_application_get_settings(wig_application_get());

  GtkWidget *stack = adw_view_stack_new();
  self->stack = stack;
  wig_settings_page_add_browsing_pane(ADW_VIEW_STACK(stack), settings);
  wig_settings_page_add_content_pane(ADW_VIEW_STACK(stack), settings);
  g_signal_connect_object(stack, "notify::visible-child-name", G_CALLBACK(wig_settings_page_visible_pane_changed), self,
                          G_CONNECT_SWAPPED);

  /* The window this sits in already has a header bar, so the switcher stands on
   * its own above the panes rather than inside one of its own. */
  GtkWidget *switcher = adw_view_switcher_new();
  adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(stack));
  adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
  gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(switcher, 6);
  gtk_widget_set_margin_bottom(switcher, 6);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(toolbar), ADW_TOOLBAR_FLAT);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), switcher);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack);

  AdwNavigationPage *root = adw_navigation_page_new(toolbar, WIG_SETTINGS_PAGE_TITLE);

  self->navigation = adw_navigation_view_new();
  adw_navigation_view_add(ADW_NAVIGATION_VIEW(self->navigation), root);
  gtk_widget_set_parent(self->navigation, GTK_WIDGET(self));

  wig_native_page_set_title(WIG_NATIVE_PAGE(self), WIG_SETTINGS_PAGE_TITLE);
  g_signal_connect(self, "notify::uri", G_CALLBACK(wig_settings_page_uri_changed), NULL);
}

GtkWidget *wig_settings_page_new(const char *uri)
{
  return g_object_new(WIG_TYPE_SETTINGS_PAGE, "uri", uri, NULL);
}
