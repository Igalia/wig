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

#include "wig-downloads-list.h"

#include "wig-application.h"
#include "wig-download-row.h"

struct _WigDownloadsList {
  GtkWidget parent;

  WigDownloadsManager *manager;
  /* Folded, since that is what every row is compared against. */
  char *terms;

  GtkWidget *box;
  GtkWidget *error;
  GtkWidget *empty;
  GtkWidget *list;
  GtkWidget *scroller;
};

G_DEFINE_FINAL_TYPE(WigDownloadsList, wig_downloads_list, GTK_TYPE_WIDGET)

static void wig_downloads_list_show_error(WigDownloadsList *self, const char *message)
{
  if (message)
    gtk_label_set_text(GTK_LABEL(self->error), message);
  gtk_widget_set_visible(self->error, message != NULL);
}

void wig_downloads_list_clear_error(WigDownloadsList *self)
{
  wig_downloads_list_show_error(self, NULL);
}

static gboolean text_matches(const char *text, const char *terms)
{
  if (!text)
    return FALSE;

  g_autofree char *folded = g_utf8_casefold(text, -1);

  return strstr(folded, terms) != NULL;
}

/* A download is looked for by the name it was saved under or by where it came
 * from, which are the two things its row shows. */
static gboolean record_matches(WigDownloadRecord *record, const char *terms)
{
  if (!terms)
    return TRUE;

  const char *destination = webkit_download_get_destination(record->download);
  g_autofree char *name = destination ? g_path_get_basename(destination) : NULL;

  return text_matches(name, terms)
      || text_matches(webkit_uri_request_get_uri(webkit_download_get_request(record->download)), terms);
}

/* Rows borrow their record, so a record the manager has dropped must lose its
 * row here before anything reads it again. */
void wig_downloads_list_sync(WigDownloadsList *self)
{
  GPtrArray *records = wig_downloads_manager_get_records(self->manager);
  g_autoptr(GHashTable) shown = g_hash_table_new(NULL, NULL);
  guint matching = 0;

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
    g_signal_connect_object(row, "error", G_CALLBACK(wig_downloads_list_show_error), self, G_CONNECT_SWAPPED);
    gtk_box_prepend(GTK_BOX(self->list), row);
  }

  /* A search leaves the rows where they are and takes the ones it does not want
   * off screen, so what is left keeps the order it had. */
  for (GtkWidget *child = gtk_widget_get_first_child(self->list); child; child = gtk_widget_get_next_sibling(child)) {
    gboolean matches = record_matches(wig_download_row_get_record(WIG_DOWNLOAD_ROW(child)), self->terms);

    gtk_widget_set_visible(child, matches);
    matching += matches ? 1 : 0;
  }

  gtk_label_set_text(GTK_LABEL(self->empty), self->terms ? "No downloads match." : "No downloads yet.");

  /* An empty scroller still asks for room for its scrollbar, which would make
   * the empty popover taller than the one-row popover it turns into. */
  gtk_widget_set_visible(self->empty, matching == 0);
  gtk_widget_set_visible(self->scroller, matching > 0);
}

void wig_downloads_list_set_terms(WigDownloadsList *self, const char *terms)
{
  g_clear_pointer(&self->terms, g_free);
  if (terms && *terms)
    self->terms = g_utf8_casefold(terms, -1);

  wig_downloads_list_sync(self);
}

void wig_downloads_list_set_max_height(WigDownloadsList *self, int height)
{
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(self->scroller), height > 0);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(self->scroller), height);
}

static void wig_downloads_list_dispose(GObject *object)
{
  WigDownloadsList *self = WIG_DOWNLOADS_LIST(object);

  g_clear_pointer(&self->box, gtk_widget_unparent);
  g_clear_pointer(&self->terms, g_free);

  G_OBJECT_CLASS(wig_downloads_list_parent_class)->dispose(object);
}

static void wig_downloads_list_class_init(WigDownloadsListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_downloads_list_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-downloads-list");
}

static void wig_downloads_list_init(WigDownloadsList *self)
{
  self->manager = wig_application_get_downloads_manager(wig_application_get());

  self->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(self->box, GTK_WIDGET(self));

  self->error = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->error), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->error), TRUE);
  gtk_widget_add_css_class(self->error, "downloads-error");
  gtk_widget_set_visible(self->error, FALSE);
  gtk_box_append(GTK_BOX(self->box), self->error);

  self->empty = gtk_label_new("No downloads yet.");
  gtk_widget_add_css_class(self->empty, "downloads-empty");
  gtk_widget_add_css_class(self->empty, "dim-label");
  gtk_box_append(GTK_BOX(self->box), self->empty);

  self->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->list, "downloads-list");

  self->scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller), self->list);
  gtk_widget_set_vexpand(self->scroller, TRUE);
  gtk_box_append(GTK_BOX(self->box), self->scroller);
}

GtkWidget *wig_downloads_list_new(void)
{
  return g_object_new(WIG_TYPE_DOWNLOADS_LIST, NULL);
}
