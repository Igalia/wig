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

#include "wig-downloads-button.h"

#include "wig-application.h"
#include "wig-download-row.h"
#include "wig-downloads-paintable.h"

#define ATTENTION_MS 2000
#define LIST_WIDTH 460
#define LIST_HEIGHT 420

struct _WigDownloadsButton {
  GtkWidget parent;

  GtkWidget *revealer;
  GtkWidget *menu_button;
  GtkWidget *image;
  GtkWidget *popover;
  GtkWidget *clear_button;
  GtkWidget *error_label;
  GtkWidget *empty_label;
  GtkWidget *list;
  GtkWidget *scroller;
  WigDownloadsPaintable *paintable;

  WigDownloadsManager *manager;
  gboolean popover_open;
  guint attention_id;
};

G_DEFINE_FINAL_TYPE(WigDownloadsButton, wig_downloads_button, GTK_TYPE_WIDGET)

static void wig_downloads_button_show_error(WigDownloadsButton *self, const char *message)
{
  if (message)
    gtk_label_set_text(GTK_LABEL(self->error_label), message);
  gtk_widget_set_visible(self->error_label, message != NULL);
}

/* Rows borrow their record, so a record the manager has dropped must lose its
 * row here before anything reads it again. */
static void wig_downloads_button_sync_list(WigDownloadsButton *self)
{
  GPtrArray *records = wig_downloads_manager_get_records(self->manager);
  g_autoptr(GHashTable) shown = g_hash_table_new(NULL, NULL);

  GtkWidget *next = NULL;
  for (GtkWidget *child = gtk_widget_get_first_child(self->list); child; child = next) {
    next = gtk_widget_get_next_sibling(child);

    WigDownloadRow *row = WIG_DOWNLOAD_ROW(child);
    WigDownloadRecord *record = wig_download_row_get_record(row);
    if (!g_ptr_array_find(records, record, NULL)) {
      gtk_box_remove(GTK_BOX(self->list), child);
      continue;
    }

    wig_download_row_update(row);
    g_hash_table_add(shown, record);
  }

  /* Records are kept oldest first, so prepending them in that order leaves the
   * newest download at the top, above the rows that are already there. */
  for (guint i = 0; i < records->len; i++) {
    WigDownloadRecord *record = g_ptr_array_index(records, i);
    if (g_hash_table_contains(shown, record))
      continue;

    GtkWidget *row = wig_download_row_new(record);
    g_signal_connect_object(row, "error", G_CALLBACK(wig_downloads_button_show_error), self, G_CONNECT_SWAPPED);
    gtk_box_prepend(GTK_BOX(self->list), row);
  }

  /* An empty scroller still asks for room for its scrollbar, which would make
   * the empty popover taller than the one-row popover it turns into. */
  gboolean empty = wig_downloads_manager_is_empty(self->manager);
  gtk_widget_set_visible(self->empty_label, empty);
  gtk_widget_set_visible(self->scroller, !empty);
  gtk_widget_set_sensitive(self->clear_button, wig_downloads_manager_has_finished(self->manager));
}

static void wig_downloads_button_update(WigDownloadsButton *self)
{
  /* Once the popover is up the button has to stay put underneath it, even for an
   * empty list the user asked to see. */
  gboolean any = !wig_downloads_manager_is_empty(self->manager);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->revealer), any || self->popover_open);

  double progress = wig_downloads_manager_get_progress(self->manager);
  wig_downloads_paintable_set_active(self->paintable, progress > 0);
  wig_downloads_paintable_set_progress(self->paintable, progress);

  /* A closed popover is caught up when it is shown, so progress ticks only cost
   * a list rebuild while it is actually on screen. */
  if (self->popover_open)
    wig_downloads_button_sync_list(self);
}

static void wig_downloads_button_clear_clicked(WigDownloadsButton *self)
{
  wig_downloads_button_show_error(self, NULL);
  wig_downloads_manager_clear_finished(self->manager);
}

static void wig_downloads_button_attention_over(WigDownloadsButton *self)
{
  self->attention_id = 0;
  gtk_widget_remove_css_class(self->image, "accent");
}

static void wig_downloads_button_download_added(WigDownloadsButton *self)
{
  g_clear_handle_id(&self->attention_id, g_source_remove);
  gtk_widget_add_css_class(self->image, "accent");
  self->attention_id = g_timeout_add_once(ATTENTION_MS, (GSourceOnceFunc)wig_downloads_button_attention_over, self);

  wig_downloads_button_update(self);
}

static void wig_downloads_button_download_completed(WigDownloadsButton *self)
{
  wig_downloads_paintable_flash_done(self->paintable);
}

static void wig_downloads_button_popover_shown(WigDownloadsButton *self)
{
  self->popover_open = TRUE;

  wig_downloads_button_show_error(self, NULL);
  wig_downloads_button_sync_list(self);
}

static void wig_downloads_button_popover_closed(WigDownloadsButton *self)
{
  self->popover_open = FALSE;

  wig_downloads_button_update(self);
}

static void wig_downloads_button_dispose(GObject *object)
{
  WigDownloadsButton *self = WIG_DOWNLOADS_BUTTON(object);

  g_clear_handle_id(&self->attention_id, g_source_remove);
  g_clear_pointer(&self->revealer, gtk_widget_unparent);
  g_clear_object(&self->paintable);

  G_OBJECT_CLASS(wig_downloads_button_parent_class)->dispose(object);
}

static void wig_downloads_button_class_init(WigDownloadsButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_downloads_button_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-downloads-button");
}

static GtkWidget *wig_downloads_button_build_list(WigDownloadsButton *self)
{
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(content, LIST_WIDTH, -1);

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(header, "downloads-header");

  GtkWidget *title = gtk_label_new("Downloads");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_add_css_class(title, "heading");
  gtk_box_append(GTK_BOX(header), title);

  self->clear_button = gtk_button_new_with_label("Clear completed");
  gtk_widget_add_css_class(self->clear_button, "flat");
  g_signal_connect_swapped(self->clear_button, "clicked", G_CALLBACK(wig_downloads_button_clear_clicked), self);
  gtk_box_append(GTK_BOX(header), self->clear_button);

  gtk_box_append(GTK_BOX(content), header);

  self->error_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->error_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->error_label), TRUE);
  gtk_widget_add_css_class(self->error_label, "downloads-error");
  gtk_widget_set_visible(self->error_label, FALSE);
  gtk_box_append(GTK_BOX(content), self->error_label);

  self->empty_label = gtk_label_new("No downloads yet.");
  gtk_widget_add_css_class(self->empty_label, "downloads-empty");
  gtk_widget_add_css_class(self->empty_label, "dim-label");
  gtk_box_append(GTK_BOX(content), self->empty_label);

  self->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->list, "downloads-list");

  self->scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(self->scroller), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(self->scroller), LIST_HEIGHT);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller), self->list);
  gtk_box_append(GTK_BOX(content), self->scroller);

  return content;
}

static void wig_downloads_button_init(WigDownloadsButton *self)
{
  self->popover = gtk_popover_new();
  gtk_widget_add_css_class(self->popover, "downloads-popover");
  gtk_popover_set_child(GTK_POPOVER(self->popover), wig_downloads_button_build_list(self));

  self->image = gtk_image_new();
  gtk_widget_set_valign(self->image, GTK_ALIGN_CENTER);

  self->menu_button = gtk_menu_button_new();
  gtk_menu_button_set_child(GTK_MENU_BUTTON(self->menu_button), self->image);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->menu_button), self->popover);
  gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(self->menu_button), FALSE);
  gtk_widget_set_tooltip_text(self->menu_button, "Downloads");
  gtk_widget_add_css_class(self->menu_button, "toolbar-button");

  self->revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->revealer), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
  gtk_revealer_set_child(GTK_REVEALER(self->revealer), self->menu_button);
  gtk_widget_set_parent(self->revealer, GTK_WIDGET(self));

  /* The paintable needs a widget to take its scale factor and animation clock
   * from, so it can only be built once the image exists. */
  self->paintable = WIG_DOWNLOADS_PAINTABLE(wig_downloads_paintable_new(self->image));
  gtk_image_set_from_paintable(GTK_IMAGE(self->image), GDK_PAINTABLE(self->paintable));

  g_signal_connect_swapped(self->popover, "show", G_CALLBACK(wig_downloads_button_popover_shown), self);
  g_signal_connect_swapped(self->popover, "closed", G_CALLBACK(wig_downloads_button_popover_closed), self);

  self->manager = wig_application_get_downloads_manager(wig_application_get());
  g_signal_connect_object(self->manager, "added", G_CALLBACK(wig_downloads_button_download_added), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->manager, "completed", G_CALLBACK(wig_downloads_button_download_completed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->manager, "changed", G_CALLBACK(wig_downloads_button_update), self, G_CONNECT_SWAPPED);

  wig_downloads_button_update(self);
}

GtkWidget *wig_downloads_button_new(void)
{
  return GTK_WIDGET(g_object_new(WIG_TYPE_DOWNLOADS_BUTTON, NULL));
}

void wig_downloads_button_popup(WigDownloadsButton *self)
{
  /* Asking for the list is reason enough to show the button, even with nothing
   * downloaded yet: the popover has to have something to point at. */
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->revealer), TRUE);
  gtk_menu_button_popup(GTK_MENU_BUTTON(self->menu_button));
}
