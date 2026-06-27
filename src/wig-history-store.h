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

#include "wig-history-item.h"

G_BEGIN_DECLS

#define WIG_TYPE_HISTORY_STORE (wig_history_store_get_type())
G_DECLARE_FINAL_TYPE(WigHistoryStore, wig_history_store, WIG, HISTORY_STORE, GObject)

WigHistoryStore *wig_history_store_new(const char *state_dir, GError **error);

void wig_history_store_record_visit(WigHistoryStore *self, const char *url, const char *title, gboolean typed,
                                    gint64 visit_time, GError **error);
void wig_history_store_update_title(WigHistoryStore *self, const char *url, const char *title, GError **error);
GPtrArray *wig_history_store_query(WigHistoryStore *self, const char *search, gint64 before_time, guint limit,
                                   gboolean *has_more, GError **error);

G_END_DECLS