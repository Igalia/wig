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

#include "wig-external-uri-dialog.h"

#include "wig-modal-dialog.h"

#define WIG_EXTERNAL_URI_DIALOG_OPEN_DELAY_MS 500

typedef struct {
  char *uri;
  WigExternalUriDialogResponseFunc on_response;
  gpointer user_data;
} ExternalUriDialogData;

static void external_uri_dialog_data_free(ExternalUriDialogData *data)
{
  g_free(data->uri);
  g_free(data);
}

static void wig_external_uri_dialog_cancel(gpointer user_data)
{
  ExternalUriDialogData *data = user_data;

  data->on_response(WIG_EXTERNAL_URI_RESPONSE_CANCEL, data->uri, data->user_data);
}

static void wig_external_uri_dialog_cancel_clicked(GtkButton *button, ExternalUriDialogData *data)
{
  wig_external_uri_dialog_cancel(data);
}

static void wig_external_uri_dialog_open_clicked(GtkButton *button, ExternalUriDialogData *data)
{
  data->on_response(WIG_EXTERNAL_URI_RESPONSE_OPEN, data->uri, data->user_data);
}

static gboolean wig_external_uri_dialog_enable_open(gpointer user_data)
{
  gtk_widget_set_sensitive(GTK_WIDGET(user_data), TRUE);
  return G_SOURCE_REMOVE;
}

static char *external_uri_dialog_host(const char *uri)
{
  if (!uri || !*uri)
    return NULL;

  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  const char *host = parsed ? g_uri_get_host(parsed) : NULL;

  return (host && *host) ? g_strdup(host) : NULL;
}

static GtkWidget *external_uri_dialog_label(const char *text)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  return label;
}

GtkWidget *wig_external_uri_dialog_show(GtkOverlay *overlay, const char *page_uri, const char *uri,
                                        WigExternalUriDialogResponseFunc on_response, gpointer user_data)
{
  ExternalUriDialogData *data = g_new0(ExternalUriDialogData, 1);
  data->uri = g_strdup(uri);
  data->on_response = on_response;
  data->user_data = user_data;

  GtkWidget *card = NULL;
  GtkWidget *backdrop = wig_modal_dialog_present(overlay, &card, wig_external_uri_dialog_cancel, data);
  g_signal_connect_swapped(backdrop, "destroy", G_CALLBACK(external_uri_dialog_data_free), data);

  GtkWidget *title = external_uri_dialog_label("Open Link in Another App?");
  gtk_widget_add_css_class(title, "title-4");
  gtk_box_append(GTK_BOX(card), title);

  g_autofree char *host = external_uri_dialog_host(page_uri);
  g_autofree char *message_text = g_strdup_printf("%s wants to open a “%s:” link with an app on this computer.",
                                                  host ? host : "This page", g_uri_peek_scheme(uri));
  gtk_box_append(GTK_BOX(card), external_uri_dialog_label(message_text));

  GtkWidget *uri_label = gtk_label_new(uri);
  gtk_label_set_ellipsize(GTK_LABEL(uri_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(uri_label), 50);
  gtk_label_set_xalign(GTK_LABEL(uri_label), 0.0f);
  gtk_widget_set_tooltip_text(uri_label, uri);
  gtk_widget_add_css_class(uri_label, "dim-label");
  gtk_widget_add_css_class(uri_label, "monospace");
  gtk_box_append(GTK_BOX(card), uri_label);

  GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(button_row, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(card), button_row);

  GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
  g_signal_connect(cancel_button, "clicked", G_CALLBACK(wig_external_uri_dialog_cancel_clicked), data);
  gtk_box_append(GTK_BOX(button_row), cancel_button);

  GtkWidget *open_button = gtk_button_new_with_label("Open");
  gtk_widget_add_css_class(open_button, "suggested-action");
  gtk_widget_set_sensitive(open_button, FALSE);
  g_signal_connect(open_button, "clicked", G_CALLBACK(wig_external_uri_dialog_open_clicked), data);
  gtk_box_append(GTK_BOX(button_row), open_button);
  g_timeout_add_full(G_PRIORITY_DEFAULT, WIG_EXTERNAL_URI_DIALOG_OPEN_DELAY_MS, wig_external_uri_dialog_enable_open,
                     g_object_ref(open_button), g_object_unref);

  gtk_widget_grab_focus(cancel_button);

  return backdrop;
}

void wig_external_uri_dialog_dismiss(GtkOverlay *overlay, GtkWidget *dialog)
{
  wig_modal_dialog_dismiss(overlay, dialog);
}
