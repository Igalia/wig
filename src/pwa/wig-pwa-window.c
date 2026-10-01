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

#include "wig-pwa-window.h"

#include "wig-application.h"
#include "wig-tab.h"
#include "wpe-display-gtk.h"
#include "wpe-toplevel-gtk.h"

#include <glib/gstdio.h>

typedef struct {
  GWeakRef image;
  char *directory;
  char *path;
} ShortcutIconDownload;

typedef struct {
  GtkImage *image;
  char *uri;
} PendingShortcutIcon;

struct _WigPwaWindow {
  WigWindowBase parent;

  char *id;
  WigPwaNavigationPolicy *navigation_policy;
  GtkWidget *header_bar;
  GtkWidget *stop_reload_button;
  GtkWidget *title_label;
  GtkWidget *location_label;
  GtkWidget *shortcuts_button;
  GtkWidget *shortcuts_popover;
  GPtrArray *pending_shortcut_icons;
  WigTab *tab;
  GSignalGroup *web_view_signals;
};

G_DEFINE_FINAL_TYPE(WigPwaWindow, wig_pwa_window, WIG_TYPE_WINDOW_BASE)

static void wig_pwa_window_close(WigPwaWindow *self)
{
  gtk_window_destroy(GTK_WINDOW(self));
}

static void wig_pwa_window_update_title(WigPwaWindow *self)
{
  WebKitWebView *web_view = wig_window_base_get_active_web_view(WIG_WINDOW_BASE(self));
  const char *title = web_view ? webkit_web_view_get_title(web_view) : NULL;
  gtk_window_set_title(GTK_WINDOW(self), title && *title ? title : "Web App");
  gtk_label_set_label(GTK_LABEL(self->title_label), title && *title ? title : "Web App");
}

static void wig_pwa_window_update_scope_chrome(WigPwaWindow *self)
{
  WebKitWebView *web_view = wig_window_base_get_active_web_view(WIG_WINDOW_BASE(self));
  const char *uri = web_view ? webkit_web_view_get_uri(web_view) : NULL;
  gtk_label_set_label(GTK_LABEL(self->location_label), uri ? uri : "");
  gtk_widget_set_tooltip_text(self->location_label, uri);
  gtk_widget_set_visible(self->location_label, TRUE);
}

static void wig_pwa_window_pwa_manager_changed(WigPwaWindow *self, const char *id)
{
  if (g_strcmp0(id, self->id) != 0)
    return;

  WigPwaManager *manager = wig_application_get_pwa_manager(wig_application_get());
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) navigation_policy = wig_pwa_manager_get_navigation_policy(manager, self->id,
                                                                                              &error);
  if (!navigation_policy) {
    g_warning("Failed to update web app navigation policy: %s", error->message);
    return;
  }
  g_set_object(&self->navigation_policy, navigation_policy);
  wig_pwa_window_update_scope_chrome(self);
}

static void external_uri_opened(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  if (!g_app_info_launch_default_for_uri_finish(result, &error)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
        || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      g_debug("pwa: no handler for external URI: %s", error->message);
    else
      g_warning("pwa: failed to open external URI: %s", error->message);
  }
}

static void open_external_uri(WigPwaWindow *self, const char *uri)
{
  const char *scheme = g_uri_peek_scheme(uri);
  if (scheme && (g_ascii_strcasecmp(scheme, "https") == 0 || g_ascii_strcasecmp(scheme, "http") == 0)) {
    wig_application_open_uri_in_browser(wig_application_get(), uri);
    return;
  }
  g_app_info_launch_default_for_uri_async(uri, NULL, NULL, external_uri_opened, NULL);
}

static gboolean wig_pwa_window_decide_policy(WigPwaWindow *self, WebKitPolicyDecision *decision,
                                             WebKitPolicyDecisionType decision_type)
{
  if (wig_window_base_handle_download_response(decision, decision_type))
    return TRUE;
  if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
    return FALSE;

  WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
      WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
  const char *scheme = uri ? g_uri_peek_scheme(uri) : NULL;
  if (!scheme || g_ascii_strcasecmp(scheme, "https") == 0 || g_ascii_strcasecmp(scheme, "http") == 0
      || g_ascii_strcasecmp(scheme, "about") == 0 || g_ascii_strcasecmp(scheme, "blob") == 0
      || g_ascii_strcasecmp(scheme, "data") == 0 || g_ascii_strcasecmp(scheme, "javascript") == 0) {
    g_debug("pwa: allowing navigation to %s (redirect=%d)", uri ? uri : "(null)",
            webkit_navigation_action_is_redirect(action));
    return FALSE;
  }

  webkit_policy_decision_ignore(decision);
  if (webkit_navigation_action_is_user_gesture(action)) {
    g_debug("pwa: opening external URI %s", uri);
    open_external_uri(self, uri);
  } else {
    g_debug("pwa: blocked external URI without user gesture: %s", uri);
  }
  return TRUE;
}

static WebKitWebView *wig_pwa_window_create_web_view(WigPwaWindow *self, WebKitNavigationAction *action)
{
  const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
  WigPwaScopeMatch match = wig_pwa_navigation_policy_match(self->navigation_policy, uri);
  if (webkit_navigation_action_is_user_gesture(action) && match != WIG_PWA_SCOPE_MATCH_OUTSIDE) {
    WebKitWebView *parent_view = wig_window_base_get_active_web_view(WIG_WINDOW_BASE(self));
    g_autoptr(WebKitWebView) web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", parent_view,
                                                                     "settings",
                                                                     webkit_web_view_get_settings(parent_view), NULL));
    GtkApplication *gtk_application = gtk_window_get_application(GTK_WINDOW(self));
    WigApplication *application = WIG_APPLICATION(gtk_application);
    WigPwaManager *manager = wig_application_get_pwa_manager(application);

    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) shortcuts = wig_pwa_manager_get_shortcuts(manager, self->id, &error);
    if (!shortcuts) {
      g_warning("pwa: failed to load shortcuts for popup: %s", error->message);
      return NULL;
    }

    g_autofree char *background_color = wig_pwa_manager_get_background_color(manager, self->id, &error);
    if (!background_color && error) {
      g_debug("pwa: failed to load popup background color: %s", error->message);
      g_clear_error(&error);
    }

    g_autoptr(WigTab) tab = wig_tab_new(web_view);
    WigPwaWindow *window = wig_pwa_window_new(gtk_application, wig_application_get_display(application), self->id,
                                              self->navigation_policy, shortcuts, background_color, tab);
    g_signal_connect_object(web_view, "ready-to-show", G_CALLBACK(gtk_window_present), window, G_CONNECT_SWAPPED);
    g_debug("pwa: created related window for in-scope request: %s", uri);
    return web_view;
  } else if (webkit_navigation_action_is_user_gesture(action) && uri) {
    open_external_uri(self, uri);
    g_debug("pwa: opened out-of-scope new-window request externally: %s", uri);
  } else {
    g_debug("pwa: blocked new-window request without user gesture: %s", uri ? uri : "(null)");
  }
  return NULL;
}

static void wig_pwa_window_close_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  gtk_window_destroy(GTK_WINDOW(user_data));
}

static void wig_pwa_window_open_shortcut(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigPwaWindow *self = WIG_PWA_WINDOW(user_data);
  WebKitWebView *web_view = wig_window_base_get_active_web_view(WIG_WINDOW_BASE(self));
  const char *uri = g_variant_get_string(parameter, NULL);
  if (self->shortcuts_popover)
    gtk_popover_popdown(GTK_POPOVER(self->shortcuts_popover));
  webkit_web_view_load_uri(web_view, uri);
  g_debug("pwa: opened shortcut %s", uri);
}

static void shortcut_icon_download_free(gpointer user_data, GClosure *closure)
{
  ShortcutIconDownload *data = user_data;
  if (data->path)
    g_unlink(data->path);
  if (data->directory)
    g_rmdir(data->directory);
  g_weak_ref_clear(&data->image);
  g_clear_pointer(&data->directory, g_free);
  g_clear_pointer(&data->path, g_free);
  g_free(data);
}

static void shortcut_icon_download_finished(WebKitDownload *download, ShortcutIconDownload *data)
{
  g_autoptr(GFile) file = g_file_new_for_path(data->path);
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkTexture) texture = gdk_texture_new_from_file(file, &error);
  if (!texture) {
    g_debug("pwa: failed to load shortcut icon: %s", error->message);
    return;
  }

  g_autoptr(GtkImage) image = g_weak_ref_get(&data->image);
  if (image)
    gtk_image_set_from_paintable(image, GDK_PAINTABLE(texture));
}

static void load_shortcut_icon(WebKitWebView *web_view, const char *uri, GtkImage *image)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *directory = g_dir_make_tmp("wig-pwa-shortcut-icon-XXXXXX", &error);
  if (!directory) {
    g_debug("pwa: failed to create shortcut icon directory: %s", error->message);
    return;
  }

  ShortcutIconDownload *data = g_new0(ShortcutIconDownload, 1);
  g_weak_ref_init(&data->image, image);
  data->directory = g_steal_pointer(&directory);
  data->path = g_build_filename(data->directory, "icon", NULL);

  WebKitNetworkSession *session = webkit_web_view_get_network_session(web_view);
  g_autoptr(WebKitDownload) download = webkit_network_session_download_uri(session, uri);
  g_signal_connect_data(download, "finished", G_CALLBACK(shortcut_icon_download_finished), data,
                        shortcut_icon_download_free, 0);
  webkit_download_set_destination(download, data->path);
}

static void pending_shortcut_icon_free(PendingShortcutIcon *icon)
{
  g_clear_object(&icon->image);
  g_clear_pointer(&icon->uri, g_free);
  g_free(icon);
}

static void wig_pwa_window_shortcuts_shown(WigPwaWindow *self)
{
  if (!self->pending_shortcut_icons || !self->tab)
    return;

  WebKitWebView *web_view = wig_tab_get_web_view(self->tab);
  for (guint i = 0; i < self->pending_shortcut_icons->len; i++) {
    PendingShortcutIcon *icon = g_ptr_array_index(self->pending_shortcut_icons, i);
    load_shortcut_icon(web_view, icon->uri, icon->image);
  }
  g_clear_pointer(&self->pending_shortcut_icons, g_ptr_array_unref);
}

static void wig_pwa_window_set_shortcuts(WigPwaWindow *self, GVariant *shortcuts)
{
  if (g_variant_n_children(shortcuts) == 0)
    return;

  self->pending_shortcut_icons = g_ptr_array_new_with_free_func((GDestroyNotify)pending_shortcut_icon_free);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(box, 220, -1);
  GVariantIter iterator;
  g_variant_iter_init(&iterator, shortcuts);
  while (TRUE) {
    g_autoptr(GVariant) shortcut = g_variant_iter_next_value(&iterator);
    if (!shortcut)
      break;

    const char *name = NULL;
    const char *uri = NULL;
    const char *icon_uri = NULL;
    g_variant_lookup(shortcut, "name", "&s", &name);
    g_variant_lookup(shortcut, "url", "&s", &uri);
    g_variant_lookup(shortcut, "icon_url", "&s", &icon_uri);

    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.open-shortcut");
    gtk_actionable_set_action_target_value(GTK_ACTIONABLE(button), g_variant_new_string(uri));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *image = gtk_image_new_from_icon_name("applications-internet-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
    gtk_box_append(GTK_BOX(row), image);
    GtkWidget *label = gtk_label_new(name);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(row), label);
    gtk_button_set_child(GTK_BUTTON(button), row);
    gtk_box_append(GTK_BOX(box), button);

    if (icon_uri && *icon_uri) {
      PendingShortcutIcon *icon = g_new0(PendingShortcutIcon, 1);
      icon->image = GTK_IMAGE(g_object_ref(image));
      icon->uri = g_strdup(icon_uri);
      g_ptr_array_add(self->pending_shortcut_icons, icon);
    }
  }

  self->shortcuts_popover = gtk_popover_new();
  g_signal_connect_object(self->shortcuts_popover, "show", G_CALLBACK(wig_pwa_window_shortcuts_shown), self,
                          G_CONNECT_SWAPPED);
  gtk_popover_set_child(GTK_POPOVER(self->shortcuts_popover), box);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->shortcuts_button), self->shortcuts_popover);
  gtk_widget_set_visible(self->shortcuts_button, TRUE);
}

static void wig_pwa_window_loading_changed(WigPwaWindow *self)
{
  gboolean is_loading = wig_window_base_get_loading(WIG_WINDOW_BASE(self));

  if (self->stop_reload_button)
    gtk_button_set_icon_name(GTK_BUTTON(self->stop_reload_button),
                             is_loading ? "process-stop-symbolic" : "view-refresh-symbolic");
}

static gboolean wig_pwa_window_context_menu(WigPwaWindow *self, WebKitContextMenu *context_menu,
                                            WebKitHitTestResult *hit_test_result)
{
  return wig_window_base_show_context_menu(WIG_WINDOW_BASE(self), context_menu, hit_test_result);
}

/* An app window has no tabs of its own, so what the menu calls a new tab is one
 * in a browser window. */
static void wig_pwa_window_open_in_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_open_uri_in_browser(wig_application_get(), g_variant_get_string(parameter, NULL));
}

static void wig_pwa_window_copy_text(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigPwaWindow *self = WIG_PWA_WINDOW(user_data);
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), g_variant_get_string(parameter, NULL));
}

static const GActionEntry context_menu_actions[] = {
  { "open-in-new-tab", wig_pwa_window_open_in_new_tab, "s" },
  { "copy-text", wig_pwa_window_copy_text, "s" },
};

static void wig_pwa_window_finalize(GObject *object)
{
  WigPwaWindow *self = WIG_PWA_WINDOW(object);
  g_clear_pointer(&self->id, g_free);
  G_OBJECT_CLASS(wig_pwa_window_parent_class)->finalize(object);
}

static void wig_pwa_window_dispose(GObject *object)
{
  WigPwaWindow *self = WIG_PWA_WINDOW(object);
  if (self->web_view_signals)
    g_signal_group_set_target(self->web_view_signals, NULL);
  g_clear_object(&self->web_view_signals);
  if (self->tab) {
    WebKitWebView *web_view = wig_tab_get_web_view(self->tab);
    wig_window_base_set_active_web_view(WIG_WINDOW_BASE(self), NULL);
    wig_window_base_detach_web_view(WIG_WINDOW_BASE(self), web_view);
    gtk_window_set_child(GTK_WINDOW(self), NULL);
  }
  g_clear_object(&self->tab);
  g_clear_object(&self->navigation_policy);
  g_clear_pointer(&self->pending_shortcut_icons, g_ptr_array_unref);
  G_OBJECT_CLASS(wig_pwa_window_parent_class)->dispose(object);
}

static void wig_pwa_window_class_init(WigPwaWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_pwa_window_dispose;
  object_class->finalize = wig_pwa_window_finalize;
}

static void wig_pwa_window_init(WigPwaWindow *self)
{
  self->web_view_signals = g_signal_group_new(WEBKIT_TYPE_WEB_VIEW);
  g_signal_group_connect_swapped(self->web_view_signals, "close", G_CALLBACK(wig_pwa_window_close), self);
  g_signal_group_connect_swapped(self->web_view_signals, "notify::title", G_CALLBACK(wig_pwa_window_update_title),
                                 self);
  g_signal_group_connect_swapped(self->web_view_signals, "notify::uri", G_CALLBACK(wig_pwa_window_update_scope_chrome),
                                 self);
  g_signal_group_connect_swapped(self->web_view_signals, "context-menu", G_CALLBACK(wig_pwa_window_context_menu), self);
  g_signal_group_connect_swapped(self->web_view_signals, "decide-policy", G_CALLBACK(wig_pwa_window_decide_policy),
                                 self);
  g_signal_group_connect_swapped(self->web_view_signals, "create", G_CALLBACK(wig_pwa_window_create_web_view), self);

  gtk_window_set_default_size(GTK_WINDOW(self), 1024, 720);
  gtk_window_set_title(GTK_WINDOW(self), "Web App");

  self->header_bar = gtk_header_bar_new();
  GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(title_box, GTK_ALIGN_CENTER);
  self->title_label = gtk_label_new("Web App");
  gtk_label_set_ellipsize(GTK_LABEL(self->title_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(self->title_label), 50);
  gtk_box_append(GTK_BOX(title_box), self->title_label);
  self->location_label = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(self->location_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(self->location_label), 50);
  gtk_widget_add_css_class(self->location_label, "caption");
  gtk_widget_add_css_class(self->location_label, "dim-label");
  gtk_widget_set_visible(self->location_label, FALSE);
  gtk_box_append(GTK_BOX(title_box), self->location_label);
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(self->header_bar), title_box);
  GtkWidget *back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_add_css_class(back_button, "flat");
  gtk_widget_set_tooltip_text(back_button, "Back");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(back_button), "win.go-back");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(self->header_bar), back_button);

  GtkWidget *forward_button = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_widget_add_css_class(forward_button, "flat");
  gtk_widget_set_tooltip_text(forward_button, "Forward");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(forward_button), "win.go-forward");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(self->header_bar), forward_button);
  wig_window_base_set_navigation_buttons(WIG_WINDOW_BASE(self), back_button, forward_button);

  self->stop_reload_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_add_css_class(self->stop_reload_button, "flat");
  gtk_widget_set_tooltip_text(self->stop_reload_button, "Stop or Reload");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(self->stop_reload_button), "win.stop-reload");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(self->header_bar), self->stop_reload_button);
  self->shortcuts_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(self->shortcuts_button), "view-list-bullet-symbolic");
  gtk_widget_add_css_class(self->shortcuts_button, "flat");
  gtk_widget_set_tooltip_text(self->shortcuts_button, "Shortcuts");
  gtk_widget_set_visible(self->shortcuts_button, FALSE);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(self->header_bar), self->shortcuts_button);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(self->header_bar),
                          wig_window_base_get_permissions_button(WIG_WINDOW_BASE(self)));
  gtk_window_set_titlebar(GTK_WINDOW(self), self->header_bar);
}

WigPwaWindow *wig_pwa_window_new(GtkApplication *application, WPEDisplay *display, const char *id,
                                 WigPwaNavigationPolicy *navigation_policy, GVariant *shortcuts,
                                 const char *background_color, WigTab *tab)
{
  g_return_val_if_fail(GTK_IS_APPLICATION(application), NULL);
  g_return_val_if_fail(WPE_IS_DISPLAY_GTK(display), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(WIG_IS_PWA_NAVIGATION_POLICY(navigation_policy), NULL);
  g_return_val_if_fail(g_variant_is_of_type(shortcuts, G_VARIANT_TYPE("aa{sv}")), NULL);
  g_return_val_if_fail(WIG_IS_TAB(tab), NULL);

  WebKitWebView *web_view = wig_tab_get_web_view(tab);

  WigPwaWindow *self = g_object_new(WIG_TYPE_PWA_WINDOW, "application", application, NULL);
  self->id = g_strdup(id);
  g_set_object(&self->navigation_policy, navigation_policy);
  g_set_object(&self->tab, tab);
  g_signal_connect(self, "notify::loading", G_CALLBACK(wig_pwa_window_loading_changed), NULL);
  g_object_bind_property(tab, "loading", self, "loading", G_BINDING_SYNC_CREATE);
  g_signal_connect_object(wig_application_get_pwa_manager(wig_application_get()), "changed",
                          G_CALLBACK(wig_pwa_window_pwa_manager_changed), self, G_CONNECT_SWAPPED);
  static const GActionEntry actions[] = {
    { "close-tab", wig_pwa_window_close_action },
    { "open-shortcut", wig_pwa_window_open_shortcut, "s" },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(self), actions, G_N_ELEMENTS(actions), self);
  g_autoptr(GSimpleActionGroup) context_menu_action_group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(context_menu_action_group), context_menu_actions,
                                  G_N_ELEMENTS(context_menu_actions), self);
  gtk_widget_insert_action_group(GTK_WIDGET(self), "popup", G_ACTION_GROUP(context_menu_action_group));
  g_autoptr(WPEToplevel) toplevel = wpe_toplevel_gtk_new(WPE_DISPLAY_GTK(display), 0, GTK_WINDOW(self));
  wig_window_base_set_toplevel(WIG_WINDOW_BASE(self), toplevel);
  gtk_window_set_child(GTK_WINDOW(self), wig_tab_get_widget(tab));
  wig_window_base_attach_web_view(WIG_WINDOW_BASE(self), web_view);
  if (background_color) {
    WebKitColor color;
    if (webkit_color_parse(&color, background_color)) {
      webkit_web_view_set_background_color(web_view, &color);
      g_debug("Applied PWA background color %s", background_color);
    } else {
      g_debug("Ignored invalid PWA background color %s", background_color);
    }
  }
  wig_window_base_set_active_web_view(WIG_WINDOW_BASE(self), web_view);
  wig_pwa_window_set_shortcuts(self, shortcuts);
  g_signal_group_set_target(self->web_view_signals, web_view);
  wig_pwa_window_update_title(self);
  wig_pwa_window_update_scope_chrome(self);
  return self;
}

const char *wig_pwa_window_get_id(WigPwaWindow *self)
{
  g_return_val_if_fail(WIG_IS_PWA_WINDOW(self), NULL);
  return self->id;
}

WebKitWebView *wig_pwa_window_get_web_view(WigPwaWindow *self)
{
  g_return_val_if_fail(WIG_IS_PWA_WINDOW(self), NULL);
  return wig_tab_get_web_view(self->tab);
}

WigTab *wig_pwa_window_take_tab(WigPwaWindow *self)
{
  g_assert(self->tab != NULL);

  g_signal_group_set_target(self->web_view_signals, NULL);
  wig_window_base_set_active_web_view(WIG_WINDOW_BASE(self), NULL);
  wig_window_base_detach_web_view(WIG_WINDOW_BASE(self), wig_tab_get_web_view(self->tab));
  gtk_window_set_child(GTK_WINDOW(self), NULL);
  return g_steal_pointer(&self->tab);
}

gboolean wig_pwa_window_is_current_uri_in_scope(WigPwaWindow *self)
{
  g_return_val_if_fail(WIG_IS_PWA_WINDOW(self), FALSE);

  WebKitBackForwardList *history = webkit_web_view_get_back_forward_list(wig_tab_get_web_view(self->tab));
  WebKitBackForwardListItem *current_item = webkit_back_forward_list_get_current_item(history);
  const char *uri = current_item ? webkit_back_forward_list_item_get_uri(current_item) : NULL;
  return uri && wig_pwa_navigation_policy_match(self->navigation_policy, uri) != WIG_PWA_SCOPE_MATCH_OUTSIDE;
}
