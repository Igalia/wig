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

#include "wig-history-item.h"

struct _WigHistoryItem {
  GObject parent;

  char *id;
  char *url;
  char *title;
  gint64 last_visit_time;
  guint visit_count;
  guint typed_count;
};

G_DEFINE_FINAL_TYPE(WigHistoryItem, wig_history_item, G_TYPE_OBJECT)

typedef enum {
  PROP_ID = 1,
  PROP_URL,
  PROP_TITLE,
  PROP_LAST_VISIT_TIME,
  PROP_VISIT_COUNT,
  PROP_TYPED_COUNT,
} WigHistoryItemProps;

static GParamSpec *props[PROP_TYPED_COUNT + 1];

static void wig_history_item_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigHistoryItem *self = WIG_HISTORY_ITEM(object);

  switch ((WigHistoryItemProps)prop_id) {
  case PROP_ID:
    g_value_set_string(value, self->id);
    break;
  case PROP_URL:
    g_value_set_string(value, self->url);
    break;
  case PROP_TITLE:
    g_value_set_string(value, self->title);
    break;
  case PROP_LAST_VISIT_TIME:
    g_value_set_int64(value, self->last_visit_time);
    break;
  case PROP_VISIT_COUNT:
    g_value_set_uint(value, self->visit_count);
    break;
  case PROP_TYPED_COUNT:
    g_value_set_uint(value, self->typed_count);
    break;
  }
}

static void wig_history_item_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigHistoryItem *self = WIG_HISTORY_ITEM(object);

  switch ((WigHistoryItemProps)prop_id) {
  case PROP_ID:
    g_free(self->id);
    self->id = g_value_dup_string(value);
    break;
  case PROP_URL:
    g_free(self->url);
    self->url = g_value_dup_string(value);
    break;
  case PROP_TITLE:
    g_free(self->title);
    self->title = g_value_dup_string(value);
    break;
  case PROP_LAST_VISIT_TIME:
    self->last_visit_time = g_value_get_int64(value);
    break;
  case PROP_VISIT_COUNT:
    self->visit_count = g_value_get_uint(value);
    break;
  case PROP_TYPED_COUNT:
    self->typed_count = g_value_get_uint(value);
    break;
  }
}

static void wig_history_item_finalize(GObject *object)
{
  WigHistoryItem *self = WIG_HISTORY_ITEM(object);

  g_free(self->id);
  g_free(self->url);
  g_free(self->title);

  G_OBJECT_CLASS(wig_history_item_parent_class)->finalize(object);
}

static void wig_history_item_class_init(WigHistoryItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = wig_history_item_get_property;
  object_class->set_property = wig_history_item_set_property;
  object_class->finalize = wig_history_item_finalize;

  props[PROP_ID] = g_param_spec_string("id", NULL, NULL, NULL,
                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  props[PROP_URL] = g_param_spec_string("url", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_TITLE] = g_param_spec_string("title", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_LAST_VISIT_TIME] = g_param_spec_int64("last-visit-time", NULL, NULL, 0, G_MAXINT64, 0,
                                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_VISIT_COUNT] = g_param_spec_uint("visit-count", NULL, NULL, 0, G_MAXUINT, 0,
                                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_TYPED_COUNT] = g_param_spec_uint("typed-count", NULL, NULL, 0, G_MAXUINT, 0,
                                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, G_N_ELEMENTS(props), props);
}

static void wig_history_item_init(WigHistoryItem *self)
{
}

WigHistoryItem *wig_history_item_new(const char *id, const char *url, const char *title, gint64 last_visit_time,
                                     guint visit_count, guint typed_count)
{
  return g_object_new(WIG_TYPE_HISTORY_ITEM, "id", id, "url", url, "title", title, "last-visit-time", last_visit_time,
                      "visit-count", visit_count, "typed-count", typed_count, NULL);
}

const char *wig_history_item_get_id(WigHistoryItem *self)
{
  return self->id;
}

const char *wig_history_item_get_url(WigHistoryItem *self)
{
  return self->url;
}

const char *wig_history_item_get_title(WigHistoryItem *self)
{
  return self->title;
}

gint64 wig_history_item_get_last_visit_time(WigHistoryItem *self)
{
  return self->last_visit_time;
}

guint wig_history_item_get_visit_count(WigHistoryItem *self)
{
  return self->visit_count;
}

guint wig_history_item_get_typed_count(WigHistoryItem *self)
{
  return self->typed_count;
}