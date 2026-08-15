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

#define WIG_TYPE_HISTORY_ITEM (wig_history_item_get_type())
G_DECLARE_FINAL_TYPE(WigHistoryItem, wig_history_item, WIG, HISTORY_ITEM, GObject)

WigHistoryItem *wig_history_item_new(const char *id, const char *url, const char *title, gint64 last_visit_time,
                                     guint visit_count, guint typed_count);

const char *wig_history_item_get_id(WigHistoryItem *self);
const char *wig_history_item_get_url(WigHistoryItem *self);
const char *wig_history_item_get_title(WigHistoryItem *self);
gint64 wig_history_item_get_last_visit_time(WigHistoryItem *self);
guint wig_history_item_get_visit_count(WigHistoryItem *self);
guint wig_history_item_get_typed_count(WigHistoryItem *self);

G_END_DECLS