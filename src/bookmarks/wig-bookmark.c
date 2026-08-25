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

#include "wig-bookmark.h"

struct _WigBookmark {
  GObject parent;

  char *id;
  char *parent_id;
  gboolean is_folder;
  char *title;
  char *url;
  int position;
  gint64 date_added;
  gint64 last_modified;
};

G_DEFINE_FINAL_TYPE(WigBookmark, wig_bookmark, G_TYPE_OBJECT)

typedef enum {
  PROP_ID = 1,
  PROP_PARENT_ID,
  PROP_IS_FOLDER,
  PROP_TITLE,
  PROP_URL,
  PROP_POSITION,
  PROP_DATE_ADDED,
  PROP_LAST_MODIFIED,
} WigBookmarkProps;

static GParamSpec *props[PROP_LAST_MODIFIED + 1];

static void wig_bookmark_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigBookmark *self = WIG_BOOKMARK(object);

  switch ((WigBookmarkProps)prop_id) {
  case PROP_ID:
    g_value_set_string(value, self->id);
    break;
  case PROP_PARENT_ID:
    g_value_set_string(value, self->parent_id);
    break;
  case PROP_IS_FOLDER:
    g_value_set_boolean(value, self->is_folder);
    break;
  case PROP_TITLE:
    g_value_set_string(value, self->title);
    break;
  case PROP_URL:
    g_value_set_string(value, self->url);
    break;
  case PROP_POSITION:
    g_value_set_int(value, self->position);
    break;
  case PROP_DATE_ADDED:
    g_value_set_int64(value, self->date_added);
    break;
  case PROP_LAST_MODIFIED:
    g_value_set_int64(value, self->last_modified);
    break;
  }
}

static void wig_bookmark_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  WigBookmark *self = WIG_BOOKMARK(object);

  switch ((WigBookmarkProps)prop_id) {
  case PROP_ID:
    g_free(self->id);
    self->id = g_value_dup_string(value);
    break;
  case PROP_PARENT_ID:
    g_free(self->parent_id);
    self->parent_id = g_value_dup_string(value);
    break;
  case PROP_IS_FOLDER:
    self->is_folder = g_value_get_boolean(value);
    break;
  case PROP_TITLE:
    g_free(self->title);
    self->title = g_value_dup_string(value);
    break;
  case PROP_URL:
    g_free(self->url);
    self->url = g_value_dup_string(value);
    break;
  case PROP_POSITION:
    self->position = g_value_get_int(value);
    break;
  case PROP_DATE_ADDED:
    self->date_added = g_value_get_int64(value);
    break;
  case PROP_LAST_MODIFIED:
    self->last_modified = g_value_get_int64(value);
    break;
  }
}

static void wig_bookmark_finalize(GObject *object)
{
  WigBookmark *self = WIG_BOOKMARK(object);

  g_free(self->id);
  g_free(self->parent_id);
  g_free(self->title);
  g_free(self->url);

  G_OBJECT_CLASS(wig_bookmark_parent_class)->finalize(object);
}

static void wig_bookmark_class_init(WigBookmarkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = wig_bookmark_get_property;
  object_class->set_property = wig_bookmark_set_property;
  object_class->finalize = wig_bookmark_finalize;

  props[PROP_ID] = g_param_spec_string("id", NULL, NULL, NULL,
                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  props[PROP_PARENT_ID] = g_param_spec_string("parent-id", NULL, NULL, NULL,
                                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_IS_FOLDER] = g_param_spec_boolean("is-folder", NULL, NULL, FALSE,
                                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_TITLE] = g_param_spec_string("title", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_URL] = g_param_spec_string("url", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_POSITION] = g_param_spec_int("position", NULL, NULL, 0, G_MAXINT, 0,
                                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_DATE_ADDED] = g_param_spec_int64("date-added", NULL, NULL, 0, G_MAXINT64, 0,
                                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_LAST_MODIFIED] = g_param_spec_int64("last-modified", NULL, NULL, 0, G_MAXINT64, 0,
                                                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, G_N_ELEMENTS(props), props);
}

static void wig_bookmark_init(WigBookmark *self)
{
}

WigBookmark *wig_bookmark_new(const char *id, const char *parent_id, gboolean is_folder, const char *title,
                              const char *url, int position, gint64 date_added, gint64 last_modified)
{
  return g_object_new(WIG_TYPE_BOOKMARK, "id", id, "parent-id", parent_id, "is-folder", is_folder, "title", title,
                      "url", url, "position", position, "date-added", date_added, "last-modified", last_modified, NULL);
}

gboolean wig_bookmark_id_is_root(const char *id)
{
  return g_strcmp0(id, WIG_BOOKMARKS_ROOT_FAVORITES) == 0;
}

const char *wig_bookmark_get_id(WigBookmark *self)
{
  return self->id;
}

const char *wig_bookmark_get_parent_id(WigBookmark *self)
{
  return self->parent_id;
}

gboolean wig_bookmark_get_is_folder(WigBookmark *self)
{
  return self->is_folder;
}

const char *wig_bookmark_get_title(WigBookmark *self)
{
  return self->title;
}

const char *wig_bookmark_get_url(WigBookmark *self)
{
  return self->url;
}

int wig_bookmark_get_position(WigBookmark *self)
{
  return self->position;
}

gint64 wig_bookmark_get_date_added(WigBookmark *self)
{
  return self->date_added;
}

gint64 wig_bookmark_get_last_modified(WigBookmark *self)
{
  return self->last_modified;
}
