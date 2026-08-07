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
#include "wig-script-dialog.h"
#include "wig-utils.h"
#include "wpe-view-gtk.h"

struct _WigTab {
  GObject parent;

  guint id;
  WebKitWebView *web_view;
  GtkWidget *view_overlay;
  GtkWidget *status_label;
  GIcon *icon;
  char *title;
  gboolean discarded;
  gboolean pinned;
  gboolean loading;
  gboolean selected;
  gboolean search_active;
  guint search_match_count;

  gboolean status_active;
  double cursor_x;
  double cursor_y;
  int status_label_w;
  int status_label_h;
};

static gboolean wig_tab_on_script_dialog(WigTab *self, WebKitScriptDialog *dialog)
{
  wig_script_dialog_show(GTK_OVERLAY(self->view_overlay), dialog);
  return TRUE;
}

static gboolean wig_tab_on_authenticate(WigTab *self, WebKitAuthenticationRequest *request)
{
  wig_auth_dialog_show(GTK_OVERLAY(self->view_overlay), request);
  return TRUE;
}

G_DEFINE_FINAL_TYPE(WigTab, wig_tab, G_TYPE_OBJECT)

static guint wig_tab_next_id = 1;

typedef enum {
  PROP_ICON = 1,
  PROP_TITLE,
  PROP_PINNED,
  PROP_LOADING,
  PROP_SELECTED,
} WigTabProps;

static GParamSpec *props[PROP_SELECTED + 1];

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
  case PROP_LOADING: {
    gboolean loading = g_value_get_boolean(value);
    if (self->loading != loading) {
      self->loading = loading;
      g_object_notify_by_pspec(object, props[PROP_LOADING]);
    }
    break;
  }
  case PROP_SELECTED:
    wig_tab_set_selected(self, g_value_get_boolean(value));
    break;
  }
}

static void wig_tab_dispose(GObject *object)
{
  WigTab *self = WIG_TAB(object);
  g_clear_object(&self->icon);
  if (self->web_view)
    g_signal_handlers_disconnect_by_data(self->web_view, self);
  g_clear_object(&self->web_view);
  g_clear_object(&self->view_overlay);
  G_OBJECT_CLASS(wig_tab_parent_class)->dispose(object);
}

static void wig_tab_finalize(GObject *object)
{
  WigTab *self = WIG_TAB(object);
  g_free(self->title);
  G_OBJECT_CLASS(wig_tab_parent_class)->finalize(object);
}

static void wig_tab_init(WigTab *self)
{
  self->id = wig_tab_next_id++;
  self->title = g_strdup("New Tab");
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

  g_object_class_install_properties(gobject_class, G_N_ELEMENTS(props), props);
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

/* Once a load commits, an empty title means the page genuinely has none, so fall
 * back to the hostname.  A real title, if any, arrives via notify::title. */
static void wig_tab_on_load_changed(WigTab *self, WebKitLoadEvent load_event)
{
  if (load_event == WEBKIT_LOAD_STARTED) {
    wig_tab_set_icon(self, NULL);
    wig_tab_set_hovered_link(self, NULL, NULL);
  }

  if (load_event != WEBKIT_LOAD_COMMITTED)
    return;

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

WigTab *wig_tab_new(WebKitWebView *web_view)
{
  g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);

  WigTab *self = WIG_TAB(g_object_new(WIG_TYPE_TAB, NULL));
  self->web_view = g_object_ref(web_view);

  GtkWidget *web_view_widget = wpe_view_gtk_get_widget(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(web_view)));
  self->view_overlay = g_object_ref_sink(gtk_overlay_new());
  gtk_overlay_set_child(GTK_OVERLAY(self->view_overlay), web_view_widget);

  self->status_label = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(self->status_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(self->status_label), 0.0f);
  gtk_widget_set_halign(self->status_label, GTK_ALIGN_START);
  gtk_widget_set_valign(self->status_label, GTK_ALIGN_END);
  gtk_widget_add_css_class(self->status_label, "link-status-bar");
  gtk_widget_set_visible(self->status_label, FALSE);
  gtk_widget_set_can_target(self->status_label, FALSE);

  gtk_overlay_add_overlay(GTK_OVERLAY(self->view_overlay), self->status_label);

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect_object(motion, "motion", G_CALLBACK(wig_tab_overlay_motion), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(self->view_overlay, motion);

  g_signal_connect_object(web_view, "notify::title", G_CALLBACK(wig_tab_on_title_changed), self, G_CONNECT_SWAPPED);
#if HAVE_FAVICON_SUPPORT
  g_signal_connect_object(web_view, "notify::page-icons", G_CALLBACK(wig_tab_on_page_icons_changed), self,
                          G_CONNECT_SWAPPED);
#endif
  g_signal_connect_object(web_view, "load-changed", G_CALLBACK(wig_tab_on_load_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "script-dialog", G_CALLBACK(wig_tab_on_script_dialog), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "authenticate", G_CALLBACK(wig_tab_on_authenticate), self, G_CONNECT_SWAPPED);
  g_object_bind_property(G_OBJECT(web_view), "is-loading", self, "loading", G_BINDING_SYNC_CREATE);

  /* Pick up a title and icons the view may already have (e.g. a related view). */
  wig_tab_on_title_changed(self);
#if HAVE_FAVICON_SUPPORT
  wig_tab_on_page_icons_changed(self);
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
  if (self->discarded) {
    WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(self->web_view);
    WebKitBackForwardListItem *item = webkit_back_forward_list_get_current_item(list);
    if (item)
      return webkit_back_forward_list_item_get_uri(item);
  }

  return webkit_web_view_get_uri(self->web_view);
}

gboolean wig_tab_get_discarded(WigTab *self)
{
  return self->discarded;
}

/* A restored view holds a back/forward list but no page: nothing is loaded until
 * the tab is looked at. Take the label from the list so the tab is recognisable
 * in the meantime, as no title or icon will arrive from the web process. */
void wig_tab_mark_discarded(WigTab *self)
{
  g_assert(WIG_IS_TAB(self));

  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(self->web_view);
  WebKitBackForwardListItem *item = webkit_back_forward_list_get_current_item(list);
  if (!item)
    return;

  self->discarded = TRUE;

  const char *title = webkit_back_forward_list_item_get_title(item);
  if (title && *title) {
    wig_tab_set_title(self, title);
    return;
  }

  g_autofree char *host = wig_tab_uri_host(webkit_back_forward_list_item_get_uri(item));
  if (host)
    wig_tab_set_title(self, host);
}

void wig_tab_load_discarded(WigTab *self)
{
  g_assert(WIG_IS_TAB(self));

  if (!self->discarded)
    return;

  self->discarded = FALSE;

  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(self->web_view);
  WebKitBackForwardListItem *item = webkit_back_forward_list_get_current_item(list);
  if (!item)
    return;

  g_debug("tab: loading discarded tab %u (%s)", self->id, webkit_back_forward_list_item_get_uri(item));
  webkit_web_view_go_to_back_forward_list_item(self->web_view, item);
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

gboolean wig_tab_get_loading(WigTab *self)
{
  return self->loading;
}

gboolean wig_tab_get_selected(WigTab *self)
{
  return self->selected;
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
