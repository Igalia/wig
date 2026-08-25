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

#include <glib-object.h>

G_BEGIN_DECLS

#define WIG_BOOKMARKS_ROOT_FAVORITES "favorites"

#define WIG_TYPE_BOOKMARK (wig_bookmark_get_type())
G_DECLARE_FINAL_TYPE(WigBookmark, wig_bookmark, WIG, BOOKMARK, GObject)

gboolean wig_bookmark_id_is_root(const char *id);

WigBookmark *wig_bookmark_new(const char *id, const char *parent_id, gboolean is_folder, const char *title,
                              const char *url, int position, gint64 date_added, gint64 last_modified);

const char *wig_bookmark_get_id(WigBookmark *self);
const char *wig_bookmark_get_parent_id(WigBookmark *self);
gboolean wig_bookmark_get_is_folder(WigBookmark *self);
const char *wig_bookmark_get_title(WigBookmark *self);
const char *wig_bookmark_get_url(WigBookmark *self);
int wig_bookmark_get_position(WigBookmark *self);
gint64 wig_bookmark_get_date_added(WigBookmark *self);
gint64 wig_bookmark_get_last_modified(WigBookmark *self);

G_END_DECLS
