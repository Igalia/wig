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
#include "wig-settings-features.h"
#include "wig-settings-rows.h"
#include "wig-settings-search.h"

#include <adwaita.h>

struct _WigSettingsPage {
  WigNativePage parent;

  /* Holds the one page for now, and is what a settings row drills into. */
  GtkWidget *navigation;
  GtkWidget *stack;
  GtkWidget *content;
  GtkWidget *search;
  GtkWidget *search_bar;
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

/* The feature lists used to be pages of their own, and the addresses they were
 * reached by are kept working. */
char *wig_settings_page_moved_uri(const char *uri)
{
  static const struct {
    const char *from;
    const char *to;
  } moved[] = {
    { "wig:features", WIG_SETTINGS_PAGE_URI "/features" },
    { "wig:developer-features", WIG_SETTINGS_PAGE_URI "/developer-features" },
  };

  if (!uri)
    return NULL;

  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  if (!parsed || g_strcmp0(g_uri_get_scheme(parsed), "wig") != 0)
    return NULL;

  for (guint i = 0; i < G_N_ELEMENTS(moved); i++) {
    /* The path alone, so a query left over from the old page is dropped. */
    if (g_str_equal(g_uri_get_path(parsed), moved[i].from + strlen("wig:")))
      return g_strdup(moved[i].to);
  }

  return NULL;
}

static char *wig_settings_page_pane_for_uri(const char *uri)
{
  g_autoptr(GUri) parsed = NULL;
  const char *pane = settings_uri_pane(uri, &parsed);

  return pane && *pane ? g_strdup(pane) : NULL;
}

/* What a row needs to be put somewhere and found again. */
typedef struct {
  WigSettingsPage *page;
  AdwPreferencesGroup *group;
  const char *name;
  const char *title;
} Pane;

/* Each pane is a page of its own in the switcher, so the group inside it carries
 * no title of its own until there is a second one to tell apart. A switcher
 * button falls back to the broken image icon, so every pane names one. */
static void wig_settings_page_new_pane(WigSettingsPage *self, Pane *pane, const char *name, const char *title,
                                       const char *icon_name)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

  pane->page = self;
  pane->group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  pane->name = name;
  pane->title = title;

  adw_preferences_page_add(page, pane->group);
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), GTK_WIDGET(page), name, title, icon_name);
}

/* Searching looks through the rows wherever they are, so every one of them is
 * offered along with the pane it would be found in. */
static void pane_add(Pane *pane, GtkWidget *row)
{
  adw_preferences_group_add(pane->group, row);
  wig_settings_search_add_row(WIG_SETTINGS_SEARCH(pane->page->search), row, pane->name, pane->title);
}

static void wig_settings_page_add_browsing_pane(WigSettingsPage *self, GSettings *settings)
{
  static const char *tab_layout_nicks[] = { "horizontal", "vertical", NULL };
  static const char *const tab_layout_labels[] = { "Tab Bar", "Sidebar", NULL };

  Pane pane;
  wig_settings_page_new_pane(self, &pane, "browser", "Browsing", "web-browser-symbolic");

  pane_add(&pane,
           wig_settings_switch_row_new(settings, "restore-tabs", "Restore Tabs on Startup",
                                       "Open the windows and tabs that were open when wig was last closed."));
  pane_add(&pane,
           wig_settings_combo_row_new(settings, "tab-layout", "Tab Layout",
                                      "Show tabs in a bar above the page or in a sidebar beside it.", tab_layout_nicks,
                                      tab_layout_labels));

#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  static const char *https_nicks[] = { "keep-as-requested", "https-first", "https-only", NULL };
  static const char *const https_labels[] = { "Off", "HTTPS-First", "HTTPS-Only", NULL };

  pane_add(&pane,
           wig_settings_combo_row_new(settings, "https-navigation-policy", "HTTPS Navigation",
                                      "Upgrade addresses typed as http to https. HTTPS-First quietly falls "
                                      "back to http when the secure load fails; HTTPS-Only shows an error "
                                      "instead.",
                                      https_nicks, https_labels));
#endif

  /* The template it reveals is part of the same setting, so only the row naming
   * the engine is worth finding. */
  wig_settings_search_add_row(WIG_SETTINGS_SEARCH(self->search),
                              wig_settings_search_engine_rows_add(pane.group, settings), pane.name, pane.title);
}

static void wig_settings_page_add_content_pane(WigSettingsPage *self, GSettings *settings)
{
  Pane pane;
  wig_settings_page_new_pane(self, &pane, "content", "Content", "text-x-generic-symbolic");

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
}

/* The feature lists are pages of their own rather than rows in a group, so they
 * go in beside the panes rather than through one. */
static void wig_settings_page_add_features_panes(WigSettingsPage *self)
{
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_features_new(WIG_FEATURES_EXPERIMENTAL),
                                      "features", "Features", "applications-science-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), wig_settings_features_new(WIG_FEATURES_DEVELOPMENT),
                                      "developer-features", "Developer", "applications-engineering-symbolic");
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

static void wig_settings_page_set_searching(WigSettingsPage *self, gboolean searching)
{
  gtk_stack_set_visible_child_name(GTK_STACK(self->content), searching ? "search" : "panes");
}

static void wig_settings_page_search_changed(WigSettingsPage *self, GtkSearchEntry *entry)
{
  const char *terms = gtk_editable_get_text(GTK_EDITABLE(entry));

  /* An empty search is the settings as they were, not a search with everything
   * in it. */
  wig_settings_page_set_searching(self, *terms != '\0');
  wig_settings_search_set_terms(WIG_SETTINGS_SEARCH(self->search), terms);
}

static void wig_settings_page_search_mode_changed(WigSettingsPage *self)
{
  if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(self->search_bar)))
    wig_settings_page_set_searching(self, FALSE);
}

/* A result stands for a row in a pane, so following one goes to the pane and
 * leaves the search behind. */
static void wig_settings_page_search_activated(WigSettingsPage *self, const char *pane)
{
  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar), FALSE);
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->stack), pane);
}

static gboolean wig_settings_page_start_search(WigNativePage *page)
{
  WigSettingsPage *self = WIG_SETTINGS_PAGE(page);

  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar), TRUE);
  return TRUE;
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
  WIG_NATIVE_PAGE_CLASS(klass)->start_search = wig_settings_page_start_search;

  gtk_widget_class_set_css_name(widget_class, "wig-settings-page");
}

static void wig_settings_page_init(WigSettingsPage *self)
{
  GSettings *settings = wig_application_get_settings(wig_application_get());

  self->search = wig_settings_search_new();
  g_signal_connect_object(self->search, "activated", G_CALLBACK(wig_settings_page_search_activated), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *stack = adw_view_stack_new();
  self->stack = stack;
  wig_settings_page_add_browsing_pane(self, settings);
  wig_settings_page_add_content_pane(self, settings);
  wig_settings_page_add_features_panes(self);
  g_signal_connect_object(stack, "notify::visible-child-name", G_CALLBACK(wig_settings_page_visible_pane_changed), self,
                          G_CONNECT_SWAPPED);

  /* Searching takes the place of the panes rather than sitting beside them, so
   * what is on screen is either everything or what was asked for. */
  self->content = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->content), stack, "panes");
  gtk_stack_add_named(GTK_STACK(self->content), self->search, "search");

  /* The window this sits in already has a header bar, so the switcher stands on
   * its own above the panes rather than inside one of its own. */
  GtkWidget *switcher = adw_view_switcher_new();
  adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(stack));
  adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
  gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(switcher, 6);
  gtk_widget_set_margin_bottom(switcher, 6);

  GtkWidget *entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(entry, TRUE);
  g_signal_connect_object(entry, "search-changed", G_CALLBACK(wig_settings_page_search_changed), self,
                          G_CONNECT_SWAPPED);

  /* The rows are held to a readable width by a clamp of their own, and the
   * entry searching them lines up with them rather than with the window. */
  GtkWidget *entry_clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(entry_clamp), entry);

  self->search_bar = gtk_search_bar_new();
  gtk_search_bar_set_child(GTK_SEARCH_BAR(self->search_bar), entry_clamp);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(self->search_bar), GTK_EDITABLE(entry));
  /* Typing anywhere in the page starts a search, the way the preferences dialog
   * wig cannot use does. */
  gtk_search_bar_set_key_capture_widget(GTK_SEARCH_BAR(self->search_bar), GTK_WIDGET(self));
  g_signal_connect_object(self->search_bar, "notify::search-mode-enabled",
                          G_CALLBACK(wig_settings_page_search_mode_changed), self, G_CONNECT_SWAPPED);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(toolbar), ADW_TOOLBAR_FLAT);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), switcher);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), self->search_bar);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->content);

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
