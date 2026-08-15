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

#include "wig-native-page.h"

typedef struct {
  char *title;
  char *uri;
} WigNativePagePrivate;

/* A page wig draws itself in place of a document. It carries the title and the
 * address the document would have had, and keeps them up to date as the user
 * moves around inside it; the tab showing it follows both.
 *
 * The address is what the view navigates to, so a page that answers to more than
 * one of them says which it is at rather than being built again for each. */
G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(WigNativePage, wig_native_page, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_TITLE,
  PROP_URI,
  N_PROPS,
};

static GParamSpec *props[N_PROPS];

const char *wig_native_page_get_title(WigNativePage *self)
{
  WigNativePagePrivate *priv = wig_native_page_get_instance_private(self);

  return priv->title;
}

void wig_native_page_set_title(WigNativePage *self, const char *title)
{
  WigNativePagePrivate *priv = wig_native_page_get_instance_private(self);

  if (g_set_str(&priv->title, title))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TITLE]);
}

const char *wig_native_page_get_uri(WigNativePage *self)
{
  WigNativePagePrivate *priv = wig_native_page_get_instance_private(self);

  return priv->uri;
}

void wig_native_page_set_uri(WigNativePage *self, const char *uri)
{
  WigNativePagePrivate *priv = wig_native_page_get_instance_private(self);

  if (g_set_str(&priv->uri, uri))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_URI]);
}

gboolean wig_native_page_start_search(WigNativePage *self)
{
  WigNativePageClass *klass = WIG_NATIVE_PAGE_GET_CLASS(self);

  return klass->start_search ? klass->start_search(self) : FALSE;
}

static void wig_native_page_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigNativePage *self = WIG_NATIVE_PAGE(object);

  switch (prop_id) {
  case PROP_TITLE:
    g_value_set_string(value, wig_native_page_get_title(self));
    break;
  case PROP_URI:
    g_value_set_string(value, wig_native_page_get_uri(self));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_native_page_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigNativePage *self = WIG_NATIVE_PAGE(object);

  switch (prop_id) {
  case PROP_URI:
    wig_native_page_set_uri(self, g_value_get_string(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_native_page_finalize(GObject *object)
{
  WigNativePagePrivate *priv = wig_native_page_get_instance_private(WIG_NATIVE_PAGE(object));

  g_clear_pointer(&priv->title, g_free);
  g_clear_pointer(&priv->uri, g_free);

  G_OBJECT_CLASS(wig_native_page_parent_class)->finalize(object);
}

static void wig_native_page_class_init(WigNativePageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->get_property = wig_native_page_get_property;
  object_class->set_property = wig_native_page_set_property;
  object_class->finalize = wig_native_page_finalize;

  /* Only the page knows what it is called, so the title is read and watched
   * rather than handed to it. */
  props[PROP_TITLE] = g_param_spec_string("title", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_URI] = g_param_spec_string("uri", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  /* Every page so far is one widget filling the tab. */
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
}

static void wig_native_page_init(WigNativePage *self)
{
  /* Nothing in the page holds the focus to begin with, and a container cannot
   * take it on their behalf without private API. Focusing the page itself puts
   * it in the path a key press takes, which is what lets a page answer a
   * shortcut the browser would otherwise take for itself. */
  gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
}
