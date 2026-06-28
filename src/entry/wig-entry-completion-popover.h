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

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _WigEntryCompletionItem WigEntryCompletionItem;

#define WIG_TYPE_ENTRY_COMPLETION_POPOVER (wig_entry_completion_popover_get_type())
G_DECLARE_FINAL_TYPE(WigEntryCompletionPopover, wig_entry_completion_popover, WIG, ENTRY_COMPLETION_POPOVER, GtkPopover)

WigEntryCompletionItem *wig_entry_completion_item_new(const char *title, const char *url, const char *subtitle,
                                                      const char *entry_text);
void wig_entry_completion_item_free(WigEntryCompletionItem *item);

GtkWidget *wig_entry_completion_popover_new(void);
void wig_entry_completion_popover_set_width(WigEntryCompletionPopover *self, int width);
void wig_entry_completion_popover_set_items(WigEntryCompletionPopover *self, const char *entry_text, GPtrArray *items);
gboolean wig_entry_completion_popover_select_next(WigEntryCompletionPopover *self);
gboolean wig_entry_completion_popover_select_previous(WigEntryCompletionPopover *self);
guint wig_entry_completion_popover_get_n_items(WigEntryCompletionPopover *self);

G_END_DECLS