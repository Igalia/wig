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

#include "wig-script-dialog.h"

#include "wig-modal-dialog.h"

typedef struct {
  WebKitScriptDialog *script_dialog;
  GtkWidget *entry;
  GtkWidget *backdrop;
  GtkOverlay *overlay;
} ScriptDialogData;

static void script_dialog_data_free(ScriptDialogData *data)
{
  webkit_script_dialog_unref(data->script_dialog);
  g_free(data);
}

static void wig_script_dialog_respond(gboolean confirmed, ScriptDialogData *data)
{
  WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(data->script_dialog);

  switch (type) {
  case WEBKIT_SCRIPT_DIALOG_CONFIRM:
  case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
    webkit_script_dialog_confirm_set_confirmed(data->script_dialog, confirmed);
    break;
  case WEBKIT_SCRIPT_DIALOG_PROMPT:
    webkit_script_dialog_prompt_set_text(data->script_dialog,
                                         confirmed ? gtk_editable_get_text(GTK_EDITABLE(data->entry)) : NULL);
    break;
  case WEBKIT_SCRIPT_DIALOG_ALERT:
    break;
  }

  webkit_script_dialog_close(data->script_dialog);
  wig_modal_dialog_dismiss(data->overlay, data->backdrop);
}

static void wig_script_dialog_confirm_clicked(GtkButton *button, ScriptDialogData *data)
{
  wig_script_dialog_respond(TRUE, data);
}

static void wig_script_dialog_cancel_clicked(GtkButton *button, ScriptDialogData *data)
{
  wig_script_dialog_respond(FALSE, data);
}

static void wig_script_dialog_entry_activated(GtkEntry *entry, ScriptDialogData *data)
{
  wig_script_dialog_respond(TRUE, data);
}

static void wig_script_dialog_cancel(gpointer user_data)
{
  wig_script_dialog_respond(FALSE, user_data);
}

void wig_script_dialog_show(GtkOverlay *overlay, WebKitScriptDialog *dialog)
{
  g_return_if_fail(GTK_IS_OVERLAY(overlay));
  g_return_if_fail(dialog != NULL);

  webkit_script_dialog_ref(dialog);
  WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(dialog);
  const char *message = webkit_script_dialog_get_message(dialog);

  ScriptDialogData *data = g_new0(ScriptDialogData, 1);
  data->script_dialog = dialog;
  data->overlay = overlay;

  GtkWidget *card = NULL;
  data->backdrop = wig_modal_dialog_present(overlay, &card, wig_script_dialog_cancel, data);
  g_signal_connect_swapped(data->backdrop, "destroy", G_CALLBACK(script_dialog_data_free), data);

  /* Message label. */
  GtkWidget *label = gtk_label_new(message);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_box_append(GTK_BOX(card), label);

  /* Text entry for prompt dialogs. */
  if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
    data->entry = gtk_entry_new();
    const char *default_text = webkit_script_dialog_prompt_get_default_text(dialog);
    if (default_text)
      gtk_editable_set_text(GTK_EDITABLE(data->entry), default_text);
    g_signal_connect(data->entry, "activate", G_CALLBACK(wig_script_dialog_entry_activated), data);
    gtk_box_append(GTK_BOX(card), data->entry);
  }

  /* Button row. */
  GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(button_row, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(card), button_row);

  GtkWidget *focus_widget;
  if (type == WEBKIT_SCRIPT_DIALOG_ALERT) {
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(close_btn, "suggested-action");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(wig_script_dialog_confirm_clicked), data);
    gtk_box_append(GTK_BOX(button_row), close_btn);
    focus_widget = close_btn;
  } else if (type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM) {
    GtkWidget *stay_btn = gtk_button_new_with_label("Stay");
    g_signal_connect(stay_btn, "clicked", G_CALLBACK(wig_script_dialog_cancel_clicked), data);
    gtk_box_append(GTK_BOX(button_row), stay_btn);
    GtkWidget *leave_btn = gtk_button_new_with_label("Leave");
    gtk_widget_add_css_class(leave_btn, "destructive-action");
    g_signal_connect(leave_btn, "clicked", G_CALLBACK(wig_script_dialog_confirm_clicked), data);
    gtk_box_append(GTK_BOX(button_row), leave_btn);
    focus_widget = stay_btn;
  } else {
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(wig_script_dialog_cancel_clicked), data);
    gtk_box_append(GTK_BOX(button_row), cancel_btn);
    GtkWidget *ok_btn = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(wig_script_dialog_confirm_clicked), data);
    gtk_box_append(GTK_BOX(button_row), ok_btn);
    focus_widget = (type == WEBKIT_SCRIPT_DIALOG_PROMPT) ? data->entry : ok_btn;
  }

  gtk_widget_grab_focus(focus_widget);
}
