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

#include "wig-window-base.h"

#include "wig-application.h"
#include "wig-context-menu.h"
#include "wig-downloads-button.h"
#include "wig-emoji-chooser.h"
#include "wig-favicon.h"
#include "wig-permissions-popover.h"
#include "wig-search-bar.h"
#include "wpe-view-gtk.h"

typedef struct {
  guint id;
  WPEToplevel *toplevel;
  WebKitWebView *active_web_view;
  GSignalGroup *active_web_view_signals;
  GSignalGroup *back_forward_list_signals;
  GHashTable *web_views;
  gboolean loading;
  GtkWidget *back_history_popover;
  GtkWidget *forward_history_popover;
  GtkWidget *permissions_button;
  GtkWidget *downloads_button;
  GtkWidget *permissions_popover;
  GtkWidget *permission_request_popover;
  GtkWidget *emoji_chooser;
  GtkWidget *search_bar;
  WigTab *active_tab;
  GSimpleActionGroup *context_menu_actions;
  WigPermissionsManager *permissions_manager; /* borrowed from application */
  char *active_origin;
} WigWindowBasePrivate;

typedef struct {
  GSignalGroup *signals;
#if HAVE_APPLICATION_MANIFEST_SUPPORT
  WebKitApplicationManifest *application_manifest;
  GCancellable *application_manifest_cancellable;
#endif
} WigWebViewState;

static void wig_web_view_state_free(WigWebViewState *state)
{
  g_clear_object(&state->signals);
#if HAVE_APPLICATION_MANIFEST_SUPPORT
  g_cancellable_cancel(state->application_manifest_cancellable);
  g_clear_object(&state->application_manifest_cancellable);
  g_clear_object(&state->application_manifest);
#endif
  g_free(state);
}

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(WigWindowBase, wig_window_base, GTK_TYPE_APPLICATION_WINDOW)

typedef enum {
  PROP_ID = 1,
  PROP_LOADING,
} WigWindowBaseProps;

static GParamSpec *props[PROP_LOADING + 1];

static void wig_window_base_update_loading_actions(WigWindowBase *self);
static guint next_window_id = 1;

static void wig_window_base_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(WIG_WINDOW_BASE(object));
  switch ((WigWindowBaseProps)prop_id) {
  case PROP_ID:
    g_value_set_uint(value, priv->id);
    break;
  case PROP_LOADING:
    g_value_set_boolean(value, priv->loading);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_window_base_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(WIG_WINDOW_BASE(object));
  switch ((WigWindowBaseProps)prop_id) {
  case PROP_ID:
    priv->id = g_value_get_uint(value);
    break;
  case PROP_LOADING:
    wig_window_base_set_loading(WIG_WINDOW_BASE(object), g_value_get_boolean(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static WebKitWebView *get_active_web_view(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->active_web_view;
}

static void notification_closed(WebKitNotification *notification, char *notification_id)
{
  WigApplication *application = wig_application_get();
  g_application_withdraw_notification(G_APPLICATION(application), notification_id);
  wig_application_untrack_notification(application, notification_id);
}

static void free_closure_data(gpointer data, GClosure *closure)
{
  g_free(data);
}

static gboolean show_notification(WebKitWebView *web_view, WebKitNotification *webkit_notification, WigWindowBase *self)
{
  WigApplication *application = wig_application_get();
  const char *title = webkit_notification_get_title(webkit_notification);
  g_autoptr(GNotification) notification = g_notification_new(title && *title ? title : "Notification");

  const char *body = webkit_notification_get_body(webkit_notification);
  if (body && *body)
    g_notification_set_body(notification, body);

  g_autofree char *notification_id = g_strdup_printf("wig-%" G_GUINT64_FORMAT,
                                                     webkit_notification_get_id(webkit_notification));
  g_notification_set_default_action_and_target(notification, "app.notification-clicked", "s", notification_id);

  wig_application_track_notification(application, notification_id, webkit_notification);
  g_application_send_notification(G_APPLICATION(application), notification_id, notification);

  g_signal_connect_data(webkit_notification, "closed", G_CALLBACK(notification_closed),
                        g_steal_pointer(&notification_id), free_closure_data, G_CONNECT_DEFAULT);
  return TRUE;
}

static char *web_view_origin(WebKitWebView *web_view)
{
  const char *uri = web_view ? webkit_web_view_get_uri(web_view) : NULL;
  if (!uri || !*uri)
    return NULL;

  g_autoptr(WebKitSecurityOrigin) origin = webkit_security_origin_new_for_uri(uri);
  return webkit_security_origin_to_string(origin);
}

/* The button stands for what the site is allowed to do, so it appears both for
 * an origin with answers on file and for one currently asking a question. */
static void update_permissions_button_visibility(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  gboolean has_permissions = FALSE;
  gboolean prompting = FALSE;

  g_object_get(priv->permissions_popover, "has-permissions", &has_permissions, NULL);
  g_object_get(priv->permission_request_popover, "prompting", &prompting, NULL);
  gtk_widget_set_visible(priv->permissions_button, has_permissions || prompting);
}

static void permission_request_prompting_changed(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  gboolean prompting = FALSE;

  g_object_get(priv->permission_request_popover, "prompting", &prompting, NULL);
  if (!prompting)
    return;

  gtk_menu_button_set_popover(GTK_MENU_BUTTON(priv->permissions_button), priv->permission_request_popover);
  update_permissions_button_visibility(self);
  gtk_menu_button_popup(GTK_MENU_BUTTON(priv->permissions_button));
}

static void permission_request_closed(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);

  gtk_menu_button_set_popover(GTK_MENU_BUTTON(priv->permissions_button), priv->permissions_popover);
  update_permissions_button_visibility(self);
}

static void update_permissions(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_clear_pointer(&priv->active_origin, g_free);
  priv->active_origin = web_view_origin(priv->active_web_view);

  WigPermissions *permissions = NULL;
  if (priv->active_origin)
    permissions = wig_permissions_manager_lookup(priv->permissions_manager, priv->active_origin);
  wig_permissions_popover_set_permissions(WIG_PERMISSIONS_POPOVER(priv->permissions_popover), permissions);
}

static void active_uri_changed(WigWindowBase *self, GParamSpec *pspec, WebKitWebView *web_view)
{
  update_permissions(self);
}

static void permissions_changed(WigWindowBase *self, const char *origin, WigPermissionsManager *manager)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (g_strcmp0(origin, priv->active_origin) != 0)
    return;

  WigPermissions *permissions = wig_permissions_manager_lookup(manager, origin);
  wig_permissions_popover_set_permissions(WIG_PERMISSIONS_POPOVER(priv->permissions_popover), permissions);
}

static gboolean permission_requested(WebKitWebView *web_view, WebKitPermissionRequest *request, WigWindowBase *self)
{
  g_debug("permission request %s received by window %u", G_OBJECT_TYPE_NAME(request), wig_window_base_get_id(self));

  /* The desktop portal has already asked the user what to share before WebKit
   * emits this request, and capture cannot start without the PipeWire fd. */
  if (wig_permission_request_is_display_capture(request)) {
    g_debug("allowing screen sharing request, already arranged by the portal");
    webkit_permission_request_allow(request);
    return TRUE;
  }

  if (wig_permission_kinds_for_request(request) == 0) {
    g_debug("permission request %s is not handled by wig", G_OBJECT_TYPE_NAME(request));
    return FALSE;
  }

  g_autofree char *origin = web_view_origin(web_view);
  if (!origin)
    return FALSE;

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  wig_permissions_manager_handle_request(priv->permissions_manager, origin, request,
                                         WIG_PERMISSION_REQUEST_POPOVER(priv->permission_request_popover));
  g_debug("permission request %s is awaiting a decision for %s", G_OBJECT_TYPE_NAME(request), origin);
  return TRUE;
}

static gboolean query_permission_state(WebKitWebView *web_view, WebKitPermissionStateQuery *query, WigWindowBase *self)
{
  const char *name = webkit_permission_state_query_get_name(query);

  WigPermissionKind kind = 0;
  if (g_str_equal(name, "geolocation"))
    kind = WIG_PERMISSION_GEOLOCATION;
  else if (g_str_equal(name, "notifications"))
    kind = WIG_PERMISSION_NOTIFICATION;
  else if (g_str_equal(name, "microphone"))
    kind = WIG_PERMISSION_MICROPHONE;
  else if (g_str_equal(name, "camera"))
    kind = WIG_PERMISSION_CAMERA;
  else if (g_str_equal(name, "clipboard-read") || g_str_equal(name, "clipboard-write"))
    kind = WIG_PERMISSION_CLIPBOARD;
  else if (g_str_equal(name, "xr-spatial-tracking"))
    kind = WIG_PERMISSION_XR;
  else
    return FALSE;

  WebKitSecurityOrigin *security_origin = webkit_permission_state_query_get_security_origin(query);
  g_autofree char *origin = webkit_security_origin_to_string(security_origin);
  if (!origin)
    return FALSE;

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  WigPermissions *permissions = wig_permissions_manager_lookup(priv->permissions_manager, origin);
  WebKitPermissionState state = permissions ? wig_permissions_get_state(permissions, kind)
                                            : WEBKIT_PERMISSION_STATE_PROMPT;

  g_debug("permission state query for %s at %s: %d", name, origin, state);

  webkit_permission_state_query_finish(query, state);
  return TRUE;
}

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static void application_manifest_refreshed(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  if (!wig_pwa_manager_install_finish(WIG_PWA_MANAGER(source), result, &error))
    g_warning("Failed to refresh installed web app: %s", error->message);
}

static void refresh_installed_web_app(WigWindowBase *self, WebKitApplicationManifest *manifest)
{
  const char *id = manifest ? webkit_application_manifest_get_id(manifest) : NULL;
  if (!id || !*id)
    return;

  WigPwaManager *manager = wig_application_get_pwa_manager(wig_application_get());
  g_autoptr(GError) error = NULL;
  if (wig_pwa_manager_is_installed(manager, id, &error)) {
    g_debug("pwa: refreshing installed web app %s from page manifest", id);
    wig_pwa_manager_refresh_async(manager, manifest, NULL, application_manifest_refreshed, NULL);
  } else if (error) {
    g_warning("Failed to check whether web app is installed: %s", error->message);
  }
}

static void set_application_manifest(WigWindowBase *self, WebKitWebView *web_view, WebKitApplicationManifest *manifest)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  WigWebViewState *state = g_hash_table_lookup(priv->web_views, web_view);
  if (!state || !g_set_object(&state->application_manifest, manifest))
    return;

  g_debug("pwa: application manifest for %s: %s", webkit_web_view_get_uri(web_view),
          manifest ? webkit_application_manifest_get_id(manifest) : "(none)");
  refresh_installed_web_app(self, manifest);

  WigWindowBaseClass *klass = WIG_WINDOW_BASE_GET_CLASS(self);
  if (klass->application_manifest_changed)
    klass->application_manifest_changed(self, web_view);
}

static void application_manifest_received(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WebKitApplicationManifest) manifest = webkit_web_view_get_application_manifest_finish(
      WEBKIT_WEB_VIEW(source), result, &error);
  /* The window may already be gone when the request was cancelled. */
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (error)
    g_warning("Failed to get application manifest: %s", error->message);

  set_application_manifest(WIG_WINDOW_BASE(user_data), WEBKIT_WEB_VIEW(source), manifest);
}

static void request_application_manifest(WigWindowBase *self, WebKitWebView *web_view, WigWebViewState *state)
{
  g_cancellable_cancel(state->application_manifest_cancellable);
  g_clear_object(&state->application_manifest_cancellable);
  state->application_manifest_cancellable = g_cancellable_new();
  webkit_web_view_get_application_manifest(web_view, state->application_manifest_cancellable,
                                           application_manifest_received, self);
}

static void web_view_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  WigWebViewState *state = g_hash_table_lookup(priv->web_views, web_view);
  if (!state)
    return;

  /* Clearing on commit rather than start keeps the manifest of a page whose provisional load fails. */
  if (load_event == WEBKIT_LOAD_COMMITTED) {
    g_cancellable_cancel(state->application_manifest_cancellable);
    set_application_manifest(self, web_view, NULL);
  } else if (load_event == WEBKIT_LOAD_FINISHED) {
    request_application_manifest(self, web_view, state);
  }
}

WebKitApplicationManifest *wig_window_base_get_application_manifest(WigWindowBase *self, WebKitWebView *web_view)
{
  g_return_val_if_fail(WIG_IS_WINDOW_BASE(self), NULL);
  g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  WigWebViewState *state = g_hash_table_lookup(priv->web_views, web_view);
  return state ? state->application_manifest : NULL;
}
#endif

static void wig_window_base_go_back(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_go_back(web_view);
}

static void wig_window_base_go_forward(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_go_forward(web_view);
}

static void wig_window_base_stop_reload(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindowBase *self = WIG_WINDOW_BASE(user_data);
  WebKitWebView *web_view = get_active_web_view(self);
  if (!web_view)
    return;

  g_autoptr(GVariant) state = g_action_get_state(G_ACTION(action));
  if (!g_variant_get_boolean(state)) {
    webkit_web_view_reload(web_view);
    return;
  }

  webkit_web_view_stop_loading(web_view);

  /* WebKit does not always report the load as over afterwards: a dead web
   * process, a wedged one, or a request that never reached the network each
   * leave "is-loading" set with no further notification to correct it.  The
   * state is settled here so that pressing stop is never a press that did
   * nothing. */
  wig_window_base_set_loading(self, FALSE);
}

static void wig_window_base_reload(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_reload(web_view);
}

static void wig_window_base_reload_bypass_cache(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_reload_bypass_cache(web_view);
}

static void wig_window_base_toggle_fullscreen(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  GtkWindow *window = GTK_WINDOW(user_data);
  if (gtk_window_is_fullscreen(window))
    gtk_window_unfullscreen(window);
  else
    gtk_window_fullscreen(window);
}

static void wig_window_base_zoom_in(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_set_zoom_level(web_view, webkit_web_view_get_zoom_level(web_view) + 0.1);
}

static void wig_window_base_zoom_out(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_set_zoom_level(web_view, webkit_web_view_get_zoom_level(web_view) - 0.1);
}

static void wig_window_base_zoom_reset(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_set_zoom_level(web_view, 1.0);
}

static void wig_window_base_emoji_chooser_closed(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_clear_pointer(&priv->emoji_chooser, gtk_widget_unparent);
}

static void wig_window_base_show_emoji_chooser(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigWindowBase) self = user_data;
  WebKitWebView *web_view = WEBKIT_WEB_VIEW(source);
  GdkRectangle caret;
  gboolean have_caret = wig_emoji_chooser_query_caret_finish(web_view, result, &caret);

  /* The answers are about the view that was asked, which the window may have
   * moved on from while the web process was answering. */
  if (get_active_web_view(self) != web_view)
    return;

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_clear_pointer(&priv->emoji_chooser, gtk_widget_unparent);

  priv->emoji_chooser = wig_emoji_chooser_show(web_view, have_caret ? &caret : NULL);
  g_signal_connect_object(priv->emoji_chooser, "closed", G_CALLBACK(wig_window_base_emoji_chooser_closed), self,
                          G_CONNECT_SWAPPED);
}

static void wig_window_base_emoji_target_checked(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigWindowBase) self = user_data;
  WebKitWebView *web_view = WEBKIT_WEB_VIEW(source);
  g_autoptr(GError) error = NULL;

  if (!webkit_web_view_can_execute_editing_command_finish(web_view, result, &error)) {
    g_debug("emoji: nothing editable is focused%s%s", error ? ": " : "", error ? error->message : "");
    return;
  }

  if (get_active_web_view(self) != web_view)
    return;

  wig_emoji_chooser_query_caret(web_view, wig_window_base_show_emoji_chooser, g_steal_pointer(&self));
}

static void wig_window_base_insert_emoji(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindowBase *self = WIG_WINDOW_BASE(user_data);
  WebKitWebView *web_view = get_active_web_view(self);
  if (!web_view)
    return;

  webkit_web_view_can_execute_editing_command(web_view, "InsertText", NULL, wig_window_base_emoji_target_checked,
                                              g_object_ref(self));
}

static void wig_window_base_find(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(WIG_WINDOW_BASE(user_data));

  wig_search_bar_open(WIG_SEARCH_BAR(priv->search_bar));
}

static void wig_window_base_find_next(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(WIG_WINDOW_BASE(user_data));
  if (wig_search_bar_is_open(WIG_SEARCH_BAR(priv->search_bar)))
    wig_search_bar_find_next(WIG_SEARCH_BAR(priv->search_bar));
}

static void wig_window_base_find_previous(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(WIG_WINDOW_BASE(user_data));
  if (wig_search_bar_is_open(WIG_SEARCH_BAR(priv->search_bar)))
    wig_search_bar_find_previous(WIG_SEARCH_BAR(priv->search_bar));
}

/* The find bar took the keyboard away from the page; hand it back. */
static void wig_window_base_search_bar_closed(WigSearchBar *search_bar, WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (priv->active_tab)
    gtk_widget_grab_focus(wig_tab_get_widget(priv->active_tab));
}

static void wig_window_base_toggle_inspector(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_toggle_inspector(web_view);
}

static void wig_window_base_show_downloads(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigApplication *app = wig_application_get();
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));

  wig_application_show_downloads(app, GTK_WIDGET(user_data),
                                 web_view ? wig_application_get_web_app_for_web_view(app, web_view) : NULL);
}

static void wig_window_base_show_history(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigApplication *app = wig_application_get();
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));

  wig_application_show_history(app, GTK_WIDGET(user_data),
                               web_view ? wig_application_get_web_app_for_web_view(app, web_view) : NULL);
}

static void wig_window_base_show_bookmarks(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_show_bookmarks(wig_application_get(), GTK_WIDGET(user_data));
}

static void wig_window_base_show_settings(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_application_show_settings(wig_application_get(), GTK_WIDGET(user_data));
}

static void wig_window_base_copy_text(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(user_data)), g_variant_get_string(parameter, NULL));
}

static void wig_window_base_copy_selection(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (web_view)
    webkit_web_view_execute_editing_command(web_view, WEBKIT_EDITING_COMMAND_COPY);
}

static const GActionEntry context_menu_actions[] = {
  { "copy-text", wig_window_base_copy_text, "s" },
  { "copy-selection", wig_window_base_copy_selection },
};

static void wig_window_base_on_mouse_target_changed(WigWindowBase *self, WebKitHitTestResult *hit_test_result,
                                                    guint modifiers, WebKitWebView *web_view)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (!priv->active_tab)
    return;

  const char *uri = NULL;
  if (webkit_hit_test_result_context_is_link(hit_test_result))
    uri = webkit_hit_test_result_get_link_uri(hit_test_result);

  wig_tab_set_hovered_link(priv->active_tab, uri, priv->active_origin);
}

static const GActionEntry actions[] = {
  { "go-back", wig_window_base_go_back },
  { "go-forward", wig_window_base_go_forward },
  { "stop-reload", wig_window_base_stop_reload, NULL, "false" },
  { "reload", wig_window_base_reload },
  { "reload-bypass-cache", wig_window_base_reload_bypass_cache },
  { "toggle-fullscreen", wig_window_base_toggle_fullscreen },
  { "zoom-in", wig_window_base_zoom_in },
  { "zoom-out", wig_window_base_zoom_out },
  { "zoom-reset", wig_window_base_zoom_reset },
  { "insert-emoji", wig_window_base_insert_emoji },
  { "find", wig_window_base_find },
  { "find-next", wig_window_base_find_next },
  { "find-previous", wig_window_base_find_previous },
  { "toggle-inspector", wig_window_base_toggle_inspector },
  { "show-downloads", wig_window_base_show_downloads },
  { "show-history", wig_window_base_show_history },
  { "show-bookmarks", wig_window_base_show_bookmarks },
  { "show-settings", wig_window_base_show_settings },
};

static void wig_window_base_update_navigation_actions(WigWindowBase *self)
{
  WebKitWebView *web_view = get_active_web_view(self);
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self), "go-back");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), web_view && webkit_web_view_can_go_back(web_view));
  action = g_action_map_lookup_action(G_ACTION_MAP(self), "go-forward");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), web_view && webkit_web_view_can_go_forward(web_view));
}

static void wig_window_base_update_loading_actions(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self), "stop-reload");
  g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(priv->loading));
}

static void wig_window_base_on_back_forward_list_changed(WigWindowBase *self, WebKitBackForwardListItem *item_added,
                                                         GList *items_removed, WebKitBackForwardList *list)
{
  wig_window_base_update_navigation_actions(self);
}

#if HAVE_FAVICON_SUPPORT
static void history_favicon_loaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GtkImage) image = GTK_IMAGE(user_data);
  g_autoptr(GError) error = NULL;
  GIcon *result_icon = wig_favicon_get_finish(WEBKIT_FAVICON_DATABASE(source), result, &error);
  g_autoptr(GObject) icon = result_icon ? G_OBJECT(result_icon) : NULL;
  if (icon)
    gtk_image_set_from_gicon(image, G_ICON(icon));
}
#endif

static GtkWidget *build_history_row(WebKitBackForwardListItem *item, WebKitWebView *web_view)
{
  const char *title = webkit_back_forward_list_item_get_title(item);
  const char *uri = webkit_back_forward_list_item_get_uri(item);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

#if HAVE_FAVICON_SUPPORT
  WebKitNetworkSession *session = webkit_web_view_get_network_session(web_view);
  WebKitWebsiteDataManager *data_manager = webkit_network_session_get_website_data_manager(session);
  WebKitFaviconDatabase *favicon_database = webkit_website_data_manager_get_favicon_database(data_manager);
  GtkWidget *image = gtk_image_new();
  gtk_image_set_icon_size(GTK_IMAGE(image), GTK_ICON_SIZE_NORMAL);
  gtk_box_append(GTK_BOX(box), image);
  if (favicon_database && uri)
    wig_favicon_get_async(favicon_database, uri, 16, NULL, history_favicon_loaded, g_object_ref(image));
#endif

  GtkWidget *label = gtk_label_new(title && *title ? title : uri);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);
  return box;
}

static GtkWidget *build_history_popover(WebKitWebView *web_view, WebKitBackForwardListItem *current, GList *items)
{
  GtkWidget *list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
  gtk_widget_set_size_request(list_box, 240, -1);

  gtk_list_box_append(GTK_LIST_BOX(list_box), build_history_row(current, web_view));
  GtkListBoxRow *current_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_box), 0);
  gtk_widget_add_css_class(GTK_WIDGET(current_row), "current-history-item");

  for (GList *l = items; l; l = l->next)
    gtk_list_box_append(GTK_LIST_BOX(list_box), build_history_row(WEBKIT_BACK_FORWARD_LIST_ITEM(l->data), web_view));

  GtkWidget *popover = gtk_popover_new();
  gtk_widget_add_css_class(popover, "back-history-popover");
  gtk_popover_set_child(GTK_POPOVER(popover), list_box);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  return popover;
}

static void back_history_row_activated(GtkListBox *list_box, GtkListBoxRow *row, WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  int index = gtk_list_box_row_get_index(row);
  g_clear_pointer(&priv->back_history_popover, gtk_widget_unparent);
  if (index == 0 || !priv->active_web_view)
    return;

  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(priv->active_web_view);
  WebKitBackForwardListItem *item = webkit_back_forward_list_get_nth_item(list, -index);
  if (item)
    webkit_web_view_go_to_back_forward_list_item(priv->active_web_view, item);
}

static void back_button_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (!priv->active_web_view)
    return;

  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(priv->active_web_view);
  g_autoptr(GList) items = webkit_back_forward_list_get_back_list(list);
  if (!items)
    return;

  g_clear_pointer(&priv->back_history_popover, gtk_widget_unparent);
  priv->back_history_popover = build_history_popover(priv->active_web_view,
                                                     webkit_back_forward_list_get_current_item(list), items);
  GtkWidget *list_box = gtk_popover_get_child(GTK_POPOVER(priv->back_history_popover));
  g_signal_connect_object(list_box, "row-activated", G_CALLBACK(back_history_row_activated), self, G_CONNECT_DEFAULT);
  gtk_widget_set_parent(priv->back_history_popover, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)));
  gtk_popover_popup(GTK_POPOVER(priv->back_history_popover));
}

static void forward_history_row_activated(GtkListBox *list_box, GtkListBoxRow *row, WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  int index = gtk_list_box_row_get_index(row);
  g_clear_pointer(&priv->forward_history_popover, gtk_widget_unparent);
  if (index == 0 || !priv->active_web_view)
    return;

  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(priv->active_web_view);
  WebKitBackForwardListItem *item = webkit_back_forward_list_get_nth_item(list, index);
  if (item)
    webkit_web_view_go_to_back_forward_list_item(priv->active_web_view, item);
}

static void forward_button_right_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (!priv->active_web_view)
    return;

  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(priv->active_web_view);
  g_autoptr(GList) items = webkit_back_forward_list_get_forward_list(list);
  if (!items)
    return;

  g_clear_pointer(&priv->forward_history_popover, gtk_widget_unparent);
  priv->forward_history_popover = build_history_popover(priv->active_web_view,
                                                        webkit_back_forward_list_get_current_item(list), items);
  GtkWidget *list_box = gtk_popover_get_child(GTK_POPOVER(priv->forward_history_popover));
  g_signal_connect_object(list_box, "row-activated", G_CALLBACK(forward_history_row_activated), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_set_parent(priv->forward_history_popover, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)));
  gtk_popover_popup(GTK_POPOVER(priv->forward_history_popover));
}

static gboolean wig_window_base_on_enter_fullscreen(WigWindowBase *self, WebKitWebView *web_view)
{
  gtk_window_fullscreen(GTK_WINDOW(self));
  return TRUE;
}

static gboolean wig_window_base_on_leave_fullscreen(WigWindowBase *self, WebKitWebView *web_view)
{
  gtk_window_unfullscreen(GTK_WINDOW(self));
  return TRUE;
}

static void wig_window_base_file_chooser_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WebKitFileChooserRequest) request = user_data;
  g_autoptr(GError) error = NULL;

  if (webkit_file_chooser_request_get_select_multiple(request)) {
    g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, &error);
    if (files) {
      guint n = g_list_model_get_n_items(files);
      g_auto(GStrv) paths = g_new0(char *, n + 1);
      gboolean valid = TRUE;
      for (guint i = 0; i < n; i++) {
        g_autoptr(GFile) file = g_list_model_get_item(files, i);
        paths[i] = g_file_get_path(file);
        if (!paths[i]) {
          valid = FALSE;
          break;
        }
      }
      if (valid)
        webkit_file_chooser_request_select_files(request, (const char *const *)paths);
      else {
        g_warning("file-chooser: selected file has no local path");
        webkit_file_chooser_request_cancel(request);
      }
    } else {
      g_debug("file-chooser failed: %s", error->message);
      webkit_file_chooser_request_cancel(request);
    }
  } else {
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (file) {
      g_autofree char *path = g_file_get_path(file);
      if (!path) {
        g_warning("file-chooser: selected file has no local path");
        webkit_file_chooser_request_cancel(request);
      } else {
        const char *paths[] = { path, NULL };
        webkit_file_chooser_request_select_files(request, paths);
      }
    } else {
      g_debug("file-chooser failed: %s", error->message);
      webkit_file_chooser_request_cancel(request);
    }
  }
}

static gboolean wig_window_base_on_run_file_chooser(WigWindowBase *self, WebKitFileChooserRequest *request,
                                                    WebKitWebView *web_view)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  const char *const *mime_types = webkit_file_chooser_request_get_mime_types(request);
  if (mime_types && *mime_types) {
    g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Supported Files");
    for (guint i = 0; mime_types[i]; i++)
      gtk_file_filter_add_mime_type(filter, mime_types[i]);

    g_autoptr(GtkFileFilter) all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Files");
    gtk_file_filter_add_pattern(all_filter, "*");

    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_list_store_append(filters, all_filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
  }

  if (webkit_file_chooser_request_get_select_multiple(request))
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(self), NULL, wig_window_base_file_chooser_done,
                                  g_object_ref(request));
  else
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, wig_window_base_file_chooser_done, g_object_ref(request));

  return TRUE;
}

#if HAVE_COLOR_CHOOSER_SUPPORT
static void wig_window_base_color_chooser_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WebKitColorChooserRequest) request = user_data;
  g_autoptr(GError) error = NULL;

  g_autoptr(GdkRGBA) rgba = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(source), result, &error);
  if (!rgba) {
    g_debug("color-chooser failed: %s", error->message);
    webkit_color_chooser_request_cancel(request);
    return;
  }

  WebKitColor color = {
    .red = rgba->red,
    .green = rgba->green,
    .blue = rgba->blue,
    .alpha = rgba->alpha,
  };
  webkit_color_chooser_request_set_color(request, &color);
  webkit_color_chooser_request_finish(request);
}

static gboolean wig_window_base_on_run_color_chooser(WigWindowBase *self, WebKitColorChooserRequest *request,
                                                     WebKitWebView *web_view)
{
  g_autoptr(GtkColorDialog) dialog = gtk_color_dialog_new();
  gtk_color_dialog_set_title(dialog, "Select Color");
#if HAVE_COLOR_CHOOSER_ALPHA_SUPPORT
  gtk_color_dialog_set_with_alpha(dialog, webkit_color_chooser_request_get_supports_alpha(request));
#else
  gtk_color_dialog_set_with_alpha(dialog, FALSE);
#endif

  WebKitColor color;
  webkit_color_chooser_request_get_color(request, &color);
  GdkRGBA rgba = {
    .red = (float)color.red,
    .green = (float)color.green,
    .blue = (float)color.blue,
    .alpha = (float)color.alpha,
  };

  gtk_color_dialog_choose_rgba(dialog, GTK_WINDOW(self), &rgba, NULL, wig_window_base_color_chooser_done,
                               g_object_ref(request));
  return TRUE;
}
#endif

static void wig_window_base_constructed(GObject *object)
{
  G_OBJECT_CLASS(wig_window_base_parent_class)->constructed(object);
  WigWindowBase *self = WIG_WINDOW_BASE(object);
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);

  if (priv->id == 0)
    priv->id = next_window_id++;
  else if (priv->id >= next_window_id)
    next_window_id = priv->id + 1;

  WigApplication *application = wig_application_get();
  priv->permissions_manager = wig_application_get_permissions_manager(application);
  g_signal_connect_object(priv->permissions_manager, "changed", G_CALLBACK(permissions_changed), self,
                          G_CONNECT_SWAPPED);

  g_action_map_add_action_entries(G_ACTION_MAP(object), actions, G_N_ELEMENTS(actions), object);
  wig_window_base_update_navigation_actions(self);
}

static void wig_window_base_dispose(GObject *object)
{
  WigWindowBase *self = WIG_WINDOW_BASE(object);
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);

  if (priv->active_web_view_signals)
    g_signal_group_set_target(priv->active_web_view_signals, NULL);
  if (priv->back_forward_list_signals)
    g_signal_group_set_target(priv->back_forward_list_signals, NULL);
  g_clear_object(&priv->active_web_view_signals);
  g_clear_object(&priv->back_forward_list_signals);
  g_clear_pointer(&priv->web_views, g_hash_table_unref);
  g_clear_object(&priv->active_web_view);
  g_clear_object(&priv->toplevel);
  g_clear_pointer(&priv->back_history_popover, gtk_widget_unparent);
  g_clear_pointer(&priv->forward_history_popover, gtk_widget_unparent);
  g_clear_pointer(&priv->emoji_chooser, gtk_widget_unparent);
  g_clear_object(&priv->permissions_popover);
  g_clear_object(&priv->permission_request_popover);
  g_clear_object(&priv->search_bar);
  g_clear_weak_pointer(&priv->active_tab);
  g_clear_object(&priv->context_menu_actions);
  g_clear_pointer(&priv->active_origin, g_free);

  G_OBJECT_CLASS(wig_window_base_parent_class)->dispose(object);
}

static void wig_window_base_class_init(WigWindowBaseClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->constructed = wig_window_base_constructed;
  object_class->dispose = wig_window_base_dispose;
  object_class->get_property = wig_window_base_get_property;
  object_class->set_property = wig_window_base_set_property;

  props[PROP_ID] = g_param_spec_uint("id", NULL, NULL, 0, G_MAXUINT, 0,
                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /* Bound to the active tab: a view that crashed or gave up on a load keeps
   * claiming it is loading, and only the tab knows an error page replaced it. */
  props[PROP_LOADING] = g_param_spec_boolean("loading", NULL, NULL, FALSE,
                                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, G_N_ELEMENTS(props), props);
}

static void wig_window_base_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                                          WigWindowBase *self)
{
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  /* Handle typical back/forward mouse buttons. */
  if (button == 8)
    g_action_group_activate_action(G_ACTION_GROUP(self), "go-back", NULL);
  else if (button == 9)
    g_action_group_activate_action(G_ACTION_GROUP(self), "go-forward", NULL);
}

static void wig_window_base_init(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);

  g_autoptr(GtkWindowGroup) group = gtk_window_group_new();
  gtk_window_group_add_window(group, GTK_WINDOW(self));

  GtkGestureClick *click = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect_object(click, "pressed", G_CALLBACK(wig_window_base_click_pressed), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));

  priv->permissions_popover = g_object_ref_sink(wig_permissions_popover_new());
  priv->permission_request_popover = g_object_ref_sink(wig_permission_request_popover_new());

  priv->permissions_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(priv->permissions_button), "sliders-horizontal-symbolic");
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(priv->permissions_button), priv->permissions_popover);

  priv->downloads_button = wig_downloads_button_new();

  priv->search_bar = g_object_ref_sink(wig_search_bar_new());
  g_signal_connect_object(priv->search_bar, "closed", G_CALLBACK(wig_window_base_search_bar_closed), self,
                          G_CONNECT_DEFAULT);

  priv->context_menu_actions = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(priv->context_menu_actions), context_menu_actions,
                                  G_N_ELEMENTS(context_menu_actions), self);
  gtk_widget_insert_action_group(GTK_WIDGET(self), "popup", G_ACTION_GROUP(priv->context_menu_actions));

  g_signal_connect_swapped(priv->permissions_popover, "notify::has-permissions",
                           G_CALLBACK(update_permissions_button_visibility), self);
  g_signal_connect_swapped(priv->permission_request_popover, "notify::prompting",
                           G_CALLBACK(permission_request_prompting_changed), self);
  g_signal_connect_swapped(priv->permission_request_popover, "closed", G_CALLBACK(permission_request_closed), self);
  update_permissions_button_visibility(self);

  priv->active_web_view_signals = g_signal_group_new(WEBKIT_TYPE_WEB_VIEW);
  g_signal_group_connect_swapped(priv->active_web_view_signals, "enter-fullscreen",
                                 G_CALLBACK(wig_window_base_on_enter_fullscreen), self);
  g_signal_group_connect_swapped(priv->active_web_view_signals, "leave-fullscreen",
                                 G_CALLBACK(wig_window_base_on_leave_fullscreen), self);
  g_signal_group_connect_swapped(priv->active_web_view_signals, "notify::uri", G_CALLBACK(active_uri_changed), self);
  g_signal_group_connect_swapped(priv->active_web_view_signals, "mouse-target-changed",
                                 G_CALLBACK(wig_window_base_on_mouse_target_changed), self);
  priv->back_forward_list_signals = g_signal_group_new(WEBKIT_TYPE_BACK_FORWARD_LIST);
  g_signal_group_connect_swapped(priv->back_forward_list_signals, "changed",
                                 G_CALLBACK(wig_window_base_on_back_forward_list_changed), self);
  priv->web_views = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)wig_web_view_state_free);
}

void wig_window_base_set_toplevel(WigWindowBase *self, WPEToplevel *toplevel)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_set_object(&priv->toplevel, toplevel);
}

guint wig_window_base_get_id(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->id;
}

WPEToplevel *wig_window_base_get_toplevel(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->toplevel;
}

void wig_window_base_set_navigation_buttons(WigWindowBase *self, GtkWidget *back_button, GtkWidget *forward_button)
{
  GtkGestureClick *back_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(back_gesture), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(back_gesture), GTK_PHASE_CAPTURE);
  g_signal_connect_object(back_gesture, "pressed", G_CALLBACK(back_button_right_pressed), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(back_button, GTK_EVENT_CONTROLLER(back_gesture));

  GtkGestureClick *forward_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(forward_gesture), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(forward_gesture), GTK_PHASE_CAPTURE);
  g_signal_connect_object(forward_gesture, "pressed", G_CALLBACK(forward_button_right_pressed), self,
                          G_CONNECT_DEFAULT);
  gtk_widget_add_controller(forward_button, GTK_EVENT_CONTROLLER(forward_gesture));
}

void wig_window_base_attach_web_view(WigWindowBase *self, WebKitWebView *web_view)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_assert(priv->toplevel != NULL);

  wpe_view_set_toplevel(webkit_web_view_get_wpe_view(web_view), priv->toplevel);
  if (g_hash_table_contains(priv->web_views, web_view))
    return;

  GSignalGroup *signals = g_signal_group_new(WEBKIT_TYPE_WEB_VIEW);
  g_signal_group_connect_swapped(signals, "run-file-chooser", G_CALLBACK(wig_window_base_on_run_file_chooser), self);
#if HAVE_COLOR_CHOOSER_SUPPORT
  g_signal_group_connect_swapped(signals, "run-color-chooser", G_CALLBACK(wig_window_base_on_run_color_chooser), self);
#endif
  g_signal_group_connect(signals, "show-notification", G_CALLBACK(show_notification), self);
  g_signal_group_connect(signals, "permission-request", G_CALLBACK(permission_requested), self);
  g_signal_group_connect(signals, "query-permission-state", G_CALLBACK(query_permission_state), self);
#if HAVE_APPLICATION_MANIFEST_SUPPORT
  g_signal_group_connect(signals, "load-changed", G_CALLBACK(web_view_load_changed), self);
#endif
  g_signal_group_set_target(signals, web_view);
  WigWebViewState *state = g_new0(WigWebViewState, 1);
  state->signals = signals;
  g_hash_table_insert(priv->web_views, web_view, state);
#if HAVE_APPLICATION_MANIFEST_SUPPORT
  if (!webkit_web_view_is_loading(web_view))
    request_application_manifest(self, web_view, state);
#endif
}

void wig_window_base_detach_web_view(WigWindowBase *self, WebKitWebView *web_view)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  WigWebViewState *state = g_hash_table_lookup(priv->web_views, web_view);
  if (state)
    g_signal_group_set_target(state->signals, NULL);
  g_hash_table_remove(priv->web_views, web_view);

  WPEView *view = webkit_web_view_get_wpe_view(web_view);
  if (wpe_view_get_toplevel(view) == priv->toplevel)
    wpe_view_set_toplevel(view, NULL);
}

void wig_window_base_set_active_web_view(WigWindowBase *self, WebKitWebView *web_view)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (priv->active_web_view == web_view)
    return;

  g_clear_pointer(&priv->back_history_popover, gtk_widget_unparent);
  g_clear_pointer(&priv->forward_history_popover, gtk_widget_unparent);
  g_clear_pointer(&priv->emoji_chooser, gtk_widget_unparent);
  g_signal_group_set_target(priv->active_web_view_signals, web_view);
  g_signal_group_set_target(priv->back_forward_list_signals,
                            web_view ? webkit_web_view_get_back_forward_list(web_view) : NULL);
  g_set_object(&priv->active_web_view, web_view);
  wig_window_base_update_navigation_actions(self);
  update_permissions(self);
}

WebKitWebView *wig_window_base_get_active_web_view(WigWindowBase *self)
{
  return get_active_web_view(self);
}

void wig_window_base_set_active_tab(WigWindowBase *self, WigTab *tab)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_set_weak_pointer(&priv->active_tab, tab);
  wig_search_bar_set_tab(WIG_SEARCH_BAR(priv->search_bar), tab);
}

GActionMap *wig_window_base_get_context_menu_actions(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return G_ACTION_MAP(priv->context_menu_actions);
}

GtkWidget *wig_window_base_get_search_bar(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->search_bar;
}

gboolean wig_window_base_get_loading(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->loading;
}

void wig_window_base_set_loading(WigWindowBase *self, gboolean loading)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (priv->loading == loading)
    return;

  priv->loading = loading;
  wig_window_base_update_loading_actions(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_LOADING]);
}

GtkWidget *wig_window_base_get_permissions_button(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->permissions_button;
}

GtkWidget *wig_window_base_get_downloads_button(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->downloads_button;
}

const char *wig_window_base_get_active_origin(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->active_origin;
}

gboolean wig_window_base_handle_download_response(WebKitPolicyDecision *decision,
                                                  WebKitPolicyDecisionType decision_type)
{
  if (decision_type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
    return FALSE;
  if (webkit_response_policy_decision_is_mime_type_supported(WEBKIT_RESPONSE_POLICY_DECISION(decision)))
    return FALSE;

  webkit_policy_decision_download(decision);
  return TRUE;
}

/* The hit test says a selection was clicked but not what is in it, and nothing in
 * the UI process keeps a copy, so the page is asked. */
static const char SELECTED_TEXT_SCRIPT[] = "(() => {"
                                           "  const active = document.activeElement;"
                                           "  if (active && typeof active.selectionStart === 'number')"
                                           "    return active.value.substring(active.selectionStart, "
                                           "active.selectionEnd);"
                                           "  return getSelection().toString();"
                                           "})()";

typedef struct {
  WigWindowBase *window;
  GMenu *menu;
  GMenu *search_section;
  GSimpleActionGroup *action_group;
  GdkRectangle target;
  gboolean has_position;
} ContextMenuRequest;

static void context_menu_request_free(ContextMenuRequest *request)
{
  g_clear_object(&request->window);
  g_clear_object(&request->menu);
  g_clear_object(&request->search_section);
  g_clear_object(&request->action_group);
  g_free(request);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ContextMenuRequest, context_menu_request_free)

static gboolean present_context_menu(WigWindowBase *self, GMenu *menu, GSimpleActionGroup *action_group,
                                     GdkRectangle *target)
{
  if (g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 0)
    return FALSE;

  WebKitWebView *web_view = get_active_web_view(self);
  wpe_view_gtk_show_context_menu(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(web_view)), G_MENU_MODEL(menu),
                                 G_ACTION_GROUP(action_group), target);
  return TRUE;
}

static void selected_text_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(ContextMenuRequest) request = user_data;
  WebKitWebView *web_view = WEBKIT_WEB_VIEW(source);
  g_autoptr(GError) error = NULL;
  g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(web_view, result, &error);
  if (!value)
    g_debug("context-menu: the page did not give up its selection%s%s", error ? ": " : "", error ? error->message : "");

  /* The menu is about the view that was asked, which the window may have moved
   * on from while the web process was answering. */
  if (get_active_web_view(request->window) != web_view)
    return;

  g_autofree char *selected_text = value && jsc_value_is_string(value) ? jsc_value_to_string(value) : NULL;
  g_debug("context-menu: selection is '%s'", selected_text ? selected_text : "");
  if (selected_text && *selected_text) {
    WigApplication *app = WIG_APPLICATION(gtk_window_get_application(GTK_WINDOW(request->window)));
    g_autofree char *search_engine = g_settings_get_string(wig_application_get_settings(app), "search-engine");
    wig_context_menu_add_search_item(request->search_section, selected_text, search_engine);
  }

  present_context_menu(request->window, request->menu, request->action_group,
                       request->has_position ? &request->target : NULL);
}

gboolean wig_window_base_show_context_menu(WigWindowBase *self, WebKitContextMenu *context_menu,
                                           WebKitHitTestResult *hit_test_result)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  WebKitWebView *web_view = get_active_web_view(self);
  if (!web_view)
    return FALSE;

  /* WebKit empties the menu the moment this handler returns, so everything it
   * proposed has to be taken now, before the page is asked for the selection. */
  g_autoptr(GSimpleActionGroup) action_group = g_simple_action_group_new();
  g_autoptr(GMenu) search_section = NULL;
  g_autoptr(GMenu) menu = wig_context_menu_build(context_menu, action_group, G_ACTION_GROUP(priv->context_menu_actions),
                                                 hit_test_result, &search_section);

  GdkRectangle target = { 0, 0, 1, 1 };
  gboolean has_position = webkit_context_menu_get_position(context_menu, &target.x, &target.y);

  if (webkit_hit_test_result_context_is_selection(hit_test_result)) {
    ContextMenuRequest *request = g_new0(ContextMenuRequest, 1);
    request->window = g_object_ref(self);
    request->menu = g_steal_pointer(&menu);
    request->search_section = g_steal_pointer(&search_section);
    request->action_group = g_steal_pointer(&action_group);
    request->target = target;
    request->has_position = has_position;
    webkit_web_view_evaluate_javascript(web_view, SELECTED_TEXT_SCRIPT, -1, "wig", NULL, NULL, selected_text_ready,
                                        request);
    return TRUE;
  }

  return present_context_menu(self, menu, action_group, has_position ? &target : NULL);
}
