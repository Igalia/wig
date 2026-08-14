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

#include "wig-page-details.h"

GtkWidget *wig_page_details_new(void)
{
  GtkWidget *grid = gtk_grid_new();

  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_widget_add_css_class(grid, "page-details");

  return grid;
}

void wig_page_details_add(GtkWidget *details, int row, const char *name, const char *value)
{
  g_assert(GTK_IS_GRID(details));

  GtkWidget *name_label = gtk_label_new(name);
  gtk_label_set_xalign(GTK_LABEL(name_label), 1.0f);
  gtk_widget_add_css_class(name_label, "dim-label");
  gtk_widget_set_valign(name_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(details), name_label, 0, row, 1, 1);

  GtkWidget *value_label = gtk_label_new(value);
  gtk_label_set_xalign(GTK_LABEL(value_label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(value_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(value_label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_hexpand(value_label, TRUE);
  gtk_widget_add_css_class(value_label, "page-detail-value");
  gtk_grid_attach(GTK_GRID(details), value_label, 1, row, 1, 1);
}
