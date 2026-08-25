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

#pragma once

#include "wig-bookmark.h"

G_BEGIN_DECLS

#define WIG_TYPE_BOOKMARKS_STORE (wig_bookmarks_store_get_type())
G_DECLARE_FINAL_TYPE(WigBookmarksStore, wig_bookmarks_store, WIG, BOOKMARKS_STORE, GObject)

WigBookmarksStore *wig_bookmarks_store_new(const char *state_dir, GError **error);

WigBookmark *wig_bookmarks_store_add(WigBookmarksStore *self, const char *parent_id, const char *title, const char *url,
                                     GError **error);
WigBookmark *wig_bookmarks_store_add_folder(WigBookmarksStore *self, const char *parent_id, const char *title,
                                            GError **error);
gboolean wig_bookmarks_store_update(WigBookmarksStore *self, const char *id, const char *title, const char *url,
                                    GError **error);
gboolean wig_bookmarks_store_move(WigBookmarksStore *self, const char *id, const char *parent_id, int position,
                                  GError **error);
gboolean wig_bookmarks_store_remove(WigBookmarksStore *self, const char *id, GError **error);

WigBookmark *wig_bookmarks_store_get(WigBookmarksStore *self, const char *id, GError **error);
WigBookmark *wig_bookmarks_store_find_by_url(WigBookmarksStore *self, const char *url, GError **error);
GPtrArray *wig_bookmarks_store_get_children(WigBookmarksStore *self, const char *parent_id, GError **error);
GPtrArray *wig_bookmarks_store_get_ancestors(WigBookmarksStore *self, const char *id, GError **error);
GPtrArray *wig_bookmarks_store_get_folders(WigBookmarksStore *self, GError **error);
GPtrArray *wig_bookmarks_store_search(WigBookmarksStore *self, const char *text, guint limit, GError **error);

G_END_DECLS
