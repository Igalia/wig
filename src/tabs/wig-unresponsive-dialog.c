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

#include "wig-unresponsive-dialog.h"

#include "wig-modal-dialog.h"

typedef struct {
  WigUnresponsiveDialogResponseFunc on_response;
  gpointer user_data;
} UnresponsiveDialogData;

static void unresponsive_dialog_data_free(UnresponsiveDialogData *data)
{
  g_free(data);
}

/* Waiting is just letting the page be: the tab keeps watching, so the dialog
 * comes back on the next click if the page is still stuck. */
static void wig_unresponsive_dialog_wait(gpointer user_data)
{
  UnresponsiveDialogData *data = user_data;

  data->on_response(WIG_UNRESPONSIVE_RESPONSE_WAIT, data->user_data);
}

static void wig_unresponsive_dialog_wait_clicked(GtkButton *button, UnresponsiveDialogData *data)
{
  wig_unresponsive_dialog_wait(data);
}

static void wig_unresponsive_dialog_close_clicked(GtkButton *button, UnresponsiveDialogData *data)
{
  data->on_response(WIG_UNRESPONSIVE_RESPONSE_CLOSE, data->user_data);
}

static char *unresponsive_dialog_host(const char *uri)
{
  if (!uri || !*uri)
    return NULL;

  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  const char *host = parsed ? g_uri_get_host(parsed) : NULL;

  return (host && *host) ? g_strdup(host) : NULL;
}

GtkWidget *wig_unresponsive_dialog_show(GtkOverlay *overlay, const char *uri,
                                        WigUnresponsiveDialogResponseFunc on_response, gpointer user_data)
{
  UnresponsiveDialogData *data = g_new0(UnresponsiveDialogData, 1);
  data->on_response = on_response;
  data->user_data = user_data;

  GtkWidget *card = NULL;
  GtkWidget *backdrop = wig_modal_dialog_present(overlay, &card, wig_unresponsive_dialog_wait, data);
  g_signal_connect_swapped(backdrop, "destroy", G_CALLBACK(unresponsive_dialog_data_free), data);

  g_autofree char *host = unresponsive_dialog_host(uri);
  g_autofree char *title_text = g_strdup_printf("%s is not responding", host ? host : "This page");
  GtkWidget *title = gtk_label_new(title_text);
  gtk_label_set_wrap(GTK_LABEL(title), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(title), 50);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "title-4");
  gtk_box_append(GTK_BOX(card), title);

  GtkWidget *message = gtk_label_new("You can wait for it to catch up, or close it and lose whatever it was doing.");
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(message), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(message), 50);
  gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
  gtk_box_append(GTK_BOX(card), message);

  GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(button_row, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(card), button_row);

  GtkWidget *close_button = gtk_button_new_with_label("Close Page");
  gtk_widget_add_css_class(close_button, "destructive-action");
  g_signal_connect(close_button, "clicked", G_CALLBACK(wig_unresponsive_dialog_close_clicked), data);
  gtk_box_append(GTK_BOX(button_row), close_button);

  GtkWidget *wait_button = gtk_button_new_with_label("Wait");
  gtk_widget_add_css_class(wait_button, "suggested-action");
  g_signal_connect(wait_button, "clicked", G_CALLBACK(wig_unresponsive_dialog_wait_clicked), data);
  gtk_box_append(GTK_BOX(button_row), wait_button);

  gtk_widget_grab_focus(wait_button);

  return backdrop;
}

void wig_unresponsive_dialog_dismiss(GtkOverlay *overlay, GtkWidget *dialog)
{
  g_assert(GTK_IS_OVERLAY(overlay));
  g_assert(GTK_IS_WIDGET(dialog));

  wig_modal_dialog_dismiss(overlay, dialog);
}
