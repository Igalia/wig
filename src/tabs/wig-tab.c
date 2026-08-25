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

#include "wig-tab.h"

#include "wig-auth-dialog.h"
#include "wig-blank-page.h"
#include "wig-bookmarks-page.h"
#include "wig-crash-page.h"
#include "wig-downloads-page.h"
#include "wig-error-page.h"
#include "wig-favicon.h"
#include "wig-history-page.h"
#include "wig-new-tab-page.h"
#include "wig-option-menu.h"
#include "wig-script-dialog.h"
#include "wig-settings-page.h"
#include "wig-tls-error-page.h"
#include "wig-unresponsive-dialog.h"
#include "wig-utils.h"
#include "wig-window.h"
#include "wpe-view-gtk.h"

#define WIG_TAB_UNRESPONSIVE_TIMEOUT_SECONDS 60

struct _WigTab {
  GObject parent;

  guint id;
  WebKitWebView *web_view;
  GtkWidget *view_overlay;
  GtkWidget *web_view_widget;
  GtkWidget *native_page;
  GtkWidget *option_menu;
  GtkWidget *unresponsive_dialog;
  guint unresponsive_timeout_id;
  gboolean unresponsive;
  GtkWidget *status_label;
  GIcon *icon;
  char *title;
  /* Kept so a discarded tab can still say what it is and be brought back once
   * its view is gone. */
  char *uri;
  WebKitWebViewSessionState *session_state;
  gboolean discarded;
  gboolean restoring_icon;
  gboolean has_committed;
  gboolean page_is_stand_in;
  gboolean pinned;
  gboolean closing;
  gboolean close_pending;
  gboolean loading;
  gboolean playing_audio;
  gboolean muted;
  gboolean selected;
  gboolean active;
  gboolean search_active;
  guint search_match_count;
  gboolean in_use;
  gint64 last_used;
  gint64 last_active;

  gboolean status_active;
  double cursor_x;
  double cursor_y;
  int status_label_w;
  int status_label_h;
};

typedef enum {
  PROP_ICON = 1,
  PROP_TITLE,
  PROP_PINNED,
  PROP_LOADING,
  PROP_SELECTED,
  PROP_ACTIVE,
  PROP_PLAYING_AUDIO,
  PROP_MUTED,
  PROP_PAGE_URI,
  PROP_DISCARDED,
} WigTabProps;

static GParamSpec *props[PROP_DISCARDED + 1];

enum {
  CAPTURE_CHANGED,
  WANTS_ATTENTION,
  WEB_VIEW_CHANGED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static const struct {
  const char *property;
  WebKitMediaCaptureState (*get_state)(WebKitWebView *web_view);
  void (*set_state)(WebKitWebView *web_view, WebKitMediaCaptureState state);
} capture_kinds[WIG_CAPTURE_DISPLAY + 1] = {
  [WIG_CAPTURE_CAMERA] = { "camera-capture-state", webkit_web_view_get_camera_capture_state,
                           webkit_web_view_set_camera_capture_state },
  [WIG_CAPTURE_MICROPHONE] = { "microphone-capture-state", webkit_web_view_get_microphone_capture_state,
                               webkit_web_view_set_microphone_capture_state },
  [WIG_CAPTURE_DISPLAY] = { "display-capture-state", webkit_web_view_get_display_capture_state,
                            webkit_web_view_set_display_capture_state },
};

static gboolean wig_tab_compute_in_use(WigTab *self)
{
  if (self->active)
    return TRUE;

  if (!self->web_view)
    return FALSE;

  if (self->playing_audio || self->loading)
    return TRUE;

  for (WigCaptureKind kind = WIG_CAPTURE_CAMERA; kind <= WIG_CAPTURE_DISPLAY; kind++) {
    if (capture_kinds[kind].get_state(self->web_view) != WEBKIT_MEDIA_CAPTURE_STATE_NONE)
      return TRUE;
  }

  return FALSE;
}

static void wig_tab_update_in_use(WigTab *self)
{
  gboolean in_use = wig_tab_compute_in_use(self);

  if (self->in_use == in_use)
    return;

  self->in_use = in_use;

  if (!in_use)
    self->last_used = g_get_monotonic_time();
}

static void wig_tab_set_discarded(WigTab *self, gboolean discarded)
{
  if (self->discarded == discarded)
    return;

  self->discarded = discarded;
  wig_tab_update_in_use(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DISCARDED]);
}

static void wig_tab_set_loading(WigTab *self, gboolean loading)
{
  if (loading && self->web_view && uri_is_blank_page(webkit_web_view_get_uri(self->web_view)))
    loading = FALSE;

  if (self->loading == loading)
    return;

  self->loading = loading;
  wig_tab_update_in_use(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_LOADING]);
}

static void wig_tab_on_capture_changed(WigTab *self)
{
  g_debug("tab %u: capture camera=%d microphone=%d display=%d", self->id,
          webkit_web_view_get_camera_capture_state(self->web_view),
          webkit_web_view_get_microphone_capture_state(self->web_view),
          webkit_web_view_get_display_capture_state(self->web_view));

  wig_tab_update_in_use(self);
  g_signal_emit(self, signals[CAPTURE_CHANGED], 0);
}

/* The question has been answered one way or the other, so a later attempt at
 * closing the tab starts a fresh one. */
static void wig_tab_close_question_answered(WigTab *self)
{
  self->close_pending = FALSE;
}

static gboolean wig_tab_on_script_dialog(WigTab *self, WebKitScriptDialog *dialog)
{
  WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(dialog);
  g_debug("tab %u: script dialog type %d", self->id, type);

  g_signal_emit(self, signals[WANTS_ATTENTION], 0);
  GtkWidget *shown = wig_script_dialog_show(GTK_OVERLAY(self->view_overlay), dialog);

  if (type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM)
    g_signal_connect_swapped(shown, "destroy", G_CALLBACK(wig_tab_close_question_answered), self);

  return TRUE;
}

static gboolean wig_tab_on_authenticate(WigTab *self, WebKitAuthenticationRequest *request)
{
  g_signal_emit(self, signals[WANTS_ATTENTION], 0);
  wig_auth_dialog_show(GTK_OVERLAY(self->view_overlay), request);
  return TRUE;
}

/* The dialog also goes away on its own, when it is answered or escaped, so the
 * tab follows the widget rather than assuming it outlives the request. */
static void wig_tab_unresponsive_dialog_destroyed(WigTab *self)
{
  self->unresponsive_dialog = NULL;
}

static void wig_tab_dismiss_unresponsive_dialog(WigTab *self)
{
  GtkWidget *dialog = g_steal_pointer(&self->unresponsive_dialog);
  if (!dialog)
    return;

  wig_unresponsive_dialog_dismiss(GTK_OVERLAY(self->view_overlay), dialog);
}

static void wig_tab_terminate_web_process(WigTab *self)
{
  g_warning("tab %u: ending the web process for an unresponsive %s", self->id, webkit_web_view_get_uri(self->web_view));

  webkit_web_view_terminate_web_process(self->web_view);
}

static void wig_tab_unresponsive_response(WigUnresponsiveResponse response, gpointer user_data)
{
  WigTab *self = user_data;

  wig_tab_dismiss_unresponsive_dialog(self);

  if (response == WIG_UNRESPONSIVE_RESPONSE_CLOSE)
    wig_tab_terminate_web_process(self);
}

/* A page stuck this long is not coming back, and until the process is gone the
 * tab cannot be reloaded or closed cleanly. */
static void wig_tab_unresponsive_timeout(gpointer user_data)
{
  WigTab *self = user_data;

  self->unresponsive_timeout_id = 0;

  g_warning("tab %u: unresponsive for %d seconds", self->id, WIG_TAB_UNRESPONSIVE_TIMEOUT_SECONDS);
  wig_tab_terminate_web_process(self);
}

static void wig_tab_set_unresponsive(WigTab *self, gboolean unresponsive)
{
  if (self->unresponsive == unresponsive)
    return;

  self->unresponsive = unresponsive;
  g_clear_handle_id(&self->unresponsive_timeout_id, g_source_remove);

  if (unresponsive) {
    self->unresponsive_timeout_id = g_timeout_add_seconds_once(WIG_TAB_UNRESPONSIVE_TIMEOUT_SECONDS,
                                                               wig_tab_unresponsive_timeout, self);
    return;
  }

  wig_tab_dismiss_unresponsive_dialog(self);
}

/* WebKit only notices a stuck process while it is waiting on one, so this
 * follows input rather than arriving on its own after a fixed delay. */
static void wig_tab_on_responsive_changed(WigTab *self)
{
  gboolean responsive = webkit_web_view_get_is_web_process_responsive(self->web_view);

  g_debug("tab %u: web process is %s", self->id, responsive ? "responsive again" : "not responding");

  wig_tab_set_unresponsive(self, !responsive);
}

/* Nothing is said about a stuck page until it is actually in the way: a page
 * quietly busy in the background is not worth interrupting anyone over. */
static void wig_tab_view_pressed(GtkGestureClick *gesture, int n_press, double x, double y, WigTab *self)
{
  if (!self->unresponsive || self->unresponsive_dialog)
    return;

  g_debug("tab %u: click on an unresponsive page, asking what to do", self->id);

  self->unresponsive_dialog = wig_unresponsive_dialog_show(
      GTK_OVERLAY(self->view_overlay), webkit_web_view_get_uri(self->web_view), wig_tab_unresponsive_response, self);
  g_signal_connect_object(self->unresponsive_dialog, "destroy", G_CALLBACK(wig_tab_unresponsive_dialog_destroyed), self,
                          G_CONNECT_SWAPPED);

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void wig_tab_option_menu_closed(WigTab *self)
{
  g_clear_pointer(&self->option_menu, gtk_widget_unparent);
}

static gboolean wig_tab_on_show_option_menu(WigTab *self, WebKitOptionMenu *menu, WebKitRectangle *rectangle)
{
  g_debug("tab %u: option menu with %u item(s) at %d,%d %dx%d", self->id, webkit_option_menu_get_n_items(menu),
          rectangle->x, rectangle->y, rectangle->width, rectangle->height);

  g_clear_pointer(&self->option_menu, gtk_widget_unparent);

  self->option_menu = wig_option_menu_show(self->web_view_widget, menu, rectangle);
  g_signal_connect_object(self->option_menu, "closed", G_CALLBACK(wig_tab_option_menu_closed), self, G_CONNECT_SWAPPED);

  return TRUE;
}

static void wig_tab_show_native_page(WigTab *self, GtkWidget *page);
static void wig_tab_clear_native_page(WigTab *self);

static void wig_tab_error_page_reload(WigTab *self)
{
  g_autofree char *uri = g_strdup(wig_tab_get_page_uri(self));

  wig_tab_clear_native_page(self);

  /* Nothing committed, so there is nothing for a reload to repeat. */
  if (uri)
    webkit_web_view_load_uri(self->web_view, uri);
  else
    webkit_web_view_reload(self->web_view);
}

static void wig_tab_error_page_go_back(WigTab *self)
{
  wig_tab_clear_native_page(self);
  webkit_web_view_go_back(self->web_view);
}

/* The exception lives in the network session, so it lasts until wig quits and
 * covers every later load of the same host with the same certificate. */
static void wig_tab_tls_error_page_proceed(WigTab *self)
{
  WigTlsErrorPage *page = WIG_TLS_ERROR_PAGE(self->native_page);
  g_autofree char *uri = g_strdup(wig_tls_error_page_get_uri(page));
  g_autofree char *host = g_strdup(wig_tls_error_page_get_host(page));
  g_autoptr(GTlsCertificate) certificate = g_object_ref(wig_tls_error_page_get_certificate(page));

  g_warning("tab %u: allowing untrusted certificate for %s", self->id, host);

  webkit_network_session_allow_tls_certificate_for_host(webkit_web_view_get_network_session(self->web_view),
                                                        certificate, host);

  wig_tab_clear_native_page(self);
  webkit_web_view_load_uri(self->web_view, uri);
}

static gboolean wig_tab_on_load_failed_with_tls_errors(WigTab *self, const char *failing_uri,
                                                       GTlsCertificate *certificate, GTlsCertificateFlags errors)
{
  g_warning("tab %u: TLS errors (0x%x) loading %s", self->id, errors, failing_uri);

  wig_tab_set_loading(self, FALSE);
  wig_tab_set_hovered_link(self, NULL, NULL);

  GtkWidget *page = wig_tls_error_page_new(failing_uri, certificate, errors,
                                           webkit_web_view_can_go_back(self->web_view));
  g_signal_connect_object(page, "proceed", G_CALLBACK(wig_tab_tls_error_page_proceed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(page, "go-back", G_CALLBACK(wig_tab_error_page_go_back), self, G_CONNECT_SWAPPED);

  wig_tab_show_native_page(self, page);

  return TRUE;
}

static gboolean network_is_offline(void)
{
  return !wig_network_monitor_get_available(wig_application_get_network_monitor(wig_application_get()));
}

static gboolean wig_tab_on_load_failed(WigTab *self, WebKitLoadEvent load_event, const char *failing_uri, GError *error)
{
  gboolean offline = network_is_offline();

  g_warning("tab %u: load failed for %s%s: %s", self->id, failing_uri, offline ? " while offline" : "", error->message);

  wig_tab_set_loading(self, FALSE);
  wig_tab_set_hovered_link(self, NULL, NULL);

  GtkWidget *page = wig_error_page_new(failing_uri, error, webkit_web_view_can_go_back(self->web_view), offline);
  g_signal_connect_object(page, "reload", G_CALLBACK(wig_tab_error_page_reload), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(page, "go-back", G_CALLBACK(wig_tab_error_page_go_back), self, G_CONNECT_SWAPPED);

  wig_tab_show_native_page(self, page);

  return TRUE;
}

static void wig_tab_network_came_online(WigTab *self)
{
  if (!WIG_IS_ERROR_PAGE(self->native_page))
    return;

  if (!wig_error_page_get_resumable(WIG_ERROR_PAGE(self->native_page)))
    return;

  g_debug("tab %u: network is back, reloading %s", self->id, wig_tab_get_page_uri(self));
  wig_tab_error_page_reload(self);
}

/* WebKit leaves the load state untouched when the web process dies, so a tab
 * that was mid-load keeps claiming it is loading until something reloads it. */
static void wig_tab_on_web_process_terminated(WigTab *self, WebKitWebProcessTerminationReason reason)
{
  g_warning("tab %u: web process terminated (reason %d) while showing %s", self->id, reason,
            webkit_web_view_get_uri(self->web_view));

  wig_tab_set_loading(self, FALSE);
  wig_tab_set_hovered_link(self, NULL, NULL);

  /* Nothing of the page it was showing survives the process, so the reload has
   * an unpainted view to fill again. */
  self->has_committed = FALSE;

  /* The dead process cannot report itself responsive again. */
  wig_tab_set_unresponsive(self, FALSE);
  wig_tab_dismiss_unresponsive_dialog(self);

  GtkWidget *page = wig_crash_page_new(self->web_view, reason, self->id);
  g_signal_connect_object(page, "reload", G_CALLBACK(wig_tab_error_page_reload), self, G_CONNECT_SWAPPED);

  /* The crashed view keeps the address of what it was showing, so the entry
   * needs no fallback of its own here. */
  wig_tab_show_native_page(self, page);
}

G_DEFINE_FINAL_TYPE(WigTab, wig_tab, G_TYPE_OBJECT)

static guint wig_tab_next_id = 1;

static void wig_tab_set_title(WigTab *self, const char *title)
{
  if (g_strcmp0(self->title, title) == 0)
    return;
  g_set_str(&self->title, title);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TITLE]);
}

static void wig_tab_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigTab *self = WIG_TAB(object);
  switch ((WigTabProps)prop_id) {
  case PROP_ICON:
    g_value_set_object(value, self->icon);
    break;
  case PROP_TITLE:
    g_value_set_string(value, self->title);
    break;
  case PROP_PINNED:
    g_value_set_boolean(value, self->pinned);
    break;
  case PROP_LOADING:
    g_value_set_boolean(value, self->loading);
    break;
  case PROP_SELECTED:
    g_value_set_boolean(value, self->selected);
    break;
  case PROP_ACTIVE:
    g_value_set_boolean(value, self->active);
    break;
  case PROP_PLAYING_AUDIO:
    g_value_set_boolean(value, self->playing_audio);
    break;
  case PROP_MUTED:
    g_value_set_boolean(value, self->muted);
    break;
  case PROP_DISCARDED:
    g_value_set_boolean(value, self->discarded);
    break;
  case PROP_PAGE_URI:
    g_value_set_string(value, wig_tab_get_page_uri(self));
    break;
  }
}

static void wig_tab_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigTab *self = WIG_TAB(object);
  switch ((WigTabProps)prop_id) {
  case PROP_ICON:
    wig_tab_set_icon(self, g_value_get_object(value));
    break;
  case PROP_TITLE:
    wig_tab_set_title(self, g_value_get_string(value));
    break;
  case PROP_PINNED:
    wig_tab_set_pinned(self, g_value_get_boolean(value));
    break;
  case PROP_LOADING:
    wig_tab_set_loading(self, g_value_get_boolean(value));
    break;
  case PROP_SELECTED:
    wig_tab_set_selected(self, g_value_get_boolean(value));
    break;
  case PROP_ACTIVE:
    wig_tab_set_active(self, g_value_get_boolean(value));
    break;
  case PROP_PLAYING_AUDIO: {
    gboolean playing_audio = g_value_get_boolean(value);
    if (self->playing_audio != playing_audio) {
      self->playing_audio = playing_audio;
      wig_tab_update_in_use(self);
      g_object_notify_by_pspec(object, props[PROP_PLAYING_AUDIO]);
    }
    break;
  }
  case PROP_MUTED: {
    gboolean muted = g_value_get_boolean(value);
    if (self->muted != muted) {
      self->muted = muted;
      g_object_notify_by_pspec(object, props[PROP_MUTED]);
    }
    break;
  }
  case PROP_PAGE_URI:
  case PROP_DISCARDED:
    break;
  }
}

static void wig_tab_dispose(GObject *object)
{
  WigTab *self = WIG_TAB(object);
  g_clear_object(&self->icon);
  g_clear_handle_id(&self->unresponsive_timeout_id, g_source_remove);
  g_clear_pointer(&self->option_menu, gtk_widget_unparent);
  if (self->web_view)
    g_signal_handlers_disconnect_by_data(self->web_view, self);
  g_clear_object(&self->web_view);
  g_clear_object(&self->view_overlay);
  self->web_view_widget = NULL;
  G_OBJECT_CLASS(wig_tab_parent_class)->dispose(object);
}

static void wig_tab_finalize(GObject *object)
{
  WigTab *self = WIG_TAB(object);
  g_free(self->title);
  g_free(self->uri);
  g_clear_pointer(&self->session_state, webkit_web_view_session_state_unref);
  G_OBJECT_CLASS(wig_tab_parent_class)->finalize(object);
}

static void wig_tab_init(WigTab *self)
{
  self->id = wig_tab_next_id++;
  self->title = g_strdup("New Tab");
  self->last_used = g_get_monotonic_time();
}

static void wig_tab_class_init(WigTabClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->get_property = wig_tab_get_property;
  gobject_class->set_property = wig_tab_set_property;
  gobject_class->dispose = wig_tab_dispose;
  gobject_class->finalize = wig_tab_finalize;

  props[PROP_ICON] = g_param_spec_object("icon", NULL, NULL, G_TYPE_ICON, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_TITLE] = g_param_spec_string("title", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_PINNED] = g_param_spec_boolean("pinned", NULL, NULL, FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_LOADING] = g_param_spec_boolean("loading", NULL, NULL, FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_SELECTED] = g_param_spec_boolean("selected", NULL, NULL, FALSE,
                                              G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_ACTIVE] = g_param_spec_boolean("active", NULL, NULL, FALSE,
                                            G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_PLAYING_AUDIO] = g_param_spec_boolean(
      "playing-audio", NULL, NULL, FALSE, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_MUTED] = g_param_spec_boolean("muted", NULL, NULL, FALSE,
                                           G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_PAGE_URI] = g_param_spec_string("page-uri", NULL, NULL, NULL,
                                             G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_DISCARDED] = g_param_spec_boolean("discarded", NULL, NULL, FALSE,
                                               G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(gobject_class, G_N_ELEMENTS(props), props);

  signals[CAPTURE_CHANGED] = g_signal_new("capture-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 0);

  /* A prompt is drawn over the page it belongs to, so a tab that puts one up
   * has to be the tab on screen for the question to be answerable at all. */
  signals[WANTS_ATTENTION] = g_signal_new("wants-attention", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 0);

  /* The view a tab shows is not for life: discarding one drops it and looking at
   * the tab again builds another, and whoever wired signals to it has to follow.
   * The old view, if there was one, comes with it. */
  signals[WEB_VIEW_CHANGED] = g_signal_new("web-view-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                           NULL, NULL, G_TYPE_NONE, 1, WEBKIT_TYPE_WEB_VIEW);
}

/* The title shown for a committed page that provides no <title> of its own. */
static char *wig_tab_uri_host(const char *uri)
{
  if (!uri || !*uri)
    return NULL;

  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  if (!parsed)
    return NULL;

  const char *host = g_uri_get_host(parsed);
  return (host && *host) ? g_strdup(host) : NULL;
}

/* WebKit clears the title to empty at the start of every load and only fills it
 * in once the page parses its <title>.  Ignore those transient empties so the
 * previous title stays visible while loading; the empty case is handled on
 * commit instead (see wig_tab_on_load_changed). */
static void wig_tab_on_title_changed(WigTab *self)
{
  const char *title = webkit_web_view_get_title(self->web_view);
  if (title && *title)
    wig_tab_set_title(self, title);
}

/* Some pages wig serves are built as widgets rather than documents. The view
 * still navigates to them, so everything hung off the address keeps working, but
 * what it drew is covered up and the widget carries the title the document would
 * have had. */
static void wig_tab_native_page_title_changed(WigTab *self, GParamSpec *pspec, WigNativePage *page)
{
  wig_tab_set_title(self, wig_native_page_get_title(page));
}

/* The page moves around inside itself by changing the address it is at, which
 * the view follows so the entry, history and session see an ordinary
 * navigation. */
static void wig_tab_native_page_uri_changed(WigTab *self, GParamSpec *pspec, WigNativePage *page)
{
  const char *uri = wig_native_page_get_uri(page);

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PAGE_URI]);

  if (!uri || g_strcmp0(uri, webkit_web_view_get_uri(self->web_view)) == 0)
    return;

  webkit_web_view_load_uri(self->web_view, uri);
}

static gboolean wig_tab_focus_is_in_input(WigTab *self)
{
  GtkRoot *root = gtk_widget_get_root(self->view_overlay);
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;

  return focus && GTK_IS_EDITABLE(focus);
}

static void wig_tab_show_native_page(WigTab *self, GtkWidget *page)
{
  /* One page at a time: a page left in the overlay would stay on screen. */
  wig_tab_clear_native_page(self);

  self->native_page = page;

  gtk_widget_set_visible(self->web_view_widget, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->view_overlay), page);

  /* Pages arrive on their own account as well as by being asked for: a load
   * failing, or a tab opening with nothing in it, happens while the user may be
   * partway through typing an address, and taking the focus there would throw
   * the typing away. */
  if (gtk_widget_get_focusable(page) && !wig_tab_focus_is_in_input(self))
    gtk_widget_grab_focus(page);

  g_signal_connect_object(page, "notify::title", G_CALLBACK(wig_tab_native_page_title_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(page, "notify::uri", G_CALLBACK(wig_tab_native_page_uri_changed), self, G_CONNECT_SWAPPED);

  /* A page that says nothing about what it is called leaves the tab the title
   * it already had, which for a failed load is the page it was leaving. */
  const char *title = wig_native_page_get_title(WIG_NATIVE_PAGE(page));
  if (title && *title)
    wig_tab_set_title(self, title);

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PAGE_URI]);
}

static void wig_tab_clear_native_page(WigTab *self)
{
  if (!self->native_page)
    return;

  gtk_overlay_remove_overlay(GTK_OVERLAY(self->view_overlay), self->native_page);
  self->native_page = NULL;
  self->page_is_stand_in = FALSE;
  gtk_widget_set_visible(self->web_view_widget, TRUE);

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PAGE_URI]);
}

static void wig_tab_show_stand_in_page(WigTab *self, const char *uri)
{
  g_debug("tab %u: nothing painted yet, standing in for %s with a blank page", self->id, uri ? uri : "(null)");

  self->native_page = wig_blank_page_new(uri);
  self->page_is_stand_in = TRUE;

  gtk_overlay_add_overlay(GTK_OVERLAY(self->view_overlay), self->native_page);

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PAGE_URI]);
}

static void wig_tab_on_buffer_rendered(WigTab *self)
{
  /* What wig draws over the view is what the tab is showing, so a frame painted
   * underneath one of those pages is not the view having something to show. The
   * page standing in until this very frame is the exception. */
  if (self->native_page && !self->page_is_stand_in)
    return;

  if (!self->has_committed || !self->page_is_stand_in)
    return;

  g_debug("tab %u: the site painted its first frame, taking the blank page down", self->id);
  wig_tab_clear_native_page(self);
}

/* A native page answers to every address of the page it is, panes included, so
 * moving between them is the page taking a new address rather than a new page. */
static gboolean wig_tab_native_page_answers_to(WigTab *self, const char *uri)
{
  if (!WIG_IS_NATIVE_PAGE(self->native_page))
    return FALSE;

  /* A page standing in for an unpainted view answers to no address of its own:
   * whatever the load turns out to be replaces it. */
  if (self->page_is_stand_in)
    return FALSE;

  return wig_util_uris_are_same_page(wig_native_page_get_uri(WIG_NATIVE_PAGE(self->native_page)), uri);
}

GtkWidget *wig_tab_get_native_page(WigTab *self)
{
  return self->native_page;
}

gboolean wig_tab_start_search(WigTab *self)
{
  if (!WIG_IS_NATIVE_PAGE(self->native_page))
    return FALSE;

  return wig_native_page_start_search(WIG_NATIVE_PAGE(self->native_page));
}

static void wig_tab_show_settings_page(WigTab *self, const char *uri)
{
  if (wig_tab_native_page_answers_to(self, uri))
    wig_native_page_set_uri(WIG_NATIVE_PAGE(self->native_page), uri);
  else
    wig_tab_show_native_page(self, wig_settings_page_new(uri));
}

/* Following an entry is an ordinary navigation, so the page it lands on takes
 * this one's place the way any other link would; middle-clicking it opens a tab
 * behind this one, the way middle-clicking a link on a page does. */
static void wig_tab_native_page_open_uri(WigTab *self, const char *uri, gboolean background)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));

  g_debug("tab %u: opening %s from a page of wig's own%s", self->id, uri,
          background ? " in a tab behind this one" : "");

  if (!background || !WIG_IS_WINDOW(root)) {
    webkit_web_view_load_uri(self->web_view, uri);
    return;
  }

  g_autoptr(WebKitWebView) web_view = wig_application_create_web_view(wig_application_get());
  wig_window_add_web_view_in_background(WIG_WINDOW(root), web_view);
  webkit_web_view_load_uri(web_view, uri);
}

static void wig_tab_show_history_page(WigTab *self, const char *uri)
{
  if (wig_tab_native_page_answers_to(self, uri)) {
    wig_native_page_set_uri(WIG_NATIVE_PAGE(self->native_page), uri);
    return;
  }

  GtkWidget *page = wig_history_page_new(uri);
  g_signal_connect_object(page, "open-uri", G_CALLBACK(wig_tab_native_page_open_uri), self, G_CONNECT_SWAPPED);
  wig_tab_show_native_page(self, page);
}

static void wig_tab_show_bookmarks_page(WigTab *self, const char *uri)
{
  if (wig_tab_native_page_answers_to(self, uri)) {
    wig_native_page_set_uri(WIG_NATIVE_PAGE(self->native_page), uri);
    return;
  }

  GtkWidget *page = wig_bookmarks_page_new(uri);
  g_signal_connect_object(page, "open-uri", G_CALLBACK(wig_tab_native_page_open_uri), self, G_CONNECT_SWAPPED);
  wig_tab_show_native_page(self, page);
}

static void wig_tab_show_new_tab_page(WigTab *self, const char *uri)
{
  GtkWidget *page = wig_new_tab_page_new(uri);
  g_signal_connect_object(page, "open-uri", G_CALLBACK(wig_tab_native_page_open_uri), self, G_CONNECT_SWAPPED);
  wig_tab_show_native_page(self, page);
}

/* Once a load commits, an empty title means the page genuinely has none, so fall
 * back to the hostname.  A real title, if any, arrives via notify::title. */
static void wig_tab_on_load_changed(WigTab *self, WebKitLoadEvent load_event)
{
  if (load_event == WEBKIT_LOAD_STARTED) {
    /* Anything that settled the state by hand (a stop, a crash, an error page)
     * left "is-loading" untouched, so a fresh load has to turn this back on
     * rather than wait for a change notification that will not come. */
    wig_tab_set_loading(self, TRUE);

    /* A restored tab is loading the very page its stored favicon belongs to, so
     * that icon stays until the page reports its own. */
    if (!self->restoring_icon)
      wig_tab_set_icon(self, NULL);
    self->restoring_icon = FALSE;
    wig_tab_set_hovered_link(self, NULL, NULL);

    /* Moving between panes would otherwise take the page down and put an empty
     * document on screen until the next one commits. */
    const char *started_uri = webkit_web_view_get_uri(self->web_view);
    if (!wig_tab_native_page_answers_to(self, started_uri))
      wig_tab_clear_native_page(self);

    if (!self->has_committed && !self->native_page && !uri_is_blank_page(started_uri))
      wig_tab_show_stand_in_page(self, started_uri);
  }

  if (load_event == WEBKIT_LOAD_FINISHED && self->page_is_stand_in && !self->has_committed)
    wig_tab_clear_native_page(self);

  if (load_event != WEBKIT_LOAD_COMMITTED)
    return;

  self->has_committed = TRUE;

  const char *uri = webkit_web_view_get_uri(self->web_view);
  if (uri_is_settings_page(uri)) {
    wig_tab_show_settings_page(self, uri);
    return;
  }

  if (uri_is_history_page(uri)) {
    wig_tab_show_history_page(self, uri);
    return;
  }

  if (uri_is_bookmarks_page(uri)) {
    wig_tab_show_bookmarks_page(self, uri);
    return;
  }

  if (uri_is_new_tab_page(uri)) {
    wig_tab_show_new_tab_page(self, uri);
    return;
  }

  if (uri_is_downloads_page(uri)) {
    if (wig_tab_native_page_answers_to(self, uri))
      wig_native_page_set_uri(WIG_NATIVE_PAGE(self->native_page), uri);
    else
      wig_tab_show_native_page(self, wig_downloads_page_new(uri));
    return;
  }

  /* Whatever is left of wig's own addresses commits an empty document, and so
   * does about:blank; both are shown as the nothing they are. A page standing in
   * for the frame this document will never usefully paint becomes the page the
   * tab keeps. */
  if (uri_is_blank_page(uri)) {
    if (!WIG_IS_BLANK_PAGE(self->native_page) || self->page_is_stand_in)
      wig_tab_show_native_page(self, wig_blank_page_new(uri));
    return;
  }

  const char *title = webkit_web_view_get_title(self->web_view);
  if (title && *title)
    return;

  g_autofree char *host = wig_tab_uri_host(webkit_web_view_get_uri(self->web_view));
  wig_tab_set_title(self, host ? host : "New Tab");
}

#if HAVE_FAVICON_SUPPORT
static void wig_tab_on_page_icons_changed(WigTab *self)
{
  // FIXME: Maybe we could have a custom GIconLoadable that is backed by the list.
  // on loading, which is passed a size, it then chooses the best one?
  // This allows for DPI changes working automatically?
  wig_tab_set_icon(self, wig_util_best_page_icon(webkit_web_view_get_page_icons(self->web_view), WIG_TAB_FAVICON_SIZE));
}

static void discarded_favicon_loaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigTab) self = WIG_TAB(user_data);
  g_autoptr(GError) error = NULL;
  GIcon *result_icon = wig_favicon_get_finish(WEBKIT_FAVICON_DATABASE(source), result, &error);
  g_autoptr(GObject) icon = result_icon ? G_OBJECT(result_icon) : NULL;
  if (!icon) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      g_debug("tab: failed to restore favicon: %s", error->message);
    return;
  }

  g_debug("tab %u: restored favicon arrived, discarded=%d has-icon=%d", self->id, self->discarded, self->icon != NULL);

  /* The lookup outlives the tab being loaded, so a favicon the page has already
   * reported for itself wins over the stored one. */
  if (!self->icon)
    wig_tab_set_icon(self, G_ICON(icon));
}

static void wig_tab_load_discarded_favicon(WigTab *self, const char *uri)
{
  if (!uri || !*uri)
    return;

  /* A tab restored from the session has no view to take the session from. */
  WebKitNetworkSession *session = self->web_view ? webkit_web_view_get_network_session(self->web_view)
                                                 : wig_application_get_network_session(wig_application_get());
  WebKitWebsiteDataManager *data_manager = webkit_network_session_get_website_data_manager(session);
  WebKitFaviconDatabase *database = webkit_website_data_manager_get_favicon_database(data_manager);
  if (!database) {
    g_debug("tab %u: no favicon database, cannot restore favicon for %s", self->id, uri);
    return;
  }

  g_debug("tab %u: requesting stored favicon for %s", self->id, uri);

  wig_favicon_get_async(database, uri, WIG_TAB_FAVICON_SIZE, NULL, discarded_favicon_loaded, g_object_ref(self));
}
#endif

static void wig_tab_update_label_position(WigTab *self, double cx, double cy);

static void wig_tab_overlay_motion(GtkEventControllerMotion *controller, double x, double y, WigTab *self)
{
  self->cursor_x = x;
  self->cursor_y = y;
  wig_tab_update_label_position(self, x, y);
}

/* The status label sits in the bottom-left corner of the overlay. Whenever the
 * cursor enters the bottom strip the label occupies, hide it completely until
 * the cursor leaves that region again. */
static void wig_tab_update_label_position(WigTab *self, double cx, double cy)
{
  GtkWidget *label = self->status_label;
  GtkWidget *overlay = self->view_overlay;

  if (!self->status_active)
    return;

  int overlay_w = gtk_widget_get_width(overlay);
  int overlay_h = gtk_widget_get_height(overlay);
  if (overlay_w == 0 || overlay_h == 0)
    return;

  int label_w = gtk_widget_get_width(label);
  int label_h = gtk_widget_get_height(label);
  if (label_w > 0 && label_h > 0) {
    self->status_label_w = label_w;
    self->status_label_h = label_h;
  } else if (self->status_label_w == 0 || self->status_label_h == 0) {
    int min_w, nat_w, min_h, nat_h;
    gtk_widget_measure(label, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, &nat_w, NULL, NULL);
    gtk_widget_measure(label, GTK_ORIENTATION_VERTICAL, nat_w, &min_h, &nat_h, NULL, NULL);
    self->status_label_w = MIN(nat_w, overlay_w);
    self->status_label_h = nat_h;
  }

  const int margin = 10;

  gboolean over_label = cy >= overlay_h - self->status_label_h - margin && cx <= self->status_label_w + margin;

  gtk_widget_set_visible(label, !over_label);
}

/* Everything hung off a particular view, so that a tab brought back from being
 * discarded is wired up the same as one that was never discarded. */
static void wig_tab_bind_web_view(WigTab *self, WebKitWebView *web_view)
{
  self->web_view = g_object_ref(web_view);
  self->web_view_widget = wpe_view_gtk_get_widget(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(web_view)));
  gtk_overlay_set_child(GTK_OVERLAY(self->view_overlay), self->web_view_widget);

  g_signal_connect_object(web_view, "notify::title", G_CALLBACK(wig_tab_on_title_changed), self, G_CONNECT_SWAPPED);
#if HAVE_FAVICON_SUPPORT
  g_signal_connect_object(web_view, "notify::page-icons", G_CALLBACK(wig_tab_on_page_icons_changed), self,
                          G_CONNECT_SWAPPED);
#endif
  g_signal_connect_object(web_view, "load-changed", G_CALLBACK(wig_tab_on_load_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "script-dialog", G_CALLBACK(wig_tab_on_script_dialog), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "authenticate", G_CALLBACK(wig_tab_on_authenticate), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "show-option-menu", G_CALLBACK(wig_tab_on_show_option_menu), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "notify::is-web-process-responsive", G_CALLBACK(wig_tab_on_responsive_changed),
                          self, G_CONNECT_SWAPPED);
  for (WigCaptureKind kind = WIG_CAPTURE_CAMERA; kind <= WIG_CAPTURE_DISPLAY; kind++) {
    g_autofree char *signal_name = g_strconcat("notify::", capture_kinds[kind].property, NULL);
    g_signal_connect_object(web_view, signal_name, G_CALLBACK(wig_tab_on_capture_changed), self, G_CONNECT_SWAPPED);
  }
  g_signal_connect_object(web_view, "web-process-terminated", G_CALLBACK(wig_tab_on_web_process_terminated), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "load-failed-with-tls-errors", G_CALLBACK(wig_tab_on_load_failed_with_tls_errors),
                          self, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "load-failed", G_CALLBACK(wig_tab_on_load_failed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(wig_application_get_network_monitor(wig_application_get()), "came-online",
                          G_CALLBACK(wig_tab_network_came_online), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(webkit_web_view_get_wpe_view(web_view), "buffer-rendered",
                          G_CALLBACK(wig_tab_on_buffer_rendered), self, G_CONNECT_SWAPPED);
  g_object_bind_property(G_OBJECT(web_view), "is-loading", self, "loading", G_BINDING_SYNC_CREATE);
  g_object_bind_property(G_OBJECT(web_view), "is-playing-audio", self, "playing-audio", G_BINDING_SYNC_CREATE);
  g_object_bind_property(G_OBJECT(web_view), "is-muted", self, "muted",
                         G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
}

/* Everything a tab has of its own, before any view is put in it. */
static WigTab *wig_tab_new_empty(void)
{
  WigTab *self = WIG_TAB(g_object_new(WIG_TYPE_TAB, NULL));

  /* The overlay outlives any one view: it stays in the window's stack while the
   * view inside it comes and goes. */
  self->view_overlay = g_object_ref_sink(gtk_overlay_new());

  self->status_label = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(self->status_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(self->status_label), 0.0f);
  gtk_widget_set_halign(self->status_label, GTK_ALIGN_START);
  gtk_widget_set_valign(self->status_label, GTK_ALIGN_END);
  gtk_widget_add_css_class(self->status_label, "link-status-bar");
  gtk_widget_set_visible(self->status_label, FALSE);
  gtk_widget_set_can_target(self->status_label, FALSE);

  gtk_overlay_add_overlay(GTK_OVERLAY(self->view_overlay), self->status_label);

  /* Capture phase, so a click lands here before the frozen view swallows it. */
  GtkGesture *click = gtk_gesture_click_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect_object(click, "pressed", G_CALLBACK(wig_tab_view_pressed), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(self->view_overlay, GTK_EVENT_CONTROLLER(click));

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect_object(motion, "motion", G_CALLBACK(wig_tab_overlay_motion), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(self->view_overlay, motion);

  return self;
}

WigTab *wig_tab_new(WebKitWebView *web_view)
{
  g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);

  WigTab *self = wig_tab_new_empty();
  wig_tab_bind_web_view(self, web_view);

  /* Pick up a title and icons the view may already have (e.g. a related view). */
  wig_tab_on_title_changed(self);
#if HAVE_FAVICON_SUPPORT
  wig_tab_on_page_icons_changed(self);
#endif

  return self;
}

/* A tab read back from a saved session, which starts out the same as one that
 * was unloaded by hand: no view, and only what it takes to show the tab and
 * build a view once it is looked at. */
WigTab *wig_tab_new_discarded(WebKitWebViewSessionState *state, const char *title, const char *uri)
{
  WigTab *self = wig_tab_new_empty();
  self->session_state = webkit_web_view_session_state_ref(state);
  self->uri = g_strdup(uri);
  wig_tab_set_discarded(self, TRUE);

  if (title && *title) {
    wig_tab_set_title(self, title);
  } else {
    g_autofree char *host = wig_tab_uri_host(uri);
    if (host)
      wig_tab_set_title(self, host);
  }

#if HAVE_FAVICON_SUPPORT
  self->restoring_icon = TRUE;
  wig_tab_load_discarded_favicon(self, uri);
#endif

  return self;
}

guint wig_tab_get_id(WigTab *self)
{
  return self->id;
}

WebKitWebView *wig_tab_get_web_view(WigTab *self)
{
  return self->web_view;
}

GtkWidget *wig_tab_get_widget(WigTab *self)
{
  return self->view_overlay;
}

GIcon *wig_tab_get_icon(WigTab *self)
{
  return self->icon;
}

void wig_tab_set_icon(WigTab *self, GIcon *icon)
{
  if (self->icon == icon)
    return;
  g_clear_object(&self->icon);
  self->icon = icon ? g_object_ref(icon) : NULL;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ICON]);
}

const char *wig_tab_get_title(WigTab *self)
{
  return self->title;
}

const char *wig_tab_get_uri(WigTab *self)
{
  if (!self->web_view)
    return self->uri;

  if (self->discarded) {
    WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(self->web_view);
    WebKitBackForwardListItem *item = webkit_back_forward_list_get_current_item(list);
    if (item)
      return webkit_back_forward_list_item_get_uri(item);
  }

  return webkit_web_view_get_uri(self->web_view);
}

/* Set while an error page stands in for a load that never committed, so the
 * entry can keep showing the address the user asked for. */
/* A load that fails before committing leaves the view without an address, so
 * what the page shown in its place stands for is the address the tab is at. */
const char *wig_tab_get_page_uri(WigTab *self)
{
  if (!WIG_IS_NATIVE_PAGE(self->native_page))
    return NULL;

  return wig_native_page_get_uri(WIG_NATIVE_PAGE(self->native_page));
}

gboolean wig_tab_get_discarded(WigTab *self)
{
  return self->discarded;
}

/* What a discarded tab would be built back from, which is the same thing the
 * session writes down for every tab.
 *
 * Returns: (transfer full) (nullable): the tab's session state. */
WebKitWebViewSessionState *wig_tab_get_session_state(WigTab *self)
{
  g_assert(WIG_IS_TAB(self));

  if (self->web_view)
    return webkit_web_view_get_session_state(self->web_view);

  return self->session_state ? webkit_web_view_session_state_ref(self->session_state) : NULL;
}

/* Give up the view and keep only what is needed to build an equivalent one: the
 * session state, and the title, icon and address the tab goes on showing while
 * it has nothing loaded. */
void wig_tab_discard(WigTab *self)
{
  g_assert(WIG_IS_TAB(self));

  if (self->discarded || !self->web_view)
    return;

  g_debug("tab %u: discarding (%s)", self->id, wig_tab_get_uri(self));

  g_set_str(&self->uri, wig_tab_get_uri(self));
  g_clear_pointer(&self->session_state, webkit_web_view_session_state_unref);
  self->session_state = webkit_web_view_get_session_state(self->web_view);

  /* A native page stands in front of the view and belongs to the load that is
   * being thrown away. */
  wig_tab_clear_native_page(self);
  wig_tab_dismiss_unresponsive_dialog(self);
  self->unresponsive = FALSE;
  g_clear_handle_id(&self->unresponsive_timeout_id, g_source_remove);

  g_autoptr(WebKitWebView) old_view = g_steal_pointer(&self->web_view);
  g_signal_handlers_disconnect_by_data(old_view, self);
  g_signal_handlers_disconnect_by_data(webkit_web_view_get_wpe_view(old_view), self);
  gtk_overlay_set_child(GTK_OVERLAY(self->view_overlay), NULL);
  self->web_view_widget = NULL;
  self->has_committed = FALSE;

  wig_tab_set_discarded(self, TRUE);
  wig_tab_set_loading(self, FALSE);

  g_signal_emit(self, signals[WEB_VIEW_CHANGED], 0, old_view);
}

void wig_tab_load_discarded(WigTab *self)
{
  g_assert(WIG_IS_TAB(self));

  if (!self->discarded)
    return;

  wig_tab_set_discarded(self, FALSE);

  /* A tab discarded by hand gave its view up, so looking at it again has to
   * build one and put the stored session back into it. */
  if (!self->web_view) {
    g_autoptr(WebKitWebView) web_view = wig_application_create_web_view(wig_application_get());
    if (self->session_state)
      webkit_web_view_restore_session_state(web_view, self->session_state);

    wig_tab_bind_web_view(self, web_view);
    g_signal_emit(self, signals[WEB_VIEW_CHANGED], 0, NULL);
  }

  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(self->web_view);
  WebKitBackForwardListItem *item = webkit_back_forward_list_get_current_item(list);
  if (!item) {
    /* Nothing was ever committed, so there is no history entry to go back to. */
    if (self->uri)
      webkit_web_view_load_uri(self->web_view, self->uri);
    return;
  }

  g_debug("tab: loading discarded tab %u (%s)", self->id, webkit_back_forward_list_item_get_uri(item));
  webkit_web_view_go_to_back_forward_list_item(self->web_view, item);
}

guint wig_tab_get_unused_seconds(WigTab *self)
{
  if (self->in_use)
    return 0;

  return (guint)((g_get_monotonic_time() - self->last_used) / G_USEC_PER_SEC);
}

gboolean wig_tab_get_pinned(WigTab *self)
{
  return self->pinned;
}

void wig_tab_set_pinned(WigTab *self, gboolean pinned)
{
  if (self->pinned == pinned)
    return;
  self->pinned = pinned;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PINNED]);
}

/* Set once the page has agreed to go away, so that the tab is torn down rather
 * than asked a second time. */
gboolean wig_tab_get_closing(WigTab *self)
{
  return self->closing;
}

void wig_tab_set_closing(WigTab *self, gboolean closing)
{
  self->closing = closing;
}

/* Asking a page that is still working out its answer to close again restarts
 * WebKit's try-close timeout, which closes the page out from under the question
 * the user is still looking at. */
gboolean wig_tab_get_close_pending(WigTab *self)
{
  return self->close_pending;
}

void wig_tab_set_close_pending(WigTab *self, gboolean pending)
{
  self->close_pending = pending;
}

gboolean wig_tab_get_loading(WigTab *self)
{
  return self->loading;
}

gboolean wig_tab_get_playing_audio(WigTab *self)
{
  return self->playing_audio;
}

gboolean wig_tab_get_muted(WigTab *self)
{
  return self->muted;
}

void wig_tab_set_muted(WigTab *self, gboolean muted)
{
  g_object_set(self, "muted", muted, NULL);
}

WebKitMediaCaptureState wig_tab_get_capture_state(WigTab *self, WigCaptureKind kind)
{
  g_assert(WIG_IS_TAB(self));

  if (!self->web_view)
    return WEBKIT_MEDIA_CAPTURE_STATE_NONE;

  return capture_kinds[kind].get_state(self->web_view);
}

/* Muting a device leaves it held and can be undone, while WEBKIT_MEDIA_CAPTURE_
 * STATE_NONE gives it up for good: the page has to ask for it again. */
void wig_tab_set_capture_state(WigTab *self, WigCaptureKind kind, WebKitMediaCaptureState state)
{
  g_assert(WIG_IS_TAB(self));

  if (!self->web_view)
    return;

  capture_kinds[kind].set_state(self->web_view, state);
}

gboolean wig_tab_get_selected(WigTab *self)
{
  return self->selected;
}

gboolean wig_tab_get_active(WigTab *self)
{
  return self->active;
}

gint64 wig_tab_get_last_active(WigTab *self)
{
  return self->last_active;
}

void wig_tab_set_active(WigTab *self, gboolean active)
{
  if (self->active == active)
    return;

  self->active = active;
  self->last_active = g_get_monotonic_time();
  wig_tab_update_in_use(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ACTIVE]);
}

gboolean wig_tab_get_search_active(WigTab *self)
{
  g_return_val_if_fail(WIG_IS_TAB(self), FALSE);

  return self->search_active;
}

void wig_tab_set_search_active(WigTab *self, gboolean search_active)
{
  g_return_if_fail(WIG_IS_TAB(self));

  self->search_active = search_active;
}

/* Counting matches in WebKit clears the marks the search put in the page, so the
 * last count is kept here rather than asked for again when the tab is shown. */
guint wig_tab_get_search_match_count(WigTab *self)
{
  g_return_val_if_fail(WIG_IS_TAB(self), 0);

  return self->search_match_count;
}

void wig_tab_set_search_match_count(WigTab *self, guint match_count)
{
  g_return_if_fail(WIG_IS_TAB(self));

  self->search_match_count = match_count;
}

void wig_tab_set_selected(WigTab *self, gboolean selected)
{
  if (self->selected == selected)
    return;
  self->selected = selected;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SELECTED]);
}

void wig_tab_set_hovered_link(WigTab *self, const char *uri, const char *page_origin)
{
  if (!uri || !*uri) {
    self->status_active = FALSE;
    gtk_widget_set_visible(self->status_label, FALSE);
    return;
  }

  g_autoptr(WebKitSecurityOrigin) link_origin_obj = webkit_security_origin_new_for_uri(uri);
  g_autofree char *link_origin = link_origin_obj ? webkit_security_origin_to_string(link_origin_obj) : NULL;

  /* A neat indicator that the link will take you to a different origin. */
  const char *color = NULL;
  gsize origin_len = 0;
  if (link_origin && *link_origin && g_str_has_prefix(uri, link_origin)) {
    gboolean same = (g_strcmp0(link_origin, page_origin) == 0);
    color = same ? "#96ffbb" : "#ffa2a6";
    origin_len = strlen(link_origin);
  }

  g_autofree char *markup = NULL;
  if (color && origin_len > 0) {
    g_autofree char *origin_escaped = g_markup_escape_text(uri, (gssize)origin_len);
    g_autofree char *rest_escaped = g_markup_escape_text(uri + origin_len, -1);
    markup = g_strdup_printf("<span color=\"%s\">%s</span>%s", color, origin_escaped, rest_escaped);
  } else {
    markup = g_markup_escape_text(uri, -1);
  }

  gtk_label_set_markup(GTK_LABEL(self->status_label), markup);
  self->status_active = TRUE;
  self->status_label_w = 0;
  self->status_label_h = 0;
  gtk_widget_set_visible(self->status_label, TRUE);
  wig_tab_update_label_position(self, self->cursor_x, self->cursor_y);
}
