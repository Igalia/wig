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

#include <gdk/gdkkeysyms.h>

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
  gtk_overlay_remove_overlay(data->overlay, data->backdrop);
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

static gboolean wig_script_dialog_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                              GdkModifierType state, ScriptDialogData *data)
{
  if (keyval == GDK_KEY_Escape) {
    wig_script_dialog_respond(FALSE, data);
    return TRUE;
  }
  return FALSE;
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

  /* Backdrop: a full-size overlay child that dims the web view and blocks input. */
  GtkWidget *backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  data->backdrop = backdrop;
  gtk_widget_set_halign(backdrop, GTK_ALIGN_FILL);
  gtk_widget_set_valign(backdrop, GTK_ALIGN_FILL);
  gtk_widget_add_css_class(backdrop, "script-dialog-backdrop");
  gtk_widget_set_focusable(backdrop, TRUE);
  g_signal_connect_swapped(backdrop, "destroy", G_CALLBACK(script_dialog_data_free), data);

  GtkEventController *key_ctrl = gtk_event_controller_key_new();
  g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(wig_script_dialog_key_pressed), data);
  gtk_widget_add_controller(backdrop, key_ctrl);

  /* Expanding spacers to center the card vertically. */
  GtkWidget *spacer_top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(spacer_top, TRUE);
  gtk_box_append(GTK_BOX(backdrop), spacer_top);

  /* Card: the visible dialog box, centered horizontally. */
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(card, "card");
  gtk_widget_add_css_class(card, "script-dialog-card");
  gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(backdrop), card);

  GtkWidget *spacer_bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(spacer_bottom, TRUE);
  gtk_box_append(GTK_BOX(backdrop), spacer_bottom);

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

  gtk_overlay_add_overlay(overlay, backdrop);
  gtk_widget_grab_focus(focus_widget);
}
