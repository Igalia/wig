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
#include "wig-downloads-button.h"
#include "wig-favicon.h"
#include "wig-permissions-popover.h"

typedef struct {
  guint id;
  WPEToplevel *toplevel;
  WebKitWebView *active_web_view;
  GSignalGroup *active_web_view_signals;
  GSignalGroup *back_forward_list_signals;
  GHashTable *web_view_signal_groups;
  GHashTable *crashed_web_views;
  GtkWidget *back_history_popover;
  GtkWidget *forward_history_popover;
  GtkWidget *permissions_button;
  GtkWidget *downloads_button;
  GtkWidget *permissions_popover;
  GtkWidget *permission_request_popover;
  WigPermissionsManager *permissions_manager; /* borrowed from application */
  char *active_origin;
} WigWindowBasePrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(WigWindowBase, wig_window_base, GTK_TYPE_APPLICATION_WINDOW)

typedef enum {
  PROP_ID = 1,
} WigWindowBaseProps;

static GParamSpec *props[PROP_ID + 1];
static guint next_window_id = 1;

static void wig_window_base_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(WIG_WINDOW_BASE(object));
  switch ((WigWindowBaseProps)prop_id) {
  case PROP_ID:
    g_value_set_uint(value, priv->id);
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
  else
    return FALSE;

  g_autoptr(WebKitSecurityOrigin) security_origin = webkit_permission_state_query_get_security_origin(query);
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
  WebKitWebView *web_view = get_active_web_view(WIG_WINDOW_BASE(user_data));
  if (!web_view)
    return;

  g_autoptr(GVariant) state = g_action_get_state(G_ACTION(action));
  if (g_variant_get_boolean(state))
    webkit_web_view_stop_loading(web_view);
  else
    webkit_web_view_reload(web_view);
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
};

static void wig_window_base_update_navigation_actions(WigWindowBase *self)
{
  WebKitWebView *web_view = get_active_web_view(self);
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self), "go-back");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), web_view && webkit_web_view_can_go_back(web_view));
  action = g_action_map_lookup_action(G_ACTION_MAP(self), "go-forward");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), web_view && webkit_web_view_can_go_forward(web_view));
}

/* A web view whose process died keeps reporting the load it never finished, so
 * the crashed ones are tracked here rather than trusting "is-loading". */
static void wig_window_base_update_loading_actions(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  WebKitWebView *web_view = get_active_web_view(self);
  gboolean crashed = web_view && priv->crashed_web_views && g_hash_table_contains(priv->crashed_web_views, web_view);
  gboolean is_loading = web_view && !crashed && webkit_web_view_is_loading(web_view);
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self), "stop-reload");
  g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(is_loading));

  WigWindowBaseClass *klass = WIG_WINDOW_BASE_GET_CLASS(self);
  if (klass->loading_changed)
    klass->loading_changed(self, is_loading);
}

static void wig_window_base_on_load_changed(WigWindowBase *self, WebKitLoadEvent load_event, WebKitWebView *web_view)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (load_event == WEBKIT_LOAD_STARTED)
    g_hash_table_remove(priv->crashed_web_views, web_view);

  wig_window_base_update_loading_actions(self);
}

static void wig_window_base_on_web_process_terminated(WigWindowBase *self, WebKitWebProcessTerminationReason reason,
                                                      WebKitWebView *web_view)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_hash_table_add(priv->crashed_web_views, web_view);

  wig_window_base_update_loading_actions(self);
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
  /* WPE has no API to report whether the element accepts alpha, and it never does today, so any
   * alpha the user picked would be silently discarded. */
  gtk_color_dialog_set_with_alpha(dialog, FALSE);

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
  g_clear_pointer(&priv->web_view_signal_groups, g_hash_table_unref);
  g_clear_pointer(&priv->crashed_web_views, g_hash_table_unref);
  g_clear_object(&priv->active_web_view);
  g_clear_object(&priv->toplevel);
  g_clear_pointer(&priv->back_history_popover, gtk_widget_unparent);
  g_clear_pointer(&priv->forward_history_popover, gtk_widget_unparent);
  g_clear_object(&priv->permissions_popover);
  g_clear_object(&priv->permission_request_popover);
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
  g_object_class_install_properties(object_class, G_N_ELEMENTS(props), props);
}

static void wig_window_base_init(WigWindowBase *self)
{
  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);

  priv->permissions_popover = g_object_ref_sink(wig_permissions_popover_new());
  priv->permission_request_popover = g_object_ref_sink(wig_permission_request_popover_new());

  priv->permissions_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(priv->permissions_button), "sliders-horizontal-symbolic");
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(priv->permissions_button), priv->permissions_popover);

  priv->downloads_button = wig_downloads_button_new();

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

  priv->back_forward_list_signals = g_signal_group_new(WEBKIT_TYPE_BACK_FORWARD_LIST);
  g_signal_group_connect_swapped(priv->back_forward_list_signals, "changed",
                                 G_CALLBACK(wig_window_base_on_back_forward_list_changed), self);
  priv->web_view_signal_groups = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
  priv->crashed_web_views = g_hash_table_new(g_direct_hash, g_direct_equal);
}

void wig_window_base_set_toplevel(WigWindowBase *self, WPEToplevel *toplevel)
{
  g_return_if_fail(WIG_IS_WINDOW_BASE(self));
  g_return_if_fail(WPE_IS_TOPLEVEL(toplevel));

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_set_object(&priv->toplevel, toplevel);
}

guint wig_window_base_get_id(WigWindowBase *self)
{
  g_return_val_if_fail(WIG_IS_WINDOW_BASE(self), 0);

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->id;
}

WPEToplevel *wig_window_base_get_toplevel(WigWindowBase *self)
{
  g_return_val_if_fail(WIG_IS_WINDOW_BASE(self), NULL);

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->toplevel;
}

void wig_window_base_set_navigation_buttons(WigWindowBase *self, GtkWidget *back_button, GtkWidget *forward_button)
{
  g_return_if_fail(WIG_IS_WINDOW_BASE(self));
  g_return_if_fail(GTK_IS_WIDGET(back_button));
  g_return_if_fail(GTK_IS_WIDGET(forward_button));

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
  g_return_if_fail(WIG_IS_WINDOW_BASE(self));
  g_return_if_fail(WEBKIT_IS_WEB_VIEW(web_view));

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  g_return_if_fail(WPE_IS_TOPLEVEL(priv->toplevel));

  wpe_view_set_toplevel(webkit_web_view_get_wpe_view(web_view), priv->toplevel);
  if (g_hash_table_contains(priv->web_view_signal_groups, web_view))
    return;

  GSignalGroup *signals = g_signal_group_new(WEBKIT_TYPE_WEB_VIEW);
  g_signal_group_connect_swapped(signals, "load-changed", G_CALLBACK(wig_window_base_on_load_changed), self);
  g_signal_group_connect_swapped(signals, "web-process-terminated",
                                 G_CALLBACK(wig_window_base_on_web_process_terminated), self);
  g_signal_group_connect_swapped(signals, "run-file-chooser", G_CALLBACK(wig_window_base_on_run_file_chooser), self);
#if HAVE_COLOR_CHOOSER_SUPPORT
  g_signal_group_connect_swapped(signals, "run-color-chooser", G_CALLBACK(wig_window_base_on_run_color_chooser), self);
#endif
  g_signal_group_connect(signals, "show-notification", G_CALLBACK(show_notification), self);
  g_signal_group_connect(signals, "permission-request", G_CALLBACK(permission_requested), self);
  g_signal_group_connect(signals, "query-permission-state", G_CALLBACK(query_permission_state), self);
  g_signal_group_set_target(signals, web_view);
  g_hash_table_insert(priv->web_view_signal_groups, web_view, signals);
}

void wig_window_base_detach_web_view(WigWindowBase *self, WebKitWebView *web_view)
{
  g_return_if_fail(WIG_IS_WINDOW_BASE(self));
  g_return_if_fail(WEBKIT_IS_WEB_VIEW(web_view));

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  GSignalGroup *signals = g_hash_table_lookup(priv->web_view_signal_groups, web_view);
  if (signals)
    g_signal_group_set_target(signals, NULL);
  g_hash_table_remove(priv->web_view_signal_groups, web_view);
  g_hash_table_remove(priv->crashed_web_views, web_view);
}

void wig_window_base_set_active_web_view(WigWindowBase *self, WebKitWebView *web_view)
{
  g_return_if_fail(WIG_IS_WINDOW_BASE(self));
  g_return_if_fail(web_view == NULL || WEBKIT_IS_WEB_VIEW(web_view));

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  if (priv->active_web_view == web_view)
    return;

  g_clear_pointer(&priv->back_history_popover, gtk_widget_unparent);
  g_clear_pointer(&priv->forward_history_popover, gtk_widget_unparent);
  g_signal_group_set_target(priv->active_web_view_signals, web_view);
  g_signal_group_set_target(priv->back_forward_list_signals,
                            web_view ? webkit_web_view_get_back_forward_list(web_view) : NULL);
  g_set_object(&priv->active_web_view, web_view);
  wig_window_base_update_navigation_actions(self);
  wig_window_base_update_loading_actions(self);
  update_permissions(self);
}

WebKitWebView *wig_window_base_get_active_web_view(WigWindowBase *self)
{
  g_return_val_if_fail(WIG_IS_WINDOW_BASE(self), NULL);
  return get_active_web_view(self);
}

GtkWidget *wig_window_base_get_permissions_button(WigWindowBase *self)
{
  g_return_val_if_fail(WIG_IS_WINDOW_BASE(self), NULL);

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->permissions_button;
}

GtkWidget *wig_window_base_get_downloads_button(WigWindowBase *self)
{
  g_return_val_if_fail(WIG_IS_WINDOW_BASE(self), NULL);

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->downloads_button;
}

const char *wig_window_base_get_active_origin(WigWindowBase *self)
{
  g_return_val_if_fail(WIG_IS_WINDOW_BASE(self), NULL);

  WigWindowBasePrivate *priv = wig_window_base_get_instance_private(self);
  return priv->active_origin;
}
