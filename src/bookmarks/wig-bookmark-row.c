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

#include "wig-bookmark-row.h"

struct _WigBookmarkRow {
  GtkWidget parent;

  WigBookmark *bookmark;

  GtkWidget *box;
  GtkWidget *icon;
  GtkWidget *title;
  GtkWidget *subtitle;
  GtkWidget *context_menu;
};

G_DEFINE_FINAL_TYPE(WigBookmarkRow, wig_bookmark_row, GTK_TYPE_WIDGET)

static void wig_bookmark_row_dispose(GObject *object)
{
  WigBookmarkRow *self = WIG_BOOKMARK_ROW(object);

  g_clear_pointer(&self->context_menu, gtk_widget_unparent);
  g_clear_pointer(&self->box, gtk_widget_unparent);
  g_clear_object(&self->bookmark);

  G_OBJECT_CLASS(wig_bookmark_row_parent_class)->dispose(object);
}

static void wig_bookmark_row_class_init(WigBookmarkRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_bookmark_row_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-bookmark-row");
}

static void wig_bookmark_row_init(WigBookmarkRow *self)
{
  self->icon = gtk_image_new();

  self->title = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->title), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(self->title), PANGO_ELLIPSIZE_END);

  self->subtitle = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->subtitle), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(self->subtitle), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(self->subtitle, "subtitle");

  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(labels, TRUE);
  gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(labels), self->title);
  gtk_box_append(GTK_BOX(labels), self->subtitle);

  self->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append(GTK_BOX(self->box), self->icon);
  gtk_box_append(GTK_BOX(self->box), labels);
  gtk_widget_set_parent(self->box, GTK_WIDGET(self));
}

GtkWidget *wig_bookmark_row_new(void)
{
  return g_object_new(WIG_TYPE_BOOKMARK_ROW, NULL);
}

void wig_bookmark_row_set_bookmark(WigBookmarkRow *self, WigBookmark *bookmark)
{
  if (!g_set_object(&self->bookmark, bookmark))
    return;

  if (!bookmark)
    return;

  gboolean is_folder = wig_bookmark_get_is_folder(bookmark);
  const char *title = wig_bookmark_get_title(bookmark);
  const char *url = wig_bookmark_get_url(bookmark);

  gtk_image_set_from_icon_name(GTK_IMAGE(self->icon), is_folder ? "folder-symbolic" : "user-bookmarks-symbolic");
  gtk_label_set_label(GTK_LABEL(self->title), title && *title ? title : url);
  gtk_label_set_label(GTK_LABEL(self->subtitle), is_folder ? "" : url);
  gtk_widget_set_visible(self->subtitle, !is_folder && url && *url);
}

WigBookmark *wig_bookmark_row_get_bookmark(WigBookmarkRow *self)
{
  return self->bookmark;
}

void wig_bookmark_row_show_context_menu(WigBookmarkRow *self, GtkWidget *menu, double x, double y)
{
  g_clear_pointer(&self->context_menu, gtk_widget_unparent);

  self->context_menu = menu;
  gtk_widget_set_parent(menu, GTK_WIDGET(self));
  gtk_popover_set_pointing_to(GTK_POPOVER(menu), &(GdkRectangle) { (int)x, (int)y, 1, 1 });
  gtk_popover_popup(GTK_POPOVER(menu));
}
