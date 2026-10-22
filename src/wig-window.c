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

#include "wig-window.h"

#include "mcp/wig-mcp-server.h"
#include "wig-application.h"
#include "wig-bookmark-popover.h"
#include "wig-downloads-button.h"
#include "wig-entry-completion-popover.h"
#include "wig-new-tab-page.h"
#include "wig-pwa-icon.h"
#include "wig-pwa-synthetic-manifest.h"
#include "wig-pwa-util.h"
#include "wig-settings-page.h"
#include "wig-tab-bar.h"
#include "wig-tab-list.h"
#include "wig-tab-sidebar.h"
#include "wig-tab-switcher.h"
#include "wig-tab.h"
#include "wig-utils.h"
#include "wpe-toplevel-gtk.h"
#include "wpe-view-gtk.h"

struct _WigWindow {
  WigWindowBase parent;

  WigTabLayout tab_layout;
  gboolean popup;
  GtkWidget *toolbar_view;
  GtkWidget *header_bar;
  GtkWidget *toolbar_box;
  GtkWidget *back_button;
  GtkWidget *forward_button;
  GtkWidget *stop_reload_button;
  GtkWidget *update_button;
  GtkWidget *new_tab_button;
  GtkWidget *url_entry;
  GtkWidget *bookmark_button;
  GtkWidget *bookmark_popover;
  GtkWidget *link_bookmark_popover;
  GdkRectangle context_menu_target;
  GtkWidget *entry_completion_popover;
  GtkWidget *pwa_button;
  GtkWidget *install_popover;
  WigTabList *tab_list;
  WigTabSwitcher *tab_switcher;
  GtkWidget *content_box;
  GtkWidget *paned;
  GtkWidget *tab_bar;
  GtkWidget *tab_separator;
  GtkWidget *tab_sidebar;
  GtkWidget *tab_stack;
  GtkWidget *overview_button;
  guint progress_timeout_id;
  gboolean suppress_entry_completion;
  gboolean url_entry_focused;
  gboolean url_entry_edited;
  GActionGroup *context_menu_action_group;
  GtkWidget *tab_view_context_menu;
  GPtrArray *closing_tabs;
  GSignalGroup *active_web_view_signals;
  GSignalGroup *active_tab_signals;
  GBinding *tab_loading_binding;
  GHashTable *web_view_signal_groups;
  GPtrArray *unshown_web_views;
};

G_DEFINE_FINAL_TYPE(WigWindow, wig_window, WIG_TYPE_WINDOW_BASE)
G_DEFINE_ENUM_TYPE(WigTabLayout, wig_tab_layout, G_DEFINE_ENUM_VALUE(WIG_TAB_LAYOUT_HORIZONTAL, "horizontal"),
                   G_DEFINE_ENUM_VALUE(WIG_TAB_LAYOUT_VERTICAL, "vertical"))

typedef enum {
  PROP_TAB_LAYOUT = 1,
} WigWindowProps;

static GParamSpec *props[PROP_TAB_LAYOUT + 1];

static void wig_window_match_window(WigWindow *win, WigWindow *other);
static void wig_window_set_tab_layout(WigWindow *win, WigTabLayout layout);
static void wig_window_update_load_progress(WigWindow *win);

static void wig_window_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigWindow *win = WIG_WINDOW(object);
  switch ((WigWindowProps)prop_id) {
  case PROP_TAB_LAYOUT:
    g_value_set_enum(value, (gint)win->tab_layout);
    break;
  }
}

static void wig_window_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigWindow *win = WIG_WINDOW(object);
  switch ((WigWindowProps)prop_id) {
  case PROP_TAB_LAYOUT:
    wig_window_set_tab_layout(win, (WigTabLayout)g_value_get_enum(value));
    break;
  }
}

static void wig_window_web_view_ready_to_show(WigWindow *win, WebKitWebView *web_view);

static WebKitWebView *wig_window_active_web_view(WigWindow *win)
{
  return wig_window_base_get_active_web_view(WIG_WINDOW_BASE(win));
}

/* The window shows whether the tab being looked at is loading, which is not the
 * same as its view's "is-loading": a view that crashed or gave up on a load
 * keeps claiming it loads, and only the tab knows an error page replaced it. */
static void wig_window_clear_tab_loading_binding(WigWindow *win)
{
  if (!win->tab_loading_binding)
    return;

  g_autoptr(GObject) binding = g_object_ref(G_OBJECT(win->tab_loading_binding));
  g_clear_weak_pointer(&win->tab_loading_binding);
  g_binding_unbind(G_BINDING(binding));
}

static void wig_window_bind_tab_loading(WigWindow *win, WigTab *tab)
{
  wig_window_clear_tab_loading_binding(win);

  if (!tab) {
    wig_window_base_set_loading(WIG_WINDOW_BASE(win), FALSE);
    return;
  }

  /* Bidirectional so that settling the window's state, which is what stopping
   * does, reaches the tab and takes the spinner in the strip down with it. */
  g_set_weak_pointer(
      &win->tab_loading_binding,
      g_object_bind_property(tab, "loading", win, "loading", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
}

static void wig_window_loading_changed(WigWindow *win)
{
  gboolean is_loading = wig_window_base_get_loading(WIG_WINDOW_BASE(win));

  if (win->stop_reload_button)
    gtk_button_set_icon_name(GTK_BUTTON(win->stop_reload_button),
                             is_loading ? "process-stop-symbolic" : "view-refresh-symbolic");

  wig_window_update_load_progress(win);
}

static WigTab *wig_window_get_tab_for_web_view(WigWindow *win, WebKitWebView *web_view)
{
  /* A web view can emit "close" while the window is tearing down, after the tab
   * list has already been cleared. */
  if (!win->tab_list)
    return NULL;

  guint n = wig_tab_list_get_n_tabs(win->tab_list);
  for (guint i = 0; i < n; i++) {
    WigTab *tab = wig_tab_list_get_nth(win->tab_list, i);
    if (wig_tab_get_web_view(tab) == web_view)
      return g_object_ref(tab);
  }
  return NULL;
}

static void wig_window_save_tab_to_history(WigWindow *win, WigTab *tab)
{
  WigSessionWindow *closed = wig_session_window_new(wig_window_base_get_id(WIG_WINDOW_BASE(win)));
  wig_session_window_add_tab(closed, wig_tab_get_session_state(tab), wig_tab_get_title(tab), wig_tab_get_uri(tab),
                             wig_window_active_web_view(win) == wig_tab_get_web_view(tab), wig_tab_get_pinned(tab));
  wig_session_push_closed_window(wig_application_get_session(wig_application_get()), closed);
}

/* A close the window never completed leaves the tabs that did agree sitting in
 * the list, so the pages are asked again the next time one of them is closed. */
static void wig_window_abandon_close(WigWindow *win)
{
  g_debug("window %u: the close did not go through, its tabs stay", wig_window_base_get_id(WIG_WINDOW_BASE(win)));
  g_clear_pointer(&win->closing_tabs, g_ptr_array_unref);

  guint n_tabs = wig_tab_list_get_n_tabs(win->tab_list);
  for (guint i = 0; i < n_tabs; i++)
    wig_tab_set_closing(wig_tab_list_get_nth(win->tab_list, i), FALSE);
}

/* The window is closed as a whole: a tab that agrees stays in the list until
 * every other one has agreed too, so the window goes away holding all of them
 * and is recorded as one session entry rather than a trail of single-tab ones. */
static gboolean wig_window_tab_agreed_to_close(WigWindow *win, WigTab *tab)
{
  if (!win->closing_tabs)
    return FALSE;

  if (!g_ptr_array_remove_fast(win->closing_tabs, tab)) {
    wig_window_abandon_close(win);
    return FALSE;
  }

  if (!win->closing_tabs->len)
    gtk_window_destroy(GTK_WINDOW(win));

  return TRUE;
}

static gboolean wig_window_tab_close(WigTabList *list, WigTab *tab, WigWindow *win)
{
  /* Nothing is torn down until the page has run its beforeunload handler. WebKit
   * answers by emitting close on the view, which comes back here with the tab
   * marked; a page that is refused keeps its tab. */
  if (!wig_tab_get_closing(tab)) {
    /* A discarded tab has no page to ask, so it goes without argument. */
    if (wig_tab_get_web_view(tab)) {
      if (!wig_tab_get_close_pending(tab)) {
        g_debug("tab %u: asking the page to close", wig_tab_get_id(tab));
        wig_tab_set_close_pending(tab, TRUE);
        webkit_web_view_try_close(wig_tab_get_web_view(tab));
      }
      return TRUE;
    }

    wig_tab_set_closing(tab, TRUE);
  }

  if (wig_window_tab_agreed_to_close(win, tab))
    return TRUE;

  /* The window going away records its remaining tabs itself, so recording the
   * last tab here as well would push it twice. */
  if (wig_tab_list_get_n_tabs(list) == 1) {
    gtk_window_destroy(GTK_WINDOW(win));
    return TRUE;
  }

  wig_window_save_tab_to_history(win, tab);
  return FALSE;
}

static void wig_window_web_view_closed(WigWindow *win, WebKitWebView *web_view)
{
  g_autoptr(WigTab) tab = wig_window_get_tab_for_web_view(win, web_view);
  if (!tab)
    return;

  g_debug("tab %u: the page is done with, closing it", wig_tab_get_id(tab));
  wig_tab_set_closing(tab, TRUE);
  wig_tab_list_close(win->tab_list, tab);
}

WebKitWebView *wig_window_focus_tab_by_site(WigWindow *win, const char *uri, WebKitWebView *ignore, gboolean reload)
{
  g_autoptr(GUri) lookup = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  if (!lookup)
    return NULL;

  const char *lookup_scheme = g_uri_get_scheme(lookup);
  const char *lookup_host = g_uri_get_host(lookup);
  const char *lookup_path = g_uri_get_path(lookup);

  guint n = wig_tab_list_get_n_tabs(win->tab_list);
  for (guint i = 0; i < n; i++) {
    WigTab *tab = wig_tab_list_get_nth(win->tab_list, i);
    WebKitWebView *web_view = wig_tab_get_web_view(tab);
    if (web_view == ignore)
      continue;

    const char *tab_uri = wig_tab_get_uri(tab);
    if (!tab_uri)
      continue;

    g_autoptr(GUri) parsed = g_uri_parse(tab_uri, G_URI_FLAGS_NONE, NULL);
    if (!parsed)
      continue;

    const char *tab_scheme = g_uri_get_scheme(parsed);
    const char *tab_host = g_uri_get_host(parsed);

    gboolean match;
    if (lookup_host && *lookup_host)
      match = g_str_equal(lookup_scheme, tab_scheme) && tab_host && g_ascii_strcasecmp(lookup_host, tab_host) == 0;
    else
      match = g_str_equal(lookup_scheme, tab_scheme)
          && wig_util_paths_are_same_page(lookup_path, g_uri_get_path(parsed));

    if (match) {
      gboolean discarded = wig_tab_get_discarded(tab);
      wig_tab_list_set_active(win->tab_list, tab);
      wig_tab_load_discarded(tab);
      if (reload && !discarded)
        webkit_web_view_reload(web_view);
      return web_view;
    }
  }
  return NULL;
}

static void wig_window_close_install_web_app_popover(WigWindow *win)
{
  if (win->install_popover)
    gtk_popover_popdown(GTK_POPOVER(win->install_popover));
}

typedef struct {
  WigWindow *window;
  WebKitWebView *web_view;
  char *id;
} WigWindowInstallContext;

static void wig_window_install_context_free(WigWindowInstallContext *context)
{
  g_clear_object(&context->window);
  g_clear_object(&context->web_view);
  g_clear_pointer(&context->id, g_free);
  g_free(context);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WigWindowInstallContext, wig_window_install_context_free)

static void wig_window_install_web_app_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigWindowInstallContext) context = user_data;
  g_autoptr(GError) error = NULL;
  if (!wig_pwa_manager_install_finish(WIG_PWA_MANAGER(source), result, &error)) {
    g_warning("Failed to install web app: %s", error->message);
    if (context->window->install_popover)
      gtk_widget_set_sensitive(context->window->install_popover, TRUE);
    return;
  }

  if (!wig_application_adopt_web_app(wig_application_get(), context->id, context->web_view, &error)) {
    g_warning("Failed to open installed web app: %s", error->message);
    if (context->window->install_popover)
      gtk_widget_set_sensitive(context->window->install_popover, TRUE);
    return;
  }

  wig_window_close_install_web_app_popover(context->window);
}

/* A page that offers no manifest of its own is installed from one made up out of
 * what the browser already knows: where the page is, what it calls itself, and
 * the icon the tab is showing for it. */
static void wig_window_install_as_web_app(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WebKitWebView *web_view = wig_window_active_web_view(win);
  if (!web_view)
    return;

  const char *uri = webkit_web_view_get_uri(web_view);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) manifest = wig_pwa_synthetic_manifest_new(uri, webkit_web_view_get_title(web_view), &error);
  if (!manifest) {
    g_warning("Cannot install this page as a web app: %s", error->message);
    return;
  }

  g_autoptr(WigTab) tab = wig_window_get_tab_for_web_view(win, web_view);
  GIcon *icon = tab ? wig_tab_get_icon(tab) : NULL;

  WigWindowInstallContext *context = g_new0(WigWindowInstallContext, 1);
  context->window = g_object_ref(win);
  context->web_view = g_object_ref(web_view);
  context->id = g_strdup(wig_pwa_variant_lookup_string(manifest, "id"));
  g_debug("pwa: installing '%s' from a made-up manifest", context->id);
  wig_pwa_manager_install_manifest_async(wig_application_get_pwa_manager(wig_application_get()), uri, manifest,
                                         G_IS_LOADABLE_ICON(icon) ? G_LOADABLE_ICON(icon) : NULL, NULL,
                                         wig_window_install_web_app_finished, context);
}

/* Only a page from the web can become an app; the browser's own pages cannot. */
static void wig_window_update_install_action(WigWindow *win)
{
  WebKitWebView *web_view = wig_window_active_web_view(win);
  const char *uri = web_view ? webkit_web_view_get_uri(web_view) : NULL;
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(win), "install-as-web-app");

  if (action)
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), wig_pwa_synthetic_manifest_is_possible(uri));
}

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static void wig_window_install_icon_loaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkImage *image = GTK_IMAGE(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GInputStream) stream = g_loadable_icon_load_finish(G_LOADABLE_ICON(source), result, NULL, &error);
  if (!stream) {
    g_object_unref(image);
    return;
  }

  g_autoptr(GBytes) bytes = g_input_stream_read_bytes(stream, 10 * 1024 * 1024, NULL, &error);
  if (!bytes) {
    g_object_unref(image);
    return;
  }

  g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes(bytes, &error);
  if (texture)
    gtk_image_set_from_paintable(image, GDK_PAINTABLE(texture));

  g_object_unref(image);
}

static void wig_window_install_web_app(WigWindow *win)
{
  WebKitApplicationManifest *manifest = wig_window_active_web_view(win)
      ? wig_window_base_get_application_manifest(WIG_WINDOW_BASE(win), wig_window_active_web_view(win))
      : NULL;
  if (!manifest)
    return;

  const char *id = webkit_application_manifest_get_id(manifest);
  if (!id)
    return;

  gtk_widget_set_sensitive(win->install_popover, FALSE);
  WigWindowInstallContext *context = g_new0(WigWindowInstallContext, 1);
  context->window = g_object_ref(win);
  context->web_view = g_object_ref(wig_window_active_web_view(win));
  context->id = g_strdup(id);
  wig_pwa_manager_install_async(wig_application_get_pwa_manager(wig_application_get()), manifest, NULL,
                                wig_window_install_web_app_finished, context);
}

static void wig_window_show_install_web_app_popover(WigWindow *win)
{
  WebKitApplicationManifest *manifest = wig_window_active_web_view(win)
      ? wig_window_base_get_application_manifest(WIG_WINDOW_BASE(win), wig_window_active_web_view(win))
      : NULL;
  if (!manifest)
    return;

  g_clear_pointer(&win->install_popover, gtk_widget_unparent);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_size_request(box, 280, -1);

  GtkWidget *details = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *image = gtk_image_new_from_icon_name("web-browser-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(image), 64);
  gtk_box_append(GTK_BOX(details), image);

  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(labels, TRUE);
  const char *title = webkit_application_manifest_get_short_name(manifest);
  if (!title || !*title)
    title = webkit_application_manifest_get_name(manifest);
  if (!title || !*title)
    title = webkit_web_view_get_title(wig_window_active_web_view(win));
  if (!title || !*title)
    title = "Web App";
  GtkWidget *title_label = gtk_label_new(title);
  gtk_widget_add_css_class(title_label, "title-3");
  gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(labels), title_label);

  const char *uri = webkit_application_manifest_get_start_uri(manifest);
  if (!uri || !*uri)
    uri = webkit_web_view_get_uri(wig_window_active_web_view(win));
  GtkWidget *uri_label = gtk_label_new(uri);
  gtk_widget_add_css_class(uri_label, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(uri_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(uri_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(uri_label), 28);
  gtk_box_append(GTK_BOX(labels), uri_label);
  gtk_box_append(GTK_BOX(details), labels);
  gtk_box_append(GTK_BOX(box), details);

  GtkWidget *question = gtk_label_new("Install this web app?");
  gtk_label_set_xalign(GTK_LABEL(question), 0.0f);
  gtk_box_append(GTK_BOX(box), question);

  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(actions, GTK_ALIGN_END);
  GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
  g_signal_connect_object(cancel_button, "clicked", G_CALLBACK(wig_window_close_install_web_app_popover), win,
                          G_CONNECT_SWAPPED);
  gtk_box_append(GTK_BOX(actions), cancel_button);
  GtkWidget *install_button = gtk_button_new_with_label("Install");
  gtk_widget_add_css_class(install_button, "suggested-action");
  g_signal_connect_object(install_button, "clicked", G_CALLBACK(wig_window_install_web_app), win, G_CONNECT_SWAPPED);
  gtk_box_append(GTK_BOX(actions), install_button);
  gtk_box_append(GTK_BOX(box), actions);

  WebKitApplicationManifestIcon *const *icons = webkit_application_manifest_get_icons(manifest);
  for (guint i = 0; icons && icons[i]; i++) {
    const char *const *sizes = webkit_application_manifest_icon_get_sizes(icons[i]);
    g_autoptr(GString) sizes_description = g_string_new(NULL);
    for (guint j = 0; sizes && sizes[j]; j++) {
      if (j)
        g_string_append_c(sizes_description, ' ');
      g_string_append(sizes_description, sizes[j]);
    }
    g_debug("web app icon %u: uri=%s, sizes=%s, mime-type=%s, maskable=%d, monochrome=%d", i,
            webkit_application_manifest_icon_get_uri(icons[i]), sizes_description->str,
            webkit_application_manifest_icon_get_mime_type(icons[i]),
            webkit_application_manifest_icon_get_is_maskable(icons[i]),
            webkit_application_manifest_icon_get_is_monochrome(icons[i]));
  }
  int scale_factor = gtk_widget_get_scale_factor(win->pwa_button);
  int requested_size = 64 * scale_factor;
  WebKitApplicationManifestIcon *icon = wig_pwa_icon_select(icons, requested_size);
  if (icon) {
    g_debug("using web app icon %s at %d physical pixels", webkit_application_manifest_icon_get_uri(icon),
            requested_size);
    g_loadable_icon_load_async(G_LOADABLE_ICON(icon), requested_size, NULL, wig_window_install_icon_loaded,
                               g_object_ref(image));
  }

  win->install_popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(win->install_popover), box);
  gtk_widget_set_parent(win->install_popover, win->pwa_button);
  gtk_popover_popup(GTK_POPOVER(win->install_popover));
}

static void wig_window_pwa_button_clicked(WigWindow *win)
{
  WebKitApplicationManifest *manifest = wig_window_active_web_view(win)
      ? wig_window_base_get_application_manifest(WIG_WINDOW_BASE(win), wig_window_active_web_view(win))
      : NULL;
  if (!manifest)
    return;

  const char *id = webkit_application_manifest_get_id(manifest);
  if (!id || !*id)
    return;

  WigPwaManager *manager = wig_application_get_pwa_manager(wig_application_get());
  g_autoptr(GError) error = NULL;
  if (!wig_pwa_manager_is_installed(manager, id, &error)) {
    if (error) {
      g_warning("Failed to check whether web app is installed: %s", error->message);
      return;
    }
    wig_window_show_install_web_app_popover(win);
    return;
  }

  if (!wig_application_adopt_web_app(wig_application_get(), id, wig_window_active_web_view(win), &error))
    g_warning("Failed to open installed web app: %s", error->message);
}

static void wig_window_update_pwa_button(WigWindow *win)
{
  WebKitApplicationManifest *manifest = wig_window_active_web_view(win)
      ? wig_window_base_get_application_manifest(WIG_WINDOW_BASE(win), wig_window_active_web_view(win))
      : NULL;
  const char *id = manifest ? webkit_application_manifest_get_id(manifest) : NULL;
  gboolean visible = id && *id;
  gboolean installed = FALSE;
  g_autoptr(GError) error = NULL;
  if (visible)
    installed = wig_pwa_manager_is_installed(wig_application_get_pwa_manager(wig_application_get()), id, &error);

  if ((!visible || installed) && win->install_popover)
    gtk_popover_popdown(GTK_POPOVER(win->install_popover));
  gtk_button_set_icon_name(GTK_BUTTON(win->pwa_button),
                           installed ? "window-new-symbolic" : "system-software-install-symbolic");
  gtk_widget_set_tooltip_text(win->pwa_button,
                              error           ? "Web app state unavailable"
                                  : installed ? "Open in web app window"
                                              : "Install web app");
  gtk_widget_set_sensitive(win->pwa_button, error == NULL);
  gtk_widget_set_visible(win->pwa_button, visible);
}

static void wig_window_pwa_manager_changed(WigWindow *win, const char *id, WigPwaManager *manager)
{
  wig_window_update_pwa_button(win);
}
#endif

static gboolean wig_window_decide_policy(WigWindow *win, WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType decision_type, WebKitWebView *web_view);
static WebKitWebView *wig_window_web_view_create(WigWindow *win, WebKitNavigationAction *navigation,
                                                 WebKitWebView *opener);
static void wig_window_web_process_terminated(WigWindow *win, WebKitWebProcessTerminationReason reason,
                                              WebKitWebView *web_view);

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static void wig_window_application_manifest_changed(WigWindowBase *base, WebKitWebView *web_view)
{
  WigWindow *win = WIG_WINDOW(base);
  if (wig_window_active_web_view(win) == web_view)
    wig_window_update_pwa_button(win);
}
#endif

static void wig_window_attach_web_view(WigWindow *win, WebKitWebView *web_view)
{
  wig_window_base_attach_web_view(WIG_WINDOW_BASE(win), web_view);

  if (g_hash_table_contains(win->web_view_signal_groups, web_view))
    return;

  WigMcpServer *mcp_server = wig_application_get_mcp_server(wig_application_get());
  if (mcp_server)
    wig_mcp_server_watch_view(mcp_server, web_view);

  g_autoptr(GObject) signals_object = G_OBJECT(g_signal_group_new(WEBKIT_TYPE_WEB_VIEW));
  GSignalGroup *signals = G_SIGNAL_GROUP(signals_object);
  g_signal_group_connect_swapped(signals, "close", G_CALLBACK(wig_window_web_view_closed), win);
  g_signal_group_connect_swapped(signals, "decide-policy", G_CALLBACK(wig_window_decide_policy), win);
  g_signal_group_connect_swapped(signals, "create", G_CALLBACK(wig_window_web_view_create), win);
  g_signal_group_connect_swapped(signals, "web-process-terminated", G_CALLBACK(wig_window_web_process_terminated), win);
  g_signal_group_set_target(signals, web_view);
  g_hash_table_insert(win->web_view_signal_groups, web_view, g_steal_pointer(&signals_object));
}

static void wig_window_detach_web_view(WigWindow *win, WebKitWebView *web_view)
{
  GSignalGroup *signals = g_hash_table_lookup(win->web_view_signal_groups, web_view);
  if (signals)
    g_signal_group_set_target(signals, NULL);
  g_hash_table_remove(win->web_view_signal_groups, web_view);
  g_signal_handlers_disconnect_by_func(web_view, wig_window_web_view_ready_to_show, win);
  wig_window_base_detach_web_view(WIG_WINDOW_BASE(win), web_view);
}

static WigTab *wig_window_add_tab_for_view(WigWindow *win, WebKitWebView *web_view)
{
  return wig_tab_list_append(win->tab_list, web_view);
}

static WigTab *wig_window_add_tab_after_active(WigWindow *win, WebKitWebView *web_view)
{
  WigTab *active = wig_tab_list_get_active(win->tab_list);
  guint index = active ? wig_tab_list_index_of(win->tab_list, active) + 1 : wig_tab_list_get_n_tabs(win->tab_list);

  return wig_tab_list_insert(win->tab_list, web_view, index);
}

static WebKitWebView *wig_window_create_web_view_for_new_tab(WigWindow *win)
{
  WigApplication *app = wig_application_get();
  WebKitWebView *view = wig_application_create_web_view(app);

  wig_application_mark_internal_navigation(app, view, WIG_NEW_TAB_PAGE_URI);
  webkit_web_view_load_uri(view, WIG_NEW_TAB_PAGE_URI);
  return view;
}

static void wig_window_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  g_autoptr(WebKitWebView) web_view = wig_window_create_web_view_for_new_tab(win);
  WigTab *tab = wig_window_add_tab_for_view(win, web_view);
  wig_tab_list_set_active(win->tab_list, tab);
  gtk_widget_grab_focus(win->url_entry);
}

static void wig_window_next_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  wig_tab_switch_active(win->tab_list, 1);
}

static void wig_window_prev_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  wig_tab_switch_active(win->tab_list, -1);
}

static void wig_window_focus_entry(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  gtk_widget_grab_focus(win->url_entry);
  gtk_editable_select_region(GTK_EDITABLE(win->url_entry), 0, -1);
}

static void wig_window_show_downloads(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_open_internal_page(wig_application_get(), GTK_WINDOW(user_data), "wig:downloads");
}

static void wig_window_show_history(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_open_internal_page(wig_application_get(), GTK_WINDOW(user_data), "wig:history");
}

static void wig_window_bookmark_page(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);

  if (!gtk_widget_get_sensitive(win->bookmark_button))
    return;

  gtk_menu_button_popup(GTK_MENU_BUTTON(win->bookmark_button));
}

static void wig_window_show_bookmarks(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_open_internal_page(wig_application_get(), GTK_WINDOW(user_data), "wig:bookmarks");
}

static void wig_window_show_settings(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_open_internal_page(wig_application_get(), GTK_WINDOW(user_data), "wig:settings");
}

static void wig_window_toggle_inspector(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);

  WebKitWebView *web_view = wig_window_active_web_view(win);
  if (web_view)
    webkit_web_view_toggle_inspector(web_view);
}

static void wig_window_close_tab_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WebKitWebView *web_view = wig_window_active_web_view(win);
  if (!web_view)
    return;

  g_autoptr(WigTab) tab = wig_window_get_tab_for_web_view(win, web_view);
  if (tab)
    wig_tab_list_close(win->tab_list, tab);
}

static WigWindow *get_window_by_id(WigApplication *app, guint id)
{
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
  for (GList *l = windows; l; l = g_list_next(l)) {
    if (!WIG_IS_WINDOW(l->data))
      continue;

    WigWindow *window = WIG_WINDOW(l->data);
    if (wig_window_base_get_id(WIG_WINDOW_BASE(window)) == id)
      return window;
  }

  return NULL;
}

/* Nothing on GtkWindow reports this, and on Wayland the compositor may never
 * tell us either, in which case the toplevel simply never carries the state. */
static gboolean wig_window_is_minimized(WigWindow *win)
{
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win));
  if (!GDK_IS_TOPLEVEL(surface))
    return FALSE;

  return (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
}

/* Connectors are how a monitor is recognised across restarts; it is the closest
 * thing to a stable name, and NULL when the backend does not name its outputs. */
static const char *wig_window_monitor_connector(WigWindow *win)
{
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win));
  if (!surface)
    return NULL;

  GdkMonitor *monitor = gdk_display_get_monitor_at_surface(gtk_widget_get_display(GTK_WIDGET(win)), surface);
  return monitor ? gdk_monitor_get_connector(monitor) : NULL;
}

static GdkMonitor *wig_window_find_monitor(GdkDisplay *display, const char *connector)
{
  if (!connector || !*connector)
    return NULL;

  GListModel *monitors = gdk_display_get_monitors(display);
  guint n = g_list_model_get_n_items(monitors);

  for (guint i = 0; i < n; i++) {
    g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, i);
    if (g_strcmp0(gdk_monitor_get_connector(monitor), connector) == 0)
      return g_steal_pointer(&monitor);
  }

  g_debug("session: monitor '%s' is not connected", connector);
  return NULL;
}

static void wig_window_sidebar_position_changed(GtkPaned *paned, GParamSpec *pspec, WigWindow *win)
{
  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  wig_session_queue_save(wig_application_get_session(app));
}

WigSessionWindow *wig_window_capture_session(WigWindow *win)
{
  WigSessionWindow *captured = wig_session_window_new(wig_window_base_get_id(WIG_WINDOW_BASE(win)));
  captured->focused = gtk_window_is_active(GTK_WINDOW(win));
  captured->maximized = gtk_window_is_maximized(GTK_WINDOW(win));
  captured->fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(win));
  captured->minimized = wig_window_is_minimized(win);
  captured->monitor = g_strdup(wig_window_monitor_connector(win));

  /* The default size is the size to come back at: it holds the size the window
   * had before it was maximised or fullscreened, which is the one to restore. */
  gtk_window_get_default_size(GTK_WINDOW(win), &captured->width, &captured->height);
  captured->sidebar_width = gtk_paned_get_position(GTK_PANED(win->paned));

  guint n_tabs = win->tab_list ? wig_tab_list_get_n_tabs(win->tab_list) : 0;

  for (guint i = 0; i < n_tabs; i++) {
    WigTab *tab = wig_tab_list_get_nth(win->tab_list, i);
    wig_session_window_add_tab(captured, wig_tab_get_session_state(tab), wig_tab_get_title(tab), wig_tab_get_uri(tab),
                               wig_tab_get_web_view(tab) == wig_window_active_web_view(win), wig_tab_get_pinned(tab));
  }

  return captured;
}

WigWindow *wig_window_restore(WigApplication *app, const WigSessionWindow *saved, gboolean pinned_only)
{
  g_assert(saved != NULL);

  g_debug("session: restoring window %u with %d tab(s), %dx%d sidebar=%d on '%s' maximized=%d fullscreen=%d "
          "minimized=%d focused=%d",
          saved->window_id, g_slist_length(saved->tabs), saved->width, saved->height, saved->sidebar_width,
          saved->monitor ? saved->monitor : "", saved->maximized, saved->fullscreen, saved->minimized, saved->focused);

  WigWindow *win = get_window_by_id(app, saved->window_id);
  gboolean created = win == NULL;
  if (created) {
    win = g_object_new(WIG_TYPE_WINDOW, "id", saved->window_id, "application", app, NULL);
    if (saved->width > 0 && saved->height > 0)
      gtk_window_set_default_size(GTK_WINDOW(win), saved->width, saved->height);
    if (saved->sidebar_width > 0)
      gtk_paned_set_position(GTK_PANED(win->paned), saved->sidebar_width);
    if (saved->maximized)
      gtk_window_maximize(GTK_WINDOW(win));

    if (saved->fullscreen) {
      /* Fullscreen is the only state that can be aimed at a monitor: a window
       * cannot be placed, so anything else comes back where the compositor
       * decides to put it. */
      g_autoptr(GdkMonitor) monitor = wig_window_find_monitor(gtk_widget_get_display(GTK_WIDGET(win)), saved->monitor);
      if (monitor)
        gtk_window_fullscreen_on_monitor(GTK_WINDOW(win), monitor);
      else
        gtk_window_fullscreen(GTK_WINDOW(win));
    }
  }

  WigTab *focused_tab = NULL;
  for (const GSList *l = saved->tabs; l; l = l->next) {
    const WigSessionTab *saved_tab = l->data;
    if (pinned_only && !saved_tab->pinned)
      continue;

    /* Nothing is built for the tab until it is looked at: it holds its state and
     * shows the title and address the session recorded for it. */
    g_autoptr(WigTab) tab = wig_tab_new_discarded(saved_tab->state, saved_tab->title, saved_tab->uri);
    wig_tab_list_attach(win->tab_list, tab);

    if (saved_tab->pinned) {
      wig_tab_list_set_pinned(win->tab_list, tab, TRUE);
      wig_tab_load_discarded(tab);
    }
    if (saved_tab->was_focused || !focused_tab)
      focused_tab = tab;
  }

  /* Only the tab being looked at is built; the rest stay as they were saved. */
  if (focused_tab) {
    wig_tab_list_set_active(win->tab_list, focused_tab);
    wig_tab_load_discarded(focused_tab);
  }

  if (created) {
    gtk_widget_set_visible(GTK_WIDGET(win), TRUE);
    if (saved->minimized)
      gtk_window_minimize(GTK_WINDOW(win));
  }

  return win;
}

static void wig_window_restore_closed_window(WigWindow *win, WigSessionWindow *closed)
{
  WigWindow *target_win = wig_window_restore(wig_application_get(), closed, FALSE);

  if (target_win != win)
    gtk_window_present(GTK_WINDOW(target_win));
}

static void wig_window_undo_close_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WigApplication *app = wig_application_get();

  g_autoptr(WigSessionWindow) closed = wig_session_pop_closed_window(wig_application_get_session(app));
  if (!closed)
    return;

  wig_window_restore_closed_window(win, closed);
}

static void wig_window_restore_closed(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WigApplication *app = wig_application_get();
  guint index = g_variant_get_uint32(parameter);

  g_autoptr(WigSessionWindow) closed = wig_session_take_closed_window(wig_application_get_session(app), index);
  if (!closed) {
    g_debug("window: nothing closed at %u to restore", index);
    return;
  }

  wig_window_restore_closed_window(win, closed);
}

static void wig_window_tab_view_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigWindow *win)
{
  g_autoptr(GMenu) menu = g_menu_new();
  g_menu_append(menu, "Tab Bar", "app.tab-layout::horizontal");
  g_menu_append(menu, "Sidebar", "app.tab-layout::vertical");

  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  g_clear_pointer(&win->tab_view_context_menu, gtk_widget_unparent);
  win->tab_view_context_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  gtk_widget_set_parent(win->tab_view_context_menu, widget);
  gtk_popover_set_has_arrow(GTK_POPOVER(win->tab_view_context_menu), FALSE);
  GdkRectangle rect = { (int)x, (int)y, 1, 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(win->tab_view_context_menu), &rect);
  gtk_popover_popup(GTK_POPOVER(win->tab_view_context_menu));
}

static void wig_window_add_tab_view_context_menu(WigWindow *win, GtkWidget *widget)
{
  g_autoptr(GtkGestureClick) gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
  g_signal_connect_object(gesture, "pressed", G_CALLBACK(wig_window_tab_view_right_pressed), win, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(g_steal_pointer(&gesture)));
}

static void wig_window_update_tab_view_visibility(WigWindow *win)
{
  gboolean visible = !win->popup && !gtk_window_is_fullscreen(GTK_WINDOW(win));

  if (win->tab_bar)
    gtk_widget_set_visible(win->tab_bar, visible);
  if (win->tab_separator)
    gtk_widget_set_visible(win->tab_separator, visible);
  if (win->tab_sidebar)
    gtk_widget_set_visible(win->tab_sidebar, visible);
}

static void wig_window_show_tab_sidebar(WigWindow *win)
{
  g_clear_pointer(&win->tab_view_context_menu, gtk_widget_unparent);
  g_clear_pointer(&win->tab_bar, gtk_widget_unparent);
  g_clear_pointer(&win->tab_separator, gtk_widget_unparent);

  win->tab_sidebar = wig_tab_sidebar_new(win->tab_list);
  wig_tab_switcher_set_view(win->tab_switcher, WIG_TAB_LIST_VIEW(win->tab_sidebar));
  wig_window_add_tab_view_context_menu(win, win->tab_sidebar);
  gtk_paned_set_start_child(GTK_PANED(win->paned), win->tab_sidebar);
  wig_window_update_tab_view_visibility(win);
}

static void wig_window_show_tab_bar(WigWindow *win)
{
  g_clear_pointer(&win->tab_view_context_menu, gtk_widget_unparent);
  // FIXME: Error finding last focus widget of GtkPaned 0x55daa38feb70, gtk_paned_set_focus_child was called on widget
  // (nil) which is not child of ...
  gtk_paned_set_start_child(GTK_PANED(win->paned), NULL);
  win->tab_sidebar = NULL;

  win->tab_bar = wig_tab_bar_new(win->tab_list);
  wig_tab_switcher_set_view(win->tab_switcher, WIG_TAB_LIST_VIEW(win->tab_bar));
  wig_window_add_tab_view_context_menu(win, win->tab_bar);
  gtk_box_insert_child_after(GTK_BOX(win->content_box), win->tab_bar, NULL);

  win->tab_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_insert_child_after(GTK_BOX(win->content_box), win->tab_separator, win->tab_bar);

  wig_window_update_tab_view_visibility(win);
}

static void wig_window_set_tab_layout(WigWindow *win, WigTabLayout layout)
{
  win->tab_layout = layout;
  if (!win->paned)
    return;

  if (layout == WIG_TAB_LAYOUT_VERTICAL) {
    if (!win->tab_sidebar)
      wig_window_show_tab_sidebar(win);
  } else if (!win->tab_bar) {
    wig_window_show_tab_bar(win);
  }
}

static void wig_window_reload_middle_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigWindow *win)
{
  g_action_group_activate_action(G_ACTION_GROUP(win), "duplicate-active-tab", NULL);
}

static void wig_window_tab_duplicate(WigTabList *list, guint tab_id, WigWindow *win);

static void wig_window_duplicate_active_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WigTab *tab = wig_tab_list_get_active(win->tab_list);
  if (!tab)
    return;
  wig_window_tab_duplicate(win->tab_list, wig_tab_get_id(tab), win);
}

static const GActionEntry actions[] = {
  { "new-tab", wig_window_new_tab },
  { "next-tab", wig_window_next_tab },
  { "prev-tab", wig_window_prev_tab },
  { "focus-entry", wig_window_focus_entry },
  { "show-downloads", wig_window_show_downloads },
  { "show-history", wig_window_show_history },
  { "show-bookmarks", wig_window_show_bookmarks },
  { "bookmark-page", wig_window_bookmark_page },
  { "show-settings", wig_window_show_settings },
  { "toggle-inspector", wig_window_toggle_inspector },
  { "close-tab", wig_window_close_tab_action },
  { "undo-close-tab", wig_window_undo_close_tab },
  { "restore-closed", wig_window_restore_closed, "u" },
  { "duplicate-active-tab", wig_window_duplicate_active_tab },
  { "install-as-web-app", wig_window_install_as_web_app },
};

static void wig_window_open_uri_in_new_tab(WigWindow *win, GVariant *parameter, gboolean background)
{
  const char *uri = parameter ? g_variant_get_string(parameter, NULL) : NULL;
  g_autoptr(WebKitWebView) web_view = uri && *uri ? wig_application_create_web_view(wig_application_get())
                                                  : wig_window_create_web_view_for_new_tab(win);
  WigTab *tab = wig_window_add_tab_after_active(win, web_view);

  if (!background)
    wig_tab_list_set_active(win->tab_list, tab);

  if (uri && *uri)
    webkit_web_view_load_uri(web_view, uri);
}

static void wig_window_open_in_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_window_open_uri_in_new_tab(WIG_WINDOW(user_data), parameter, FALSE);
}

static void wig_window_open_in_background_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_window_open_uri_in_new_tab(WIG_WINDOW(user_data), parameter, TRUE);
}

static void wig_window_bookmark_uri(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WigBookmarkPopover *popover = WIG_BOOKMARK_POPOVER(win->link_bookmark_popover);
  const char *uri = NULL;
  const char *label = NULL;

  g_variant_get(parameter, "(&s&s)", &uri, &label);
  if (!uri || !*uri)
    return;

  wig_bookmark_popover_set_page(popover, uri, label && *label ? label : uri);
  if (!wig_bookmark_popover_get_can_bookmark(popover))
    return;

  wig_bookmark_popover_ensure_bookmarked(popover);

  WebKitWebView *web_view = wig_window_active_web_view(win);
  WPEView *wpe_view = web_view ? webkit_web_view_get_wpe_view(web_view) : NULL;
  GtkWidget *view = wpe_view ? wpe_view_gtk_get_widget(WPE_VIEW_GTK(wpe_view)) : NULL;
  graphene_point_t in_view = GRAPHENE_POINT_INIT((float)win->context_menu_target.x, (float)win->context_menu_target.y);
  graphene_point_t in_window;
  if (view && gtk_widget_compute_point(view, GTK_WIDGET(win), &in_view, &in_window))
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle) { (int)in_window.x, (int)in_window.y, 1, 1 });

  gtk_popover_popup(GTK_POPOVER(popover));
}

static void wig_window_copy_text(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(win)), g_variant_get_string(parameter, NULL));
}

static void wig_window_copy_selection(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindow *win = WIG_WINDOW(user_data);
  WebKitWebView *web_view = wig_window_active_web_view(win);
  if (!web_view)
    return;

  webkit_web_view_execute_editing_command(web_view, WEBKIT_EDITING_COMMAND_COPY);
}

static const GActionEntry context_menu_actions[] = {
  { "open-in-new-tab", wig_window_open_in_new_tab, "s" },
  { "open-in-background-tab", wig_window_open_in_background_tab, "s" },
  { "bookmark-uri", wig_window_bookmark_uri, "(ss)" },
  { "copy-text", wig_window_copy_text, "s" },
  { "copy-selection", wig_window_copy_selection },
};

static void wig_window_tab_reload(WigTabList *list, guint tab_id, WigWindow *win)
{
  WigTab *tab = wig_tab_list_get_by_id(list, tab_id);
  if (!tab)
    return;

  /* Reloading a discarded tab is what finally loads it. */
  if (wig_tab_get_discarded(tab)) {
    wig_tab_load_discarded(tab);
    return;
  }

  webkit_web_view_reload(wig_tab_get_web_view(tab));
}

static void wig_window_tab_mute(WigTabList *list, guint tab_id, WigWindow *win)
{
  WigTab *tab = wig_tab_list_get_by_id(list, tab_id);
  if (!tab)
    return;
  WebKitWebView *web_view = wig_tab_get_web_view(tab);
  if (!web_view)
    return;

  webkit_web_view_set_is_muted(web_view, !webkit_web_view_get_is_muted(web_view));
}

static void wig_window_tab_duplicate(WigTabList *list, guint tab_id, WigWindow *win)
{
  WigTab *tab = wig_tab_list_get_by_id(list, tab_id);
  if (!tab)
    return;
  const char *uri = wig_tab_get_uri(tab);

  if (uri && g_strcmp0(g_uri_peek_scheme(uri), "wig") == 0) {
    wig_application_open_internal_page(wig_application_get(), GTK_WINDOW(win), uri);
    return;
  }

  g_autoptr(WebKitWebView) web_view = uri && *uri ? wig_application_create_web_view(wig_application_get())
                                                  : wig_window_create_web_view_for_new_tab(win);
  WigTab *new_tab = wig_window_add_tab_for_view(win, web_view);

  if (uri && *uri)
    webkit_web_view_load_uri(web_view, uri);

  wig_tab_list_set_active(win->tab_list, new_tab);
}

static void wig_window_tab_copy_link(WigTabList *list, guint tab_id, WigWindow *win)
{
  WigTab *tab = wig_tab_list_get_by_id(list, tab_id);
  if (!tab)
    return;
  const char *uri = wig_tab_get_uri(tab);
  if (uri)
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(win)), uri);
}

static char *wig_window_current_page_uri(WigWindow *win)
{
  WebKitWebView *web_view = wig_window_active_web_view(win);
  const char *url = web_view ? webkit_web_view_get_uri(web_view) : NULL;

  g_autoptr(WigTab) tab = NULL;
  if (web_view && (!url || !*url)) {
    tab = wig_window_get_tab_for_web_view(win, web_view);
    if (tab)
      url = wig_tab_get_page_uri(tab);
  }

  if (!url || !*url || g_strcmp0(url, "about:blank") == 0 || g_strcmp0(g_uri_peek_scheme(url), "wig") == 0)
    return NULL;

  return g_strdup(url);
}

static void wig_window_bookmark_state_changed(WigWindow *win)
{
  gboolean bookmarked = wig_bookmark_popover_get_bookmarked(WIG_BOOKMARK_POPOVER(win->bookmark_popover));

  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(win->bookmark_button),
                                bookmarked ? "starred-symbolic" : "non-starred-symbolic");
  gtk_widget_set_tooltip_text(win->bookmark_button, bookmarked ? "Edit Bookmark" : "Bookmark This Page");
}

static void wig_window_update_bookmark_button(WigWindow *win)
{
  if (!win->bookmark_button)
    return;

  g_autofree char *uri = wig_window_current_page_uri(win);
  WebKitWebView *web_view = wig_window_active_web_view(win);
  g_autoptr(WigTab) tab = web_view ? wig_window_get_tab_for_web_view(win, web_view) : NULL;

  wig_bookmark_popover_set_page(WIG_BOOKMARK_POPOVER(win->bookmark_popover), uri, tab ? wig_tab_get_title(tab) : NULL);
  gtk_widget_set_sensitive(win->bookmark_button,
                           wig_bookmark_popover_get_can_bookmark(WIG_BOOKMARK_POPOVER(win->bookmark_popover)));
  wig_window_bookmark_state_changed(win);
}

static void wig_window_prepare_bookmark_popover(GtkMenuButton *button, gpointer user_data)
{
  WigWindow *win = user_data;

  wig_bookmark_popover_ensure_bookmarked(WIG_BOOKMARK_POPOVER(win->bookmark_popover));
}

static void wig_window_show_url(WigWindow *win, const char *url)
{
  /* A tab with nothing in it has no address worth showing, and an entry left
   * holding "about:blank" is something to be deleted before the address the
   * user opened the tab to type. */
  if (g_strcmp0(url, "about:blank") == 0 || uri_is_new_tab_page(url))
    url = NULL;

  win->suppress_entry_completion = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(win->url_entry), url ? url : "");
  win->suppress_entry_completion = FALSE;
  win->url_entry_edited = FALSE;
}

static void wig_window_update_url(WigWindow *win)
{
  wig_window_update_bookmark_button(win);
  wig_window_update_install_action(win);

  /* Half-typed input is worth more than the address of the page it is being
   * typed over: a load finishing, or a page changing its own address, must not
   * take it away. */
  if (win->url_entry_focused && win->url_entry_edited)
    return;

  WebKitWebView *web_view = wig_window_active_web_view(win);
  const char *url = web_view ? webkit_web_view_get_uri(web_view) : NULL;

  /* A load that failed before committing leaves the view without a URI, so the
   * address of the page the tab is showing in its place stands in for it. */
  g_autoptr(WigTab) tab = NULL;
  if (web_view && (!url || !*url)) {
    tab = wig_window_get_tab_for_web_view(win, web_view);
    if (tab)
      url = wig_tab_get_page_uri(tab);
  }

  wig_window_show_url(win, url);
}

static void wig_window_reset_url_entry(WigWindow *win)
{
  win->url_entry_edited = FALSE;
  wig_window_update_url(win);
}

static void wig_window_clear_load_progress(WigWindow *win)
{
  gtk_entry_set_progress_fraction(GTK_ENTRY(win->url_entry), 0);
  g_clear_handle_id(&win->progress_timeout_id, g_source_remove);
}

/* No further progress notifications arrive for a dead process, so the fraction
 * left in the entry would stay there for good. */
static void wig_window_web_process_terminated(WigWindow *win, WebKitWebProcessTerminationReason reason,
                                              WebKitWebView *web_view)
{
  if (web_view == wig_window_active_web_view(win))
    wig_window_clear_load_progress(win);
}

static void wig_window_update_load_progress(WigWindow *win)
{
  if (!wig_window_base_get_loading(WIG_WINDOW_BASE(win))) {
    wig_window_clear_load_progress(win);
    return;
  }

  WebKitWebView *web_view = wig_window_active_web_view(win);
  gdouble progress = web_view ? webkit_web_view_get_estimated_load_progress(web_view) : 0;
  gtk_entry_set_progress_fraction(GTK_ENTRY(win->url_entry), progress);
  g_clear_handle_id(&win->progress_timeout_id, g_source_remove);
  if (progress == 1.0)
    win->progress_timeout_id = g_timeout_add_once(500, (GSourceOnceFunc)wig_window_clear_load_progress, win);
}

static void wig_window_uri_changed(WigWindow *win, GParamSpec *pspec, WebKitWebView *web_view)
{
  wig_window_update_url(win);
}

static void wig_window_load_progress_changed(WigWindow *win, GParamSpec *pspec, WebKitWebView *web_view)
{
  wig_window_update_load_progress(win);
}

static void wig_window_load_uri(WigWindow *win, const char *uri)
{
  WebKitWebView *web_view = wig_window_active_web_view(win);
  g_debug("[load-uri] win=%p current_web_view=%p uri=%s", (void *)win, (void *)web_view, uri ? uri : "(null)");

  if (!web_view)
    return;

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  wig_application_mark_typed_navigation(app, web_view, uri);
  wig_application_mark_internal_navigation(app, web_view, uri);
  webkit_web_view_load_uri(web_view, uri);
  WigTab *selected = wig_tab_list_get_active(win->tab_list);
  if (selected)
    gtk_widget_grab_focus(wig_tab_get_widget(selected));
}

static void wig_window_load_url_completed_with(WigWindow *win, char *(*complete)(const char *, const char *))
{
  gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));

  /* Ignore an empty entry, otherwise strip surrounding spaces. */
  g_autofree char *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(win->url_entry))));
  if (!*text || !wig_window_active_web_view(win))
    return;

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  g_autofree char *search_engine = g_settings_get_string(wig_application_get_settings(app), "search-engine");
  g_autofree char *complete_uri = complete(text, search_engine);

  wig_window_show_url(win, complete_uri);
  wig_window_load_uri(win, complete_uri);
}

static void wig_window_load_url(WigWindow *win)
{
  wig_window_load_url_completed_with(win, wig_util_complete_uri);
}

static void wig_window_entry_completion_popover_activate(WigEntryCompletionPopover *popover, const char *uri,
                                                         WigWindow *win)
{
  wig_window_show_url(win, uri);
  gtk_popover_popdown(GTK_POPOVER(popover));
  wig_window_load_uri(win, uri);
}

static void wig_window_entry_completion_popover_selected(WigEntryCompletionPopover *popover, const char *text,
                                                         WigWindow *win)
{
  win->suppress_entry_completion = TRUE;
  gtk_editable_set_text(GTK_EDITABLE(win->url_entry), text);
  gtk_editable_set_position(GTK_EDITABLE(win->url_entry), -1);
  win->suppress_entry_completion = FALSE;
}

static void wig_window_update_entry_completion(WigWindow *win)
{
  if (win->suppress_entry_completion || !GTK_IS_POPOVER(win->entry_completion_popover))
    return;

  const char *text = gtk_editable_get_text(GTK_EDITABLE(win->url_entry));
  if (!win->url_entry_focused || !text || !*text) {
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
    return;
  }

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  WigHistoryStore *store = wig_application_get_history_store(app);
  if (!store) {
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
    return;
  }

  gboolean has_more = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) history_items = wig_history_store_query(store, text, 0, 10, &has_more, &error);
  if (!history_items) {
    g_warning("entry-completion: history query failed: %s", error->message);
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
    return;
  }

  g_autoptr(GPtrArray) completion_items = g_ptr_array_new_with_free_func(
      (GDestroyNotify)wig_entry_completion_item_free);
  for (guint i = 0; i < history_items->len; i++) {
    WigHistoryItem *history_item = g_ptr_array_index(history_items, i);
    const char *title = wig_history_item_get_title(history_item);
    const char *url = wig_history_item_get_url(history_item);
    g_ptr_array_add(completion_items, wig_entry_completion_item_new(title && *title ? title : url, url, url, url));
  }

  g_autofree char *search_engine = g_settings_get_string(wig_application_get_settings(app), "search-engine");
  wig_entry_completion_popover_set_items(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover), text,
                                         search_engine, completion_items);
  if (wig_entry_completion_popover_get_n_items(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover)) > 0) {
    int width = gtk_widget_get_width(win->url_entry);
    GdkRectangle pointing_to = {
      .x = 0,
      .y = gtk_widget_get_height(win->url_entry),
      .width = width,
      .height = 1,
    };
    wig_entry_completion_popover_set_width(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover), width);
    gtk_popover_set_pointing_to(GTK_POPOVER(win->entry_completion_popover), &pointing_to);
    gtk_popover_popup(GTK_POPOVER(win->entry_completion_popover));
  } else
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
}

static void wig_window_url_entry_changed(GtkEditable *editable, WigWindow *win)
{
  if (!win->suppress_entry_completion)
    win->url_entry_edited = TRUE;

  wig_window_update_entry_completion(win);
}

static gboolean wig_window_url_entry_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                                 GdkModifierType state, WigWindow *win)
{
  if ((state & gtk_accelerator_get_default_mod_mask()) == GDK_CONTROL_MASK
      && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_ISO_Enter)) {
    wig_window_load_url_completed_with(win, wig_util_complete_uri_dot_com);
    return TRUE;
  }

  if (!GTK_IS_POPOVER(win->entry_completion_popover) || !gtk_widget_get_visible(win->entry_completion_popover)
      || wig_entry_completion_popover_get_n_items(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover)) == 0)
    return FALSE;

  switch (keyval) {
  case GDK_KEY_Down:
  case GDK_KEY_KP_Down:
    return wig_entry_completion_popover_select_next(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover));
  case GDK_KEY_Up:
  case GDK_KEY_KP_Up:
    return wig_entry_completion_popover_select_previous(WIG_ENTRY_COMPLETION_POPOVER(win->entry_completion_popover));
  default:
    return FALSE;
  }
}

static void wig_window_url_entry_focus_enter(GtkEventControllerFocus *controller, WigWindow *win)
{
  win->url_entry_focused = TRUE;
}

static void wig_window_url_entry_focus_leave(GtkEventControllerFocus *controller, WigWindow *win)
{
  win->url_entry_focused = FALSE;

  /* Typing that was left behind is no longer being worked on, so the next
   * address the tab reports is free to replace it. */
  win->url_entry_edited = FALSE;

  if (GTK_IS_POPOVER(win->entry_completion_popover))
    gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
}

static gboolean point_is_inside_widget(GtkWidget *root, GtkWidget *widget, double x, double y)
{
  if (!widget || !gtk_widget_get_mapped(widget))
    return FALSE;

  graphene_point_t root_point = GRAPHENE_POINT_INIT((float)x, (float)y);
  graphene_point_t widget_point;
  if (!gtk_widget_compute_point(root, widget, &root_point, &widget_point))
    return FALSE;

  return widget_point.x >= 0 && widget_point.y >= 0 && widget_point.x < gtk_widget_get_width(widget)
      && widget_point.y < gtk_widget_get_height(widget);
}

static void wig_window_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigWindow *win)
{
  if (!GTK_IS_POPOVER(win->entry_completion_popover) || !gtk_widget_get_visible(win->entry_completion_popover))
    return;

  GtkWidget *root = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  if (point_is_inside_widget(root, win->url_entry, x, y)
      || point_is_inside_widget(root, win->entry_completion_popover, x, y))
    return;

  gtk_popover_popdown(GTK_POPOVER(win->entry_completion_popover));
}

static gboolean wig_window_open_link_in_pwa(WigWindow *win, WebKitNavigationAction *action)
{
  if (webkit_navigation_action_get_navigation_type(action) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
      || webkit_navigation_action_get_mouse_button(action) == WPE_BUTTON_MIDDLE
      || !webkit_navigation_action_is_user_gesture(action))
    return FALSE;

  WebKitURIRequest *request = webkit_navigation_action_get_request(action);
  if (g_strcmp0(webkit_uri_request_get_http_method(request), "GET") != 0)
    return FALSE;

  WigApplication *application = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  g_autoptr(GError) error = NULL;
  const char *uri = webkit_uri_request_get_uri(request);
  g_autofree char *id = wig_pwa_manager_find_for_primary_scope(wig_application_get_pwa_manager(application), uri,
                                                               &error);
  if (error) {
    g_warning("pwa: failed to match browser navigation: %s", error->message);
    return FALSE;
  }
  if (!id)
    return FALSE;
  if (!wig_application_open_web_app(application, id, uri, &error)) {
    g_warning("pwa: failed to open primary-scope browser link: %s", error->message);
    return FALSE;
  }

  g_debug("pwa: opened primary-scope browser link in app %s: %s", id, uri);
  return TRUE;
}

static const char *wig_decision_type_name(WebKitPolicyDecisionType type)
{
  switch (type) {
  case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
    return "NAVIGATION_ACTION";
  case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION:
    return "NEW_WINDOW_ACTION";
  case WEBKIT_POLICY_DECISION_TYPE_RESPONSE:
    return "RESPONSE";
  default:
    return "UNKNOWN";
  }
}

static const char *wig_navigation_type_name(WebKitNavigationType type)
{
  switch (type) {
  case WEBKIT_NAVIGATION_TYPE_LINK_CLICKED:
    return "LINK_CLICKED";
  case WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED:
    return "FORM_SUBMITTED";
  case WEBKIT_NAVIGATION_TYPE_BACK_FORWARD:
    return "BACK_FORWARD";
  case WEBKIT_NAVIGATION_TYPE_RELOAD:
    return "RELOAD";
  case WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED:
    return "FORM_RESUBMITTED";
  case WEBKIT_NAVIGATION_TYPE_OTHER:
    return "OTHER";
  default:
    return "UNKNOWN";
  }
}

static const char *wig_web_view_get_committed_uri(WebKitWebView *web_view)
{
  WebKitWebResource *resource = webkit_web_view_get_main_resource(web_view);
  return resource ? webkit_web_resource_get_uri(resource) : NULL;
}

static gboolean wig_window_decide_policy(WigWindow *win, WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType decision_type, WebKitWebView *web_view)
{
  g_debug("[decide-policy] win=%p decision=%p type=%s (%d)", (void *)win, (void *)decision,
          wig_decision_type_name(decision_type), decision_type);

  // RESPONSE decisions are handled at the application level (on_web_view_decide_policy).
  if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION
      && decision_type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;

  WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
      WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  if (wig_window_open_link_in_pwa(win, action)) {
    webkit_policy_decision_ignore(decision);
    return TRUE;
  }

  WebKitNavigationType nav_type = webkit_navigation_action_get_navigation_type(action);
  WebKitURIRequest *request = webkit_navigation_action_get_request(action);
  const char *request_uri = request ? webkit_uri_request_get_uri(request) : NULL;
  const char *frame_name = webkit_navigation_action_get_frame_name(action);

  g_debug("[decide-policy]   nav_type=%s (%d) uri=%s button=%u frame_name=%s", wig_navigation_type_name(nav_type),
          nav_type, request_uri ? request_uri : "(null)", webkit_navigation_action_get_mouse_button(action),
          frame_name ? frame_name : "(null)");

  const char *target_scheme = request_uri ? g_uri_peek_scheme(request_uri) : NULL;

  g_autofree char *moved_uri = wig_settings_page_moved_uri(request_uri);
  if (moved_uri) {
    g_debug("wig: '%s' now lives at '%s'", request_uri, moved_uri);
    webkit_policy_decision_ignore(decision);
    webkit_web_view_load_uri(web_view, moved_uri);
    return TRUE;
  }

  if (g_strcmp0(target_scheme, "wig") == 0
      && !wig_application_take_internal_navigation(wig_application_get(), web_view, request_uri)) {
    const char *current_uri = webkit_web_view_get_uri(web_view);
    const char *current_scheme = current_uri ? g_uri_peek_scheme(current_uri) : NULL;
    if (g_strcmp0(current_scheme, "wig") != 0) {
      g_warning("wig: rejecting navigation to '%s' from '%s'", request_uri, current_uri ? current_uri : "(null)");
      webkit_policy_decision_ignore(decision);
      return TRUE;
    }
  }

  if (g_strcmp0(target_scheme, "wig") == 0
      && !wig_util_uris_are_same_page(wig_web_view_get_committed_uri(web_view), request_uri)
      && wig_application_focus_internal_page(wig_application_get(), request_uri, web_view)) {
    g_debug("wig: focusing the page that is already open instead of '%s'", request_uri);
    webkit_policy_decision_ignore(decision);
    return TRUE;
  }

  /* Ctrl + primary-clicking and middle-clicking a link opens it in a
   * background tab. Ctrl + Shift + Primary-clicking and a link aimed at
   * another frame (target="_blank") opens a foreground tab. Shift +
   * Primary-clicking a link opens a new window; letting the decision through
   * would make WebKit ask for a whole new window instead. */
  enum {
    TARGET_BG_TAB,
    TARGET_FG_TAB,
    TARGET_NEW_WINDOW,
    TARGET_THIS_WINDOW,
  } target
      = decision_type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION ? TARGET_FG_TAB : TARGET_THIS_WINDOW;

  if (nav_type == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
    guint modifiers = webkit_navigation_action_get_modifiers(action)
        & (WPE_MODIFIER_KEYBOARD_CONTROL | WPE_MODIFIER_KEYBOARD_SHIFT);
    switch (webkit_navigation_action_get_mouse_button(action)) {
    case WPE_BUTTON_PRIMARY:
      switch (modifiers) {
      case WPE_MODIFIER_KEYBOARD_CONTROL:
        target = TARGET_BG_TAB;
        break;
      case WPE_MODIFIER_KEYBOARD_SHIFT:
        target = TARGET_NEW_WINDOW;
        break;
      case WPE_MODIFIER_KEYBOARD_CONTROL | WPE_MODIFIER_KEYBOARD_SHIFT:
        target = TARGET_FG_TAB;
        break;
      }
      break;
    case WPE_BUTTON_MIDDLE:
      if (modifiers & WPE_MODIFIER_KEYBOARD_SHIFT)
        target = TARGET_FG_TAB;
      else
        target = TARGET_BG_TAB;
      break;
    }
  }

  if (target != TARGET_THIS_WINDOW) {
    g_autoptr(WebKitWebView) new_view = wig_application_create_web_view(wig_application_get());
    if (target == TARGET_NEW_WINDOW) {
      WigWindow *new_win = wig_window_new(wig_application_get());
      wig_window_match_window(new_win, win);
      wig_window_add_web_view(new_win, new_view);
      gtk_window_present(GTK_WINDOW(new_win));
    } else {
      WigTab *tab = wig_window_add_tab_after_active(win, new_view);
      if (target == TARGET_FG_TAB)
        wig_tab_list_set_active(win->tab_list, tab);
    }
    webkit_web_view_load_request(new_view, request);
    webkit_policy_decision_ignore(decision);
    return TRUE;
  }

  /* Built per navigation rather than at construction, so both a changed setting
   * and whatever has been decided about the site being navigated to take effect
   * without reopening the tab. */
  g_autoptr(WebKitWebsitePolicies) policies = wig_application_create_website_policies(wig_application_get(),
                                                                                      request_uri);
  webkit_policy_decision_use_with_policies(decision, policies);
  return TRUE;
}

/* The location bar stays even when the page asked for it to go, read-only, so a
 * popup cannot dress itself up as a page from somewhere else. */
static void wig_window_set_popup(WigWindow *win, gboolean popup)
{
  if (win->popup == popup)
    return;

  win->popup = popup;
  gtk_widget_set_visible(win->toolbar_box, !popup);
  gtk_editable_set_editable(GTK_EDITABLE(win->url_entry), !popup);
  gtk_widget_set_focusable(win->url_entry, !popup);
  wig_window_update_tab_view_visibility(win);
}

#if HAVE_WINDOW_GEOMETRY_SUPPORT
static void wig_window_get_chrome_size(WigWindow *win, int *width, int *height)
{
  GtkWidget *bars[] = { win->header_bar, win->tab_bar, win->tab_separator };

  *width = 0;
  *height = 0;
  for (guint i = 0; i < G_N_ELEMENTS(bars); i++) {
    if (!bars[i] || !gtk_widget_get_visible(bars[i]))
      continue;

    int natural;
    gtk_widget_measure(bars[i], GTK_ORIENTATION_VERTICAL, -1, NULL, &natural, NULL, NULL);
    *height += natural;
  }

  if (win->tab_sidebar && gtk_widget_get_visible(win->tab_sidebar))
    *width += gtk_paned_get_position(GTK_PANED(win->paned));
}

/* The size a page asks for is the size of the area it is shown in, which on WPE
 * is all of its window a page can see, so the bars around that area go on top.
 * A window cannot be placed, so the position is left to the compositor. */
static void wig_window_apply_page_geometry(WigWindow *win, WebKitWindowProperties *properties)
{
  if (!webkit_window_properties_get_has_custom_geometry(properties))
    return;

  WebKitRectangle geometry;
  webkit_window_properties_get_geometry(properties, &geometry);
  if (geometry.width <= 0 || geometry.height <= 0)
    return;

  int chrome_width, chrome_height;
  wig_window_get_chrome_size(win, &chrome_width, &chrome_height);
  g_debug("window %u: the page asked for %dx%d", wig_window_base_get_id(WIG_WINDOW_BASE(win)), geometry.width,
          geometry.height);
  gtk_window_set_default_size(GTK_WINDOW(win), geometry.width + chrome_width, geometry.height + chrome_height);
}

/* resizeTo() and friends only reach a window the page was given to itself: once
 * it holds other tabs, or was torn into another window, it is the user's. */
static void wig_window_web_view_geometry_changed(WebKitWebView *web_view, GParamSpec *pspec,
                                                 WebKitWindowProperties *properties)
{
  GtkWidget *widget = wpe_view_gtk_get_widget(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(web_view)));
  GtkRoot *root = gtk_widget_get_root(widget);
  if (!WIG_IS_WINDOW(root))
    return;

  WigWindow *win = WIG_WINDOW(root);
  if (wig_tab_list_get_n_tabs(win->tab_list) != 1 || gtk_window_is_maximized(GTK_WINDOW(win))
      || gtk_window_is_fullscreen(GTK_WINDOW(win)))
    return;

  wig_window_apply_page_geometry(win, properties);
}
#endif

static void wig_window_unshown_web_view_closed(WigWindow *win, WebKitWebView *web_view)
{
  g_ptr_array_remove(win->unshown_web_views, web_view);
}

/* Whether a page gets a popup is only known once it asks to be shown, so the
 * view waits with the window whose page opened it until then. A popup keeps a
 * window of its own; it is also the one window that does not take tabs. */
static void wig_window_web_view_ready_to_show(WigWindow *win, WebKitWebView *web_view)
{
  guint index;
  if (!g_ptr_array_find(win->unshown_web_views, web_view, &index))
    return;

  g_autoptr(WebKitWebView) owned_web_view = g_ptr_array_steal_index(win->unshown_web_views, index);
  g_signal_handlers_disconnect_by_func(web_view, wig_window_unshown_web_view_closed, win);
  g_signal_handlers_disconnect_by_func(web_view, wig_window_web_view_ready_to_show, win);

  gboolean popup = FALSE;
#if HAVE_WINDOW_POPUP_SUPPORT
  WebKitWindowProperties *properties = webkit_web_view_get_window_properties(web_view);
  popup = webkit_window_properties_get_is_popup(properties);
#endif

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  if (!popup && !win->popup && g_settings_get_boolean(wig_application_get_settings(app), "new-windows-in-tabs")) {
    g_debug("window %u: the page asked for a window, opening a tab instead",
            wig_window_base_get_id(WIG_WINDOW_BASE(win)));
    wig_tab_list_set_active(win->tab_list, wig_window_add_tab_after_active(win, web_view));
    return;
  }

  WigWindow *new_win = wig_window_new(app);
  wig_window_add_web_view(new_win, web_view);

#if HAVE_WINDOW_POPUP_SUPPORT
  g_debug("window %u: the page asked for %s", wig_window_base_get_id(WIG_WINDOW_BASE(new_win)),
          popup ? "a popup" : "a window");
  wig_window_set_popup(new_win, popup);

#if HAVE_WINDOW_GEOMETRY_SUPPORT
  wig_window_apply_page_geometry(new_win, properties);
  g_signal_connect_object(properties, "notify::geometry", G_CALLBACK(wig_window_web_view_geometry_changed), web_view,
                          G_CONNECT_SWAPPED);
#endif
#endif

  gtk_widget_grab_focus(wpe_view_gtk_get_widget(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(web_view))));
  gtk_window_present(GTK_WINDOW(new_win));
}

static WebKitWebView *wig_window_web_view_create(WigWindow *win, WebKitNavigationAction *navigation,
                                                 WebKitWebView *opener)
{
  if (wig_window_open_link_in_pwa(win, navigation))
    return NULL;

  const char *target_uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(navigation));
  const char *target_scheme = g_uri_peek_scheme(target_uri);
  if (g_strcmp0(target_scheme, "wig") == 0) {
    const char *current_uri = webkit_web_view_get_uri(opener);
    const char *current_scheme = current_uri ? g_uri_peek_scheme(current_uri) : NULL;
    if (g_strcmp0(current_scheme, "wig") != 0) {
      g_warning("wig: rejecting popup to '%s' from '%s'", target_uri, current_uri ? current_uri : "(null)");
      return NULL;
    }
  }

  g_autoptr(WebKitWebView) web_view = WEBKIT_WEB_VIEW(g_object_new(
      WEBKIT_TYPE_WEB_VIEW, "related-view", opener, "settings", webkit_web_view_get_settings(opener), NULL));

  g_ptr_array_add(win->unshown_web_views, g_object_ref(web_view));
  g_signal_connect_object(web_view, "ready-to-show", G_CALLBACK(wig_window_web_view_ready_to_show), win,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "close", G_CALLBACK(wig_window_unshown_web_view_closed), win, G_CONNECT_SWAPPED);
  return g_steal_pointer(&web_view);
}

static gboolean wig_window_web_view_context_menu(WigWindow *win, WebKitContextMenu *context_menu,
                                                 WebKitHitTestResult *hit_test_result, WebKitWebView *web_view)
{
  if (web_view != wig_window_active_web_view(win))
    return FALSE;

  /* Anything the menu goes on to offer is about what was under the pointer, so
   * a popover it opens belongs there rather than wherever the window last was. */
  win->context_menu_target = (GdkRectangle) { 0, 0, 1, 1 };
  webkit_context_menu_get_position(context_menu, &win->context_menu_target.x, &win->context_menu_target.y);

  return wig_window_base_show_context_menu(WIG_WINDOW_BASE(win), context_menu, hit_test_result);
}

static void wig_window_on_mouse_target_changed(WigWindow *win, WebKitHitTestResult *hit_test_result, guint modifiers,
                                               WebKitWebView *web_view)
{
  WigTab *tab = wig_tab_list_get_active(win->tab_list);
  if (!tab)
    return;

  const char *uri = NULL;
  if (webkit_hit_test_result_context_is_link(hit_test_result))
    uri = webkit_hit_test_result_get_link_uri(hit_test_result);

  wig_tab_set_hovered_link(tab, uri, wig_window_base_get_active_origin(WIG_WINDOW_BASE(win)));
}

static void wig_window_update_state_changed(WigWindow *win, GParamSpec *pspec, WigUpdateMonitor *monitor)
{
  switch (wig_update_monitor_get_state(monitor)) {
  case WIG_UPDATE_STATE_AVAILABLE:
    gtk_button_set_label(GTK_BUTTON(win->update_button), "Download Update");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(win->update_button), "app.download-update");
    gtk_widget_set_tooltip_text(win->update_button, NULL);
    gtk_widget_set_sensitive(win->update_button, TRUE);
    gtk_widget_set_visible(win->update_button, TRUE);
    break;
  case WIG_UPDATE_STATE_DOWNLOADING:
    gtk_button_set_label(GTK_BUTTON(win->update_button), "Downloading Update…");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(win->update_button), NULL);
    gtk_widget_set_tooltip_text(win->update_button, NULL);
    gtk_widget_set_sensitive(win->update_button, FALSE);
    gtk_widget_set_visible(win->update_button, TRUE);
    break;
  case WIG_UPDATE_STATE_READY:
    gtk_button_set_label(GTK_BUTTON(win->update_button), "Restart to Update");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(win->update_button), "app.restart-to-update");
    gtk_widget_set_tooltip_text(win->update_button, NULL);
    gtk_widget_set_sensitive(win->update_button, TRUE);
    gtk_widget_set_visible(win->update_button, TRUE);
    break;
  case WIG_UPDATE_STATE_BLOCKED:
    gtk_button_set_label(GTK_BUTTON(win->update_button), "Update Blocked");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(win->update_button), NULL);
    gtk_widget_set_tooltip_text(win->update_button, "wig is not allowed to update itself");
    gtk_widget_set_sensitive(win->update_button, FALSE);
    gtk_widget_set_visible(win->update_button, TRUE);
    break;
  case WIG_UPDATE_STATE_FAILED: {
    g_autofree char *tooltip = g_strdup_printf("%s. Click to try again.", wig_update_monitor_get_failure(monitor));
    gtk_button_set_label(GTK_BUTTON(win->update_button), "Update Failed");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(win->update_button), "app.download-update");
    gtk_widget_set_tooltip_text(win->update_button, tooltip);
    gtk_widget_set_sensitive(win->update_button, TRUE);
    gtk_widget_set_visible(win->update_button, TRUE);
    break;
  }
  case WIG_UPDATE_STATE_MANUAL:
    gtk_button_set_label(GTK_BUTTON(win->update_button), "Manual Update Required");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(win->update_button), NULL);
    gtk_widget_set_tooltip_text(win->update_button,
                                "This update changes wig's permissions, so it has to be installed from your "
                                "software manager");
    gtk_widget_set_sensitive(win->update_button, FALSE);
    gtk_widget_set_visible(win->update_button, TRUE);
    break;
  case WIG_UPDATE_STATE_NONE:
    gtk_widget_set_visible(win->update_button, FALSE);
    break;
  }
}

static void wig_window_fullscreen_changed(WigWindow *win)
{
  gtk_widget_set_visible(win->header_bar, !gtk_window_is_fullscreen(GTK_WINDOW(win)));
  wig_window_update_tab_view_visibility(win);
}

static void wig_window_tab_wants_attention(WigWindow *win, WigTab *tab)
{
  g_debug("tab %u: prompting, bringing it to the front", wig_tab_get_id(tab));
  wig_tab_list_set_active(win->tab_list, tab);
  gtk_window_present(GTK_WINDOW(win));
}

/* A discarded tab has no view to be wired to, and the one it builds when it is
 * looked at again is not the one this window was told about. */
static void wig_window_tab_web_view_changed(WigWindow *win, WebKitWebView *old_view, WigTab *tab)
{
  if (old_view)
    wig_window_detach_web_view(win, old_view);

  WebKitWebView *web_view = wig_tab_get_web_view(tab);
  if (web_view)
    wig_window_attach_web_view(win, web_view);
}

static void wig_window_tab_added(WigTabList *list, WigTab *tab, guint position, WigWindow *win)
{
  if (win->popup && wig_tab_list_get_n_tabs(list) > 1)
    wig_window_set_popup(win, FALSE);

  g_signal_connect_object(tab, "wants-attention", G_CALLBACK(wig_window_tab_wants_attention), win, G_CONNECT_SWAPPED);
  g_signal_connect_object(tab, "web-view-changed", G_CALLBACK(wig_window_tab_web_view_changed), win, G_CONNECT_SWAPPED);
  gtk_stack_add_child(GTK_STACK(win->tab_stack), wig_tab_get_widget(tab));

  WebKitWebView *web_view = wig_tab_get_web_view(tab);
  if (web_view)
    wig_window_attach_web_view(win, web_view);
  wig_session_queue_save(wig_application_get_session(wig_application_get()));
}

static void wig_window_tab_moved(WigTabList *list, WigTab *tab, guint old_index, guint new_index, WigWindow *win)
{
  wig_session_queue_save(wig_application_get_session(wig_application_get()));
}

static void wig_window_tab_removed(WigTabList *list, WigTab *tab, guint position, WigWindow *win)
{
  WebKitWebView *web_view = wig_tab_get_web_view(tab);
  if (web_view)
    wig_window_detach_web_view(win, web_view);
  gtk_stack_remove(GTK_STACK(win->tab_stack), wig_tab_get_widget(tab));
  wig_session_queue_save(wig_application_get_session(wig_application_get()));
}

static void wig_window_active_tab_changed(WigWindow *win, GParamSpec *pspec, WigTabList *list)
{
  WigTab *tab = wig_tab_list_get_active(win->tab_list);

  /* Looking at a discarded tab builds its view again, so this comes first and
   * everything below sees the view the tab now has. */
  if (tab)
    wig_tab_load_discarded(tab);

  WebKitWebView *web_view = tab ? wig_tab_get_web_view(tab) : NULL;
  g_signal_group_set_target(win->active_web_view_signals, web_view);
  g_signal_group_set_target(win->active_tab_signals, tab);
  wig_window_base_set_active_web_view(WIG_WINDOW_BASE(win), web_view);

  wig_window_bind_tab_loading(win, tab);

  if (tab)
    gtk_stack_set_visible_child(GTK_STACK(win->tab_stack), wig_tab_get_widget(tab));

  wig_window_base_set_active_tab(WIG_WINDOW_BASE(win), tab);

  wig_window_reset_url_entry(win);
#if HAVE_APPLICATION_MANIFEST_SUPPORT
  wig_window_update_pwa_button(win);
#endif

  wig_window_update_load_progress(win);

  wig_session_queue_save(wig_application_get_session(wig_application_get()));
}

static WigTab *wig_window_create_tab(WigWindow *win)
{
  wig_window_new_tab(NULL, NULL, win);
  return wig_tab_list_get_active(win->tab_list);
}

/* Find which window in this application owns a given tab id. */
static WigWindow *wig_window_find_owner(WigApplication *app, guint32 tab_id)
{
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
  for (GList *l = windows; l; l = g_list_next(l)) {
    if (!WIG_IS_WINDOW(l->data))
      continue;
    WigWindow *w = WIG_WINDOW(l->data);
    if (wig_tab_list_get_by_id(w->tab_list, tab_id))
      return w;
  }
  return NULL;
}

/* tab.move-to(uub): move tab_id to this window at insert_index, pinned or not
 * according to where it was dropped.  If the tab already lives here, this is a
 * reorder. */
/* Dragging a tab that is part of a selection drags the whole selection with it;
 * dragging anything else moves just that tab.
 *
 * Returns: (transfer full): the tabs to move, in list order. */
static GPtrArray *wig_window_tabs_to_move(WigWindow *win, WigTab *tab)
{
  if (wig_tab_get_selected(tab))
    return wig_tab_list_get_selected(win->tab_list);

  GPtrArray *tabs = g_ptr_array_new_with_free_func(g_object_unref);
  g_ptr_array_add(tabs, g_object_ref(tab));
  return tabs;
}

static void wig_window_move_tab_to(GtkWidget *widget, const char *action_name, GVariant *parameter)
{
  WigWindow *dst = WIG_WINDOW(widget);
  guint32 tab_id, insert_index;
  gboolean pinned;
  g_variant_get(parameter, "(uub)", &tab_id, &insert_index, &pinned);

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(dst)));
  WigWindow *src = wig_window_find_owner(app, tab_id);
  if (!src)
    return;

  WigTab *tab = wig_tab_list_get_by_id(src->tab_list, tab_id);
  if (!tab)
    return;

  g_autoptr(GPtrArray) tabs = wig_window_tabs_to_move(src, tab);

  if (src == dst) {
    /* Same window — reorder only.  Pinning first moves the tabs into the block
     * they were dropped in, so the index below is applied within that block. */
    for (guint i = 0; i < tabs->len; i++)
      wig_tab_list_set_pinned(dst->tab_list, g_ptr_array_index(tabs, i), pinned);

    wig_tab_list_move_many(dst->tab_list, tabs, MIN((guint)insert_index, wig_tab_list_get_n_tabs(dst->tab_list)));
    return;
  }

  guint target = MIN((guint)insert_index, wig_tab_list_get_n_tabs(dst->tab_list));
  for (guint i = 0; i < tabs->len; i++) {
    WigTab *moving = g_ptr_array_index(tabs, i);

    /* Hold a ref so the widget survives gtk_stack_remove in wig_tab_list_detach. */
    g_autoptr(GtkWidget) tab_widget = g_object_ref(wig_tab_get_widget(moving));
    g_autoptr(WigTab) owned_tab = wig_tab_list_detach(src->tab_list, moving);

    wig_tab_list_attach(dst->tab_list, owned_tab);
    wig_tab_list_set_pinned(dst->tab_list, owned_tab, pinned);
    /* attach appends, so pull the tab back to the drop point and let the rest of
     * the run follow it. */
    wig_tab_list_move(dst->tab_list, owned_tab, target);
    target = wig_tab_list_index_of(dst->tab_list, owned_tab) + 1;
  }

  wig_tab_list_set_active(dst->tab_list, tab);
  gtk_window_present(GTK_WINDOW(dst));

  /* The source has nothing left to show once its last tab has moved out. */
  if (wig_tab_list_get_n_tabs(src->tab_list) == 0)
    gtk_window_destroy(GTK_WINDOW(src));
}

/* A torn-off tab keeps the shape of where it came from, so the window it lands
 * in opens the size and state of the one it left. */
static void wig_window_match_window(WigWindow *win, WigWindow *other)
{
  int width = 0, height = 0;

  /* The default size is the one to come back at: it holds the size the window
   * had before it was maximised or fullscreened. */
  gtk_window_get_default_size(GTK_WINDOW(other), &width, &height);
  if (width > 0 && height > 0)
    gtk_window_set_default_size(GTK_WINDOW(win), width, height);

  gtk_paned_set_position(GTK_PANED(win->paned), gtk_paned_get_position(GTK_PANED(other->paned)));

  if (gtk_window_is_maximized(GTK_WINDOW(other)))
    gtk_window_maximize(GTK_WINDOW(win));

  if (gtk_window_is_fullscreen(GTK_WINDOW(other))) {
    g_autoptr(GdkMonitor) monitor = wig_window_find_monitor(gtk_widget_get_display(GTK_WIDGET(other)),
                                                            wig_window_monitor_connector(other));
    if (monitor)
      gtk_window_fullscreen_on_monitor(GTK_WINDOW(win), monitor);
    else
      gtk_window_fullscreen(GTK_WINDOW(win));
  }
}

/* Both the context menu and a tab let go outside the windows come through
 * tab.detach, so the two agree on what moves and when it is refused. */
static void wig_window_tab_detach_requested(WigTabList *list, guint tab_id, WigWindow *win)
{
  gtk_widget_activate_action(GTK_WIDGET(win), "tab.detach", "u", tab_id);
}

static void wig_window_detach_tab(GtkWidget *widget, const char *action_name, GVariant *parameter)
{
  WigWindow *win = WIG_WINDOW(widget);
  guint32 tab_id = g_variant_get_uint32(parameter);
  WigTab *tab = wig_tab_list_get_by_id(win->tab_list, tab_id);
  if (!tab)
    return;

  g_autoptr(GPtrArray) tabs = wig_window_tabs_to_move(win, tab);

  /* Tearing every tab off into a fresh window would just swap one window for
   * another. */
  if (tabs->len >= wig_tab_list_get_n_tabs(win->tab_list))
    return;

  WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(win)));
  WigWindow *new_win = wig_window_new(app);
  wig_window_match_window(new_win, win);

  for (guint i = 0; i < tabs->len; i++) {
    /* Detach reuses the existing WigTab — no close-tab signal, no history save.
     * This fires tab-removed on the old list, which removes the WPE widget from
     * the old stack. We hold a ref so it survives unparenting. */
    g_autoptr(GtkWidget) tab_widget = g_object_ref(wig_tab_get_widget(g_ptr_array_index(tabs, i)));
    g_autoptr(WigTab) owned_tab = wig_tab_list_detach(win->tab_list, g_ptr_array_index(tabs, i));

    wig_tab_list_attach(new_win->tab_list, owned_tab);
  }

  wig_tab_list_set_active(new_win->tab_list, tab);
  gtk_window_present(GTK_WINDOW(new_win));
}

static void wig_window_constructed(GObject *object)
{
  G_OBJECT_CLASS(wig_window_parent_class)->constructed(object);

  WigWindow *win = WIG_WINDOW(object);
  g_action_map_add_action_entries(G_ACTION_MAP(win), actions, G_N_ELEMENTS(actions), win);

  win->context_menu_action_group = G_ACTION_GROUP(g_simple_action_group_new());
  g_action_map_add_action_entries(G_ACTION_MAP(win->context_menu_action_group), context_menu_actions,
                                  G_N_ELEMENTS(context_menu_actions), win);
  gtk_widget_insert_action_group(GTK_WIDGET(win), "popup", win->context_menu_action_group);

  g_signal_connect(win, "notify::fullscreened", G_CALLBACK(wig_window_fullscreen_changed), NULL);

  GtkGestureClick *window_click = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(window_click), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(window_click), GTK_PHASE_CAPTURE);
  g_signal_connect_object(window_click, "pressed", G_CALLBACK(wig_window_click_pressed), win, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(GTK_WIDGET(win), GTK_EVENT_CONTROLLER(window_click));

  win->toolbar_view = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(win->toolbar_view), ADW_TOOLBAR_FLAT);

  win->header_bar = gtk_header_bar_new();

  win->toolbar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(box, "navigation-box");

  win->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->back_button), "win.go-back");
  gtk_widget_add_css_class(win->back_button, "toolbar-button");
  gtk_box_append(GTK_BOX(box), win->back_button);

  win->forward_button = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->forward_button), "win.go-forward");
  gtk_widget_add_css_class(win->forward_button, "toolbar-button");
  gtk_box_append(GTK_BOX(box), win->forward_button);

  wig_window_base_set_navigation_buttons(WIG_WINDOW_BASE(win), win->back_button, win->forward_button);
  gtk_box_append(GTK_BOX(win->toolbar_box), box);

  win->stop_reload_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(win->stop_reload_button), "win.stop-reload");
  gtk_widget_add_css_class(win->stop_reload_button, "toolbar-button");
  gtk_box_append(GTK_BOX(win->toolbar_box), win->stop_reload_button);

  GtkGestureClick *reload_middle = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(reload_middle), GDK_BUTTON_MIDDLE);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(reload_middle), GTK_PHASE_CAPTURE);
  g_signal_connect_object(reload_middle, "pressed", G_CALLBACK(wig_window_reload_middle_pressed), win,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(win->stop_reload_button, GTK_EVENT_CONTROLLER(reload_middle));

  gtk_header_bar_pack_start(GTK_HEADER_BAR(win->header_bar), win->toolbar_box);

  win->url_entry = gtk_entry_new();
  win->entry_completion_popover = wig_entry_completion_popover_new();
  gtk_widget_set_parent(win->entry_completion_popover, win->url_entry);
  g_signal_connect_object(win->entry_completion_popover, "activate",
                          G_CALLBACK(wig_window_entry_completion_popover_activate), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->entry_completion_popover, "selected",
                          G_CALLBACK(wig_window_entry_completion_popover_selected), win, G_CONNECT_DEFAULT);

  g_signal_connect_object(win->url_entry, "activate", G_CALLBACK(wig_window_load_url), win, G_CONNECT_SWAPPED);
  g_signal_connect_object(win->url_entry, "changed", G_CALLBACK(wig_window_url_entry_changed), win, G_CONNECT_DEFAULT);
  gtk_widget_set_hexpand(win->url_entry, TRUE);

  GtkEventController *url_focus = gtk_event_controller_focus_new();
  g_signal_connect_object(url_focus, "enter", G_CALLBACK(wig_window_url_entry_focus_enter), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(url_focus, "leave", G_CALLBACK(wig_window_url_entry_focus_leave), win, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(win->url_entry, url_focus);

  GtkEventController *url_keys = gtk_event_controller_key_new();
  g_signal_connect_object(url_keys, "key-pressed", G_CALLBACK(wig_window_url_entry_key_pressed), win,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(win->url_entry, url_keys);

  GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(entry_box, "linked");
  gtk_box_append(GTK_BOX(entry_box), wig_window_base_get_permissions_button(WIG_WINDOW_BASE(win)));
  gtk_box_append(GTK_BOX(entry_box), win->url_entry);
#if HAVE_APPLICATION_MANIFEST_SUPPORT
  win->pwa_button = gtk_button_new_from_icon_name("system-software-install-symbolic");
  gtk_widget_add_css_class(win->pwa_button, "flat");
  gtk_widget_set_tooltip_text(win->pwa_button, "Install web app");
  gtk_widget_set_visible(win->pwa_button, FALSE);
  g_signal_connect_object(win->pwa_button, "clicked", G_CALLBACK(wig_window_pwa_button_clicked), win,
                          G_CONNECT_SWAPPED);
  gtk_box_append(GTK_BOX(entry_box), win->pwa_button);
  g_signal_connect_object(wig_application_get_pwa_manager(wig_application_get()), "changed",
                          G_CALLBACK(wig_window_pwa_manager_changed), win, G_CONNECT_SWAPPED);
  g_signal_connect_object(wig_application_get_pwa_manager(wig_application_get()), "removed",
                          G_CALLBACK(wig_window_pwa_manager_changed), win, G_CONNECT_SWAPPED);
#endif

  win->link_bookmark_popover = wig_bookmark_popover_new();
  gtk_widget_set_parent(win->link_bookmark_popover, GTK_WIDGET(win));

  win->bookmark_popover = g_object_ref_sink(wig_bookmark_popover_new());
  g_signal_connect_object(win->bookmark_popover, "notify::bookmarked", G_CALLBACK(wig_window_bookmark_state_changed),
                          win, G_CONNECT_SWAPPED);

  win->bookmark_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(win->bookmark_button), "non-starred-symbolic");
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(win->bookmark_button), win->bookmark_popover);
  gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(win->bookmark_button), wig_window_prepare_bookmark_popover, win,
                                        NULL);
  gtk_widget_set_tooltip_text(win->bookmark_button, "Bookmark This Page");
  gtk_widget_set_focusable(win->bookmark_button, FALSE);
  gtk_box_append(GTK_BOX(entry_box), win->bookmark_button);

  GtkWidget *clamp = adw_clamp_new();
  gtk_widget_set_hexpand(clamp, TRUE);
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 860);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 560);
  adw_clamp_set_child(ADW_CLAMP(clamp), entry_box);
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(win->header_bar), clamp);

  g_autoptr(GMenu) menu = g_menu_new();

  g_autoptr(GMenu) pages_section = g_menu_new();
  g_menu_append(pages_section, "Bookmarks", "win.show-bookmarks");
  g_menu_append(pages_section, "Downloads", "win.show-downloads");
  g_menu_append(pages_section, "History", "win.show-history");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(pages_section));

  g_autoptr(GMenu) page_section = g_menu_new();
  g_menu_append(page_section, "Install as Web Application", "win.install-as-web-app");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(page_section));

  g_autoptr(GMenu) application_section = g_menu_new();
  g_menu_append(application_section, "Settings", "win.show-settings");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(application_section));

  g_autoptr(GMenu) quit_section = g_menu_new();
  g_menu_append(quit_section, "Quit", "app.quit-browser");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(quit_section));

  GtkWidget *menu_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
  gtk_header_bar_pack_end(GTK_HEADER_BAR(win->header_bar), menu_button);

  gtk_header_bar_pack_end(GTK_HEADER_BAR(win->header_bar), wig_window_base_get_downloads_button(WIG_WINDOW_BASE(win)));

  WigApplication *app = wig_application_get();
  WigUpdateMonitor *update_monitor = wig_application_get_update_monitor(app);
  if (update_monitor) {
    win->update_button = gtk_button_new();
    gtk_widget_add_css_class(win->update_button, "suggested-action");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(win->header_bar), win->update_button);
    g_signal_connect_object(update_monitor, "notify::state", G_CALLBACK(wig_window_update_state_changed), win,
                            G_CONNECT_SWAPPED);
    wig_window_update_state_changed(win, NULL, update_monitor);
  }

  win->tab_list = wig_tab_list_new();
  gtk_widget_insert_action_group(GTK_WIDGET(win), "tabs", G_ACTION_GROUP(wig_tab_list_get_action_group(win->tab_list)));
  g_signal_connect_object(win->tab_list, "close-tab", G_CALLBACK(wig_window_tab_close), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "notify::active-tab", G_CALLBACK(wig_window_active_tab_changed), win,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(win->tab_list, "tab-added", G_CALLBACK(wig_window_tab_added), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "tab-removed", G_CALLBACK(wig_window_tab_removed), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "tab-moved", G_CALLBACK(wig_window_tab_moved), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "reload-tab", G_CALLBACK(wig_window_tab_reload), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "mute-tab", G_CALLBACK(wig_window_tab_mute), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "duplicate-tab", G_CALLBACK(wig_window_tab_duplicate), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "copy-link-tab", G_CALLBACK(wig_window_tab_copy_link), win, G_CONNECT_DEFAULT);
  g_signal_connect_object(win->tab_list, "detach-tab", G_CALLBACK(wig_window_tab_detach_requested), win,
                          G_CONNECT_DEFAULT);

  win->tab_switcher = wig_tab_switcher_new(GTK_WINDOW(win), win->tab_list);

  win->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content_box = win->content_box;

  g_signal_connect_object(win->tab_list, "create-tab", G_CALLBACK(wig_window_create_tab), win, G_CONNECT_SWAPPED);

  win->tab_stack = gtk_stack_new();
  gtk_widget_set_vexpand(win->tab_stack, TRUE);

  GtkWidget *view_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(view_box), win->tab_stack);
  gtk_box_append(GTK_BOX(view_box), wig_window_base_get_search_bar(WIG_WINDOW_BASE(win)));

  win->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  GtkWidget *paned = win->paned;
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_end_child(GTK_PANED(paned), view_box);
  gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_position(GTK_PANED(paned), 200);
  g_signal_connect(paned, "notify::position", G_CALLBACK(wig_window_sidebar_position_changed), win);
  gtk_box_append(GTK_BOX(content_box), paned);

  gtk_window_set_child(GTK_WINDOW(win), content_box);
  gtk_window_set_titlebar(GTK_WINDOW(win), win->header_bar);

  g_autoptr(WPEToplevel) toplevel = wpe_toplevel_gtk_new(WPE_DISPLAY_GTK(wig_application_get_display(app)), 0,
                                                         GTK_WINDOW(win));
  wig_window_base_set_toplevel(WIG_WINDOW_BASE(win), toplevel);

  g_settings_bind(wig_application_get_settings(app), "tab-layout", win, "tab-layout", G_SETTINGS_BIND_DEFAULT);
}

static void wig_window_dispose(GObject *object)
{
  WigWindow *win = WIG_WINDOW(object);
  g_clear_handle_id(&win->progress_timeout_id, g_source_remove);
  g_clear_pointer(&win->closing_tabs, g_ptr_array_unref);
  g_clear_pointer(&win->unshown_web_views, g_ptr_array_unref);

  g_clear_object(&win->tab_switcher);

  if (win->tab_list) {
    WigApplication *app = wig_application_get();

    /* Tabs going away with the application are not tabs the user closed. */
    if (!wig_application_is_quitting(app))
      wig_session_push_closed_window(wig_application_get_session(app), wig_window_capture_session(win));

    g_clear_object(&win->tab_list);
  }

  g_clear_pointer(&win->tab_view_context_menu, gtk_widget_unparent);
  g_clear_pointer(&win->entry_completion_popover, gtk_widget_unparent);
  g_clear_object(&win->bookmark_popover);
  g_clear_pointer(&win->link_bookmark_popover, gtk_widget_unparent);
  g_clear_pointer(&win->install_popover, gtk_widget_unparent);
  if (win->active_web_view_signals)
    g_signal_group_set_target(win->active_web_view_signals, NULL);
  if (win->active_tab_signals)
    g_signal_group_set_target(win->active_tab_signals, NULL);
  wig_window_clear_tab_loading_binding(win);
  wig_window_base_set_active_web_view(WIG_WINDOW_BASE(win), NULL);
  g_clear_object(&win->active_web_view_signals);
  g_clear_object(&win->active_tab_signals);
  g_clear_pointer(&win->web_view_signal_groups, g_hash_table_unref);
  G_OBJECT_CLASS(wig_window_parent_class)->dispose(object);
}

static void wig_window_finalize(GObject *object)
{
  WigWindow *win = WIG_WINDOW(object);
  g_clear_object(&win->context_menu_action_group);

  G_OBJECT_CLASS(wig_window_parent_class)->finalize(object);
}

static void wig_window_init(WigWindow *win)
{
  win->unshown_web_views = g_ptr_array_new_with_free_func(g_object_unref);
  win->active_web_view_signals = g_signal_group_new(WEBKIT_TYPE_WEB_VIEW);
  g_signal_group_connect_swapped(win->active_web_view_signals, "notify::uri", G_CALLBACK(wig_window_uri_changed), win);
  g_signal_group_connect_swapped(win->active_web_view_signals, "notify::estimated-load-progress",
                                 G_CALLBACK(wig_window_load_progress_changed), win);
  g_signal_group_connect_swapped(win->active_web_view_signals, "context-menu",
                                 G_CALLBACK(wig_window_web_view_context_menu), win);
  g_signal_group_connect_swapped(win->active_web_view_signals, "mouse-target-changed",
                                 G_CALLBACK(wig_window_on_mouse_target_changed), win);
  win->active_tab_signals = g_signal_group_new(WIG_TYPE_TAB);
  g_signal_group_connect_swapped(win->active_tab_signals, "notify::page-uri", G_CALLBACK(wig_window_update_url), win);
  g_signal_group_connect_swapped(win->active_tab_signals, "notify::title",
                                 G_CALLBACK(wig_window_update_bookmark_button), win);
  g_signal_connect(win, "notify::loading", G_CALLBACK(wig_window_loading_changed), NULL);

  win->web_view_signal_groups = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
}

static gboolean wig_window_close_request(GtkWindow *window)
{
  WigWindow *win = WIG_WINDOW(window);
  guint n_tabs = win->tab_list ? wig_tab_list_get_n_tabs(win->tab_list) : 0;
  if (n_tabs == 0)
    return GDK_EVENT_PROPAGATE;

  /* Every page is asked, and the last tab to agree takes the window with it. A
   * page that refuses keeps its tab, and so keeps the window. */
  g_clear_pointer(&win->closing_tabs, g_ptr_array_unref);
  win->closing_tabs = g_ptr_array_new_full(n_tabs, g_object_unref);

  g_autoptr(GPtrArray) tabs = g_ptr_array_sized_new(n_tabs);
  for (guint i = 0; i < n_tabs; i++) {
    WigTab *tab = wig_tab_list_get_nth(win->tab_list, i);
    g_ptr_array_add(tabs, tab);
    g_ptr_array_add(win->closing_tabs, g_object_ref(tab));
  }
  wig_tab_list_close_many(win->tab_list, tabs);

  return GDK_EVENT_STOP;
}

static void wig_window_class_init(WigWindowClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->constructed = wig_window_constructed;
  gobject_class->dispose = wig_window_dispose;
  gobject_class->finalize = wig_window_finalize;
  gobject_class->get_property = wig_window_get_property;
  gobject_class->set_property = wig_window_set_property;

#if HAVE_APPLICATION_MANIFEST_SUPPORT
  WigWindowBaseClass *base_class = WIG_WINDOW_BASE_CLASS(klass);
  base_class->application_manifest_changed = wig_window_application_manifest_changed;
#endif

  props[PROP_TAB_LAYOUT] = g_param_spec_enum("tab-layout", NULL, NULL, WIG_TYPE_TAB_LAYOUT, WIG_TAB_LAYOUT_HORIZONTAL,
                                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property(gobject_class, PROP_TAB_LAYOUT, props[PROP_TAB_LAYOUT]);

  GtkWindowClass *window_class = GTK_WINDOW_CLASS(klass);
  window_class->close_request = wig_window_close_request;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_install_action(widget_class, "tab.detach", "u", wig_window_detach_tab);
  gtk_widget_class_install_action(widget_class, "tab.move-to", "(uub)", wig_window_move_tab_to);
}

WigWindow *wig_window_new(WigApplication *application)
{
  return g_object_new(WIG_TYPE_WINDOW, "application", application, NULL);
}

WigTabList *wig_window_get_tab_list(WigWindow *win)
{
  return win->tab_list;
}

void wig_window_add_web_view(WigWindow *win, WebKitWebView *web_view)
{
  WigTab *tab = wig_window_add_tab_for_view(win, web_view);
  wig_tab_list_set_active(win->tab_list, tab);
}

void wig_window_add_web_view_in_background(WigWindow *win, WebKitWebView *web_view)
{
  WigTab *active = wig_tab_list_get_active(win->tab_list);
  guint index = active ? wig_tab_list_index_of(win->tab_list, active) + 1 : wig_tab_list_get_n_tabs(win->tab_list);

  wig_tab_list_insert(win->tab_list, web_view, index);
}

WigTab *wig_window_take_tab(WigWindow *win, WebKitWebView *web_view)
{
  g_return_val_if_fail(WIG_IS_WINDOW(win), NULL);
  g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);

  g_autoptr(WigTab) tab = wig_window_get_tab_for_web_view(win, web_view);
  if (!tab)
    return NULL;

  g_autoptr(WigTab) detached_tab = wig_tab_list_detach(win->tab_list, tab);
  if (!detached_tab)
    return NULL;

  if (wig_tab_list_get_n_tabs(win->tab_list) == 0)
    gtk_window_destroy(GTK_WINDOW(win));
  return g_steal_pointer(&detached_tab);
}
