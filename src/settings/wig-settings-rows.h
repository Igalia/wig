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

#include <adwaita.h>

G_BEGIN_DECLS

GtkWidget *wig_settings_switch_row_new(GSettings *settings, const char *key, const char *title, const char *subtitle);

/* @nicks holds the value stored for each of @labels, in the same order, and ends
 * with NULL. It reaches the binding as its user data, which is why it is not
 * const all the way down. */
GtkWidget *wig_settings_combo_row_new(GSettings *settings, const char *key, const char *title, const char *subtitle,
                                      const char **nicks, const char *const *labels);

void wig_settings_search_engine_rows_add(AdwPreferencesGroup *group, GSettings *settings);

G_END_DECLS
