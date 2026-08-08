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

#include "wig-download-context-menu.h"

static void append_section(GMenu *menu, const char *const *labels, const char *const *actions, gsize n)
{
  g_autoptr(GMenu) section = g_menu_new();
  for (gsize i = 0; i < n; i++)
    g_menu_append(section, labels[i], actions[i]);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
}

GtkWidget *wig_download_context_menu_new(void)
{
  static const char *const open_labels[] = { "Open File", "Open Containing Folder" };
  static const char *const open_actions[] = { "download.open-file", "download.open-folder" };
  static const char *const copy_labels[] = { "Copy Download Link" };
  static const char *const copy_actions[] = { "download.copy-link" };
  static const char *const list_labels[] = { "Remove From List" };
  static const char *const list_actions[] = { "download.remove" };
  static const char *const file_labels[] = { "Trash File", "Permanently Delete File" };
  static const char *const file_actions[] = { "download.trash", "download.delete" };

  g_autoptr(GMenu) menu = g_menu_new();
  append_section(menu, open_labels, open_actions, G_N_ELEMENTS(open_labels));
  append_section(menu, copy_labels, copy_actions, G_N_ELEMENTS(copy_labels));
  append_section(menu, list_labels, list_actions, G_N_ELEMENTS(list_labels));
  append_section(menu, file_labels, file_actions, G_N_ELEMENTS(file_labels));

  GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_widget_set_halign(popover, GTK_ALIGN_START);

  return popover;
}
