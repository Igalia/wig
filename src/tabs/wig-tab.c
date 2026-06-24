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

#include "wpe-view-gtk.h"

struct _WigTab {
  GObject parent;

  WebKitWebView *web_view;
  GIcon *icon;
  char *title;
  gboolean pinned;
  gboolean loading;
  gboolean selected;
};

G_DEFINE_FINAL_TYPE(WigTab, wig_tab, G_TYPE_OBJECT)

enum { PROP_0, PROP_ICON, PROP_TITLE, PROP_PINNED, PROP_LOADING, PROP_SELECTED, N_PROPS };

static GParamSpec *props[N_PROPS];

static void wig_tab_set_title(WigTab *self, const char *title)
{
  if (g_strcmp0(self->title, title) == 0)
    return;
  g_free(self->title);
  self->title = g_strdup(title);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TITLE]);
}

static void wig_tab_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigTab *self = WIG_TAB(object);
  switch (prop_id) {
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
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_tab_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigTab *self = WIG_TAB(object);
  switch (prop_id) {
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
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_tab_dispose(GObject *object)
{
  WigTab *self = WIG_TAB(object);
  g_clear_object(&self->icon);
  if (self->web_view)
    g_signal_handlers_disconnect_by_data(self->web_view, self);
  g_clear_object(&self->web_view);
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

  g_object_class_install_properties(gobject_class, N_PROPS, props);
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
  if (load_event != WEBKIT_LOAD_COMMITTED)
    return;

  const char *title = webkit_web_view_get_title(self->web_view);
  if (title && *title)
    return;

  g_autofree char *host = wig_tab_uri_host(webkit_web_view_get_uri(self->web_view));
  wig_tab_set_title(self, host ? host : "New Tab");
}

static GIcon *wig_tab_best_page_icon(WebKitImageList *icons)
{
  if (!icons)
    return NULL;

  WebKitImage *best = NULL; /* smallest icon >= WIG_TAB_FAVICON_SIZE */
  WebKitImage *largest = NULL; /* fallback when all icons are smaller */
  int best_width = 0;
  int largest_width = -1;

  gsize n = webkit_image_list_get_length(icons);
  for (gsize i = 0; i < n; i++) {
    WebKitImage *image = webkit_image_list_get(icons, i);
    int width = webkit_image_get_width(image);
    if (width > largest_width) {
      largest = image;
      largest_width = width;
    }
    if (width >= WIG_TAB_FAVICON_SIZE && (!best || width < best_width)) {
      best = image;
      best_width = width;
    }
  }

  return G_ICON(best ? best : largest);
}

static void wig_tab_on_page_icons_changed(WigTab *self)
{
  // FIXME: Maybe we could have a custom GIconLoadable that is backed by the list.
  // on loading, which is passed a size, it then chooses the best one?
  // This allows for DPI changes working automatically?
  wig_tab_set_icon(self, wig_tab_best_page_icon(webkit_web_view_get_page_icons(self->web_view)));
}

WigTab *wig_tab_new(WebKitWebView *web_view)
{
  g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);

  WigTab *self = WIG_TAB(g_object_new(WIG_TYPE_TAB, NULL));
  self->web_view = g_object_ref(web_view);

  g_signal_connect_object(web_view, "notify::title", G_CALLBACK(wig_tab_on_title_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "notify::page-icons", G_CALLBACK(wig_tab_on_page_icons_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(web_view, "load-changed", G_CALLBACK(wig_tab_on_load_changed), self, G_CONNECT_SWAPPED);
  g_object_bind_property(G_OBJECT(web_view), "is-loading", self, "loading", G_BINDING_SYNC_CREATE);

  /* Pick up a title and icons the view may already have (e.g. a related view). */
  wig_tab_on_title_changed(self);
  wig_tab_on_page_icons_changed(self);

  return self;
}

WebKitWebView *wig_tab_get_web_view(WigTab *self)
{
  return self->web_view;
}

GtkWidget *wig_tab_get_widget(WigTab *self)
{
  return wpe_view_gtk_get_widget(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(self->web_view)));
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

void wig_tab_set_selected(WigTab *self, gboolean selected)
{
  if (self->selected == selected)
    return;
  self->selected = selected;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SELECTED]);
}
