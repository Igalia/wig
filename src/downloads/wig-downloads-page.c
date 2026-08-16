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

#include "wig-downloads-page.h"

#include "wig-application.h"
#include "wig-downloads-list.h"

#include <adwaita.h>

/* Rows say a name, where it came from and how far along it is, so they are given
 * more room than a page of prose would take. */
#define DOWNLOADS_PAGE_WIDTH 900
#define DOWNLOADS_PAGE_TIGHTENING 600

struct _WigDownloadsPage {
  WigNativePage parent;

  WigDownloadsManager *manager;

  GtkWidget *toolbar;
  GtkWidget *entry;
  GtkWidget *clear_button;
  GtkWidget *list;
};

G_DEFINE_FINAL_TYPE(WigDownloadsPage, wig_downloads_page, WIG_TYPE_NATIVE_PAGE)

gboolean uri_is_downloads_page(const char *uri)
{
  g_autoptr(GUri) parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;

  return parsed && g_strcmp0(g_uri_get_scheme(parsed), "wig") == 0 && g_str_equal(g_uri_get_path(parsed), "downloads");
}

static void wig_downloads_page_sync(WigDownloadsPage *self)
{
  wig_downloads_list_sync(WIG_DOWNLOADS_LIST(self->list));
  gtk_widget_set_sensitive(self->clear_button, wig_downloads_manager_has_finished(self->manager));
}

static void wig_downloads_page_search_changed(WigDownloadsPage *self, GtkSearchEntry *entry)
{
  wig_downloads_list_set_terms(WIG_DOWNLOADS_LIST(self->list), gtk_editable_get_text(GTK_EDITABLE(entry)));
  gtk_widget_set_sensitive(self->clear_button, wig_downloads_manager_has_finished(self->manager));
}

static void wig_downloads_page_clear_clicked(WigDownloadsPage *self)
{
  wig_downloads_list_clear_error(WIG_DOWNLOADS_LIST(self->list));
  wig_downloads_manager_clear_finished(self->manager);
}

static gboolean wig_downloads_page_start_search(WigNativePage *page)
{
  WigDownloadsPage *self = WIG_DOWNLOADS_PAGE(page);

  gtk_widget_grab_focus(self->entry);
  return TRUE;
}

static void wig_downloads_page_dispose(GObject *object)
{
  WigDownloadsPage *self = WIG_DOWNLOADS_PAGE(object);

  g_clear_pointer(&self->toolbar, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_downloads_page_parent_class)->dispose(object);
}

static void wig_downloads_page_class_init(WigDownloadsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_downloads_page_dispose;
  WIG_NATIVE_PAGE_CLASS(klass)->start_search = wig_downloads_page_start_search;

  gtk_widget_class_set_css_name(widget_class, "wig-downloads-page");
}

static void wig_downloads_page_init(WigDownloadsPage *self)
{
  self->manager = wig_application_get_downloads_manager(wig_application_get());

  self->entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->entry, TRUE);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->entry), "Search Downloads");
  g_signal_connect_object(self->entry, "search-changed", G_CALLBACK(wig_downloads_page_search_changed), self,
                          G_CONNECT_SWAPPED);

  self->clear_button = gtk_button_new_with_label("Clear Completed");
  gtk_widget_add_css_class(self->clear_button, "flat");
  g_signal_connect_object(self->clear_button, "clicked", G_CALLBACK(wig_downloads_page_clear_clicked), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(bar), self->entry);
  gtk_box_append(GTK_BOX(bar), self->clear_button);

  GtkWidget *bar_clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(bar_clamp), bar);
  adw_clamp_set_maximum_size(ADW_CLAMP(bar_clamp), DOWNLOADS_PAGE_WIDTH);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(bar_clamp), DOWNLOADS_PAGE_TIGHTENING);
  gtk_widget_set_margin_top(bar_clamp, 6);
  gtk_widget_set_margin_bottom(bar_clamp, 6);
  gtk_widget_set_margin_start(bar_clamp, 12);
  gtk_widget_set_margin_end(bar_clamp, 12);

  /* The same list the button shows, given the whole page rather than as much of
   * a popover as it fits in. */
  self->list = wig_downloads_list_new();
  gtk_widget_set_vexpand(self->list, TRUE);

  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), self->list);
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), DOWNLOADS_PAGE_WIDTH);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), DOWNLOADS_PAGE_TIGHTENING);

  self->toolbar = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(self->toolbar), ADW_TOOLBAR_FLAT);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(self->toolbar), bar_clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(self->toolbar), clamp);
  gtk_widget_set_parent(self->toolbar, GTK_WIDGET(self));

  g_signal_connect_object(self->manager, "changed", G_CALLBACK(wig_downloads_page_sync), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->manager, "added", G_CALLBACK(wig_downloads_page_sync), self, G_CONNECT_SWAPPED);

  wig_native_page_set_title(WIG_NATIVE_PAGE(self), WIG_DOWNLOADS_PAGE_TITLE);
  wig_downloads_page_sync(self);
}

GtkWidget *wig_downloads_page_new(const char *uri)
{
  return g_object_new(WIG_TYPE_DOWNLOADS_PAGE, "uri", uri, NULL);
}
