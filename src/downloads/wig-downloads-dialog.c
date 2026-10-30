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

#include "wig-downloads-dialog.h"

#include "wig-application.h"
#include "wig-downloads-list.h"
#include "wig-web-app-filter.h"

struct _WigDownloadsDialog {
  AdwDialog parent;

  WigDownloadsManager *manager;

  GtkWidget *entry;
  GtkWidget *filter;
  GtkWidget *clear_button;
  GtkWidget *list;
};

G_DEFINE_FINAL_TYPE(WigDownloadsDialog, wig_downloads_dialog, ADW_TYPE_DIALOG)

static const char *wig_downloads_dialog_web_app(WigDownloadsDialog *self)
{
  return wig_web_app_filter_get_web_app(WIG_WEB_APP_FILTER(self->filter));
}

static void wig_downloads_dialog_update_clear_button(WigDownloadsDialog *self)
{
  gtk_widget_set_sensitive(self->clear_button,
                           wig_downloads_manager_has_finished(self->manager, wig_downloads_dialog_web_app(self)));
}

static void wig_downloads_dialog_sync(WigDownloadsDialog *self)
{
  wig_downloads_list_sync(WIG_DOWNLOADS_LIST(self->list));
  wig_downloads_dialog_update_clear_button(self);
}

static void wig_downloads_dialog_web_app_changed(WigDownloadsDialog *self, GParamSpec *pspec, WigWebAppFilter *filter)
{
  wig_downloads_list_set_web_app(WIG_DOWNLOADS_LIST(self->list), wig_downloads_dialog_web_app(self));
  wig_downloads_dialog_update_clear_button(self);
}

static void wig_downloads_dialog_download_added(WigDownloadsDialog *self, const char *web_app,
                                                WigDownloadsManager *manager)
{
  wig_downloads_dialog_sync(self);
}

static void wig_downloads_dialog_search_changed(WigDownloadsDialog *self, GtkSearchEntry *entry)
{
  wig_downloads_list_set_terms(WIG_DOWNLOADS_LIST(self->list), gtk_editable_get_text(GTK_EDITABLE(entry)));
  wig_downloads_dialog_update_clear_button(self);
}

static void wig_downloads_dialog_clear_clicked(WigDownloadsDialog *self)
{
  wig_downloads_list_clear_error(WIG_DOWNLOADS_LIST(self->list));
  wig_downloads_manager_clear_finished(self->manager, wig_downloads_dialog_web_app(self));
}

static gboolean wig_downloads_dialog_start_search(GtkWidget *widget, GVariant *args, gpointer user_data)
{
  WigDownloadsDialog *self = WIG_DOWNLOADS_DIALOG(widget);

  gtk_widget_grab_focus(self->entry);
  return TRUE;
}

static void wig_downloads_dialog_class_init(WigDownloadsDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  gtk_widget_class_add_binding(widget_class, GDK_KEY_f, GDK_CONTROL_MASK, wig_downloads_dialog_start_search, NULL);
}

static void wig_downloads_dialog_init(WigDownloadsDialog *self)
{
  self->manager = wig_application_get_downloads_manager(wig_application_get());

  self->entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->entry, TRUE);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->entry), "Search Downloads");
  g_signal_connect_object(self->entry, "search-changed", G_CALLBACK(wig_downloads_dialog_search_changed), self,
                          G_CONNECT_SWAPPED);

  self->clear_button = gtk_button_new_with_label("Clear Completed");
  gtk_widget_add_css_class(self->clear_button, "flat");
  g_signal_connect_object(self->clear_button, "clicked", G_CALLBACK(wig_downloads_dialog_clear_clicked), self,
                          G_CONNECT_SWAPPED);

  self->filter = wig_web_app_filter_new();
  gtk_widget_set_tooltip_text(self->filter, "Show Downloads From");
  g_signal_connect_object(self->filter, "notify::web-app", G_CALLBACK(wig_downloads_dialog_web_app_changed), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(bar), self->entry);
  gtk_box_append(GTK_BOX(bar), self->filter);
  gtk_box_append(GTK_BOX(bar), self->clear_button);
  gtk_widget_set_margin_top(bar, 6);
  gtk_widget_set_margin_bottom(bar, 6);
  gtk_widget_set_margin_start(bar, 12);
  gtk_widget_set_margin_end(bar, 12);

  self->list = wig_downloads_list_new();
  gtk_widget_set_vexpand(self->list, TRUE);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), bar);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->list);

  g_signal_connect_object(self->manager, "changed", G_CALLBACK(wig_downloads_dialog_sync), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->manager, "added", G_CALLBACK(wig_downloads_dialog_download_added), self,
                          G_CONNECT_SWAPPED);

  adw_dialog_set_title(ADW_DIALOG(self), "Downloads");
  adw_dialog_set_content_width(ADW_DIALOG(self), 720);
  adw_dialog_set_content_height(ADW_DIALOG(self), 560);
  adw_dialog_set_child(ADW_DIALOG(self), toolbar);
  wig_downloads_dialog_sync(self);
}

AdwDialog *wig_downloads_dialog_new(const char *web_app)
{
  WigDownloadsDialog *self = g_object_new(WIG_TYPE_DOWNLOADS_DIALOG, NULL);

  wig_web_app_filter_set_web_app(WIG_WEB_APP_FILTER(self->filter), web_app);
  return ADW_DIALOG(self);
}
