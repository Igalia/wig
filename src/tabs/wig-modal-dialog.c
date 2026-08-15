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

#include "wig-modal-dialog.h"

#include <gdk/gdkkeysyms.h>

typedef struct {
  WigModalDialogCancelFunc on_cancel;
  gpointer user_data;
} EscapeData;

static gboolean modal_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state,
                                  EscapeData *data)
{
  if (keyval == GDK_KEY_Escape) {
    data->on_cancel(data->user_data);
    return TRUE;
  }
  return FALSE;
}

static void escape_data_free(gpointer data, GClosure *closure)
{
  g_free(data);
}

/* Presents a dimmed, input-blocking backdrop with a centered card inside
 * `overlay`. The caller packs its content into the card returned via
 * `out_card` (which carries the "card" and "modal-card" style classes) and is
 * responsible for grabbing focus on the appropriate widget. Pressing Escape
 * invokes `on_cancel`. The returned backdrop is owned by the overlay; dismiss
 * it with wig_modal_dialog_dismiss(). Connect to the backdrop's "destroy"
 * signal to release any per-dialog data. */
GtkWidget *wig_modal_dialog_present(GtkOverlay *overlay, GtkWidget **out_card, WigModalDialogCancelFunc on_cancel,
                                    gpointer user_data)
{
  g_return_val_if_fail(GTK_IS_OVERLAY(overlay), NULL);
  g_return_val_if_fail(out_card != NULL, NULL);
  g_return_val_if_fail(on_cancel != NULL, NULL);

  /* Backdrop: a full-size overlay child that dims the web view and blocks input. */
  GtkWidget *backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(backdrop, GTK_ALIGN_FILL);
  gtk_widget_set_valign(backdrop, GTK_ALIGN_FILL);
  gtk_widget_add_css_class(backdrop, "modal-backdrop");
  gtk_widget_set_focusable(backdrop, TRUE);

  EscapeData *escape = g_new0(EscapeData, 1);
  escape->on_cancel = on_cancel;
  escape->user_data = user_data;
  GtkEventController *key_ctrl = gtk_event_controller_key_new();
  g_signal_connect_data(key_ctrl, "key-pressed", G_CALLBACK(modal_key_pressed), escape, escape_data_free, 0);
  gtk_widget_add_controller(backdrop, key_ctrl);

  /* Expanding spacers to center the card vertically. */
  GtkWidget *spacer_top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(spacer_top, TRUE);
  gtk_box_append(GTK_BOX(backdrop), spacer_top);

  /* Card: the visible dialog box, centered horizontally. */
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(card, "card");
  gtk_widget_add_css_class(card, "modal-card");
  gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(backdrop), card);

  GtkWidget *spacer_bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(spacer_bottom, TRUE);
  gtk_box_append(GTK_BOX(backdrop), spacer_bottom);

  gtk_overlay_add_overlay(overlay, backdrop);
  *out_card = card;
  return backdrop;
}

/* The dialog is focused for as long as it is up, and a window keeps pointing at
 * whatever had the focus, so the focus is handed back to the page before the
 * widget holding it is destroyed. */
static void wig_modal_dialog_release_focus(GtkOverlay *overlay, GtkWidget *backdrop)
{
  GtkRoot *root = gtk_widget_get_root(backdrop);
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;

  if (!focus || (focus != backdrop && !gtk_widget_is_ancestor(focus, backdrop)))
    return;

  GtkWidget *child = gtk_overlay_get_child(overlay);
  if (!child || !gtk_widget_grab_focus(child))
    gtk_root_set_focus(root, NULL);
}

void wig_modal_dialog_dismiss(GtkOverlay *overlay, GtkWidget *backdrop)
{
  g_return_if_fail(GTK_IS_OVERLAY(overlay));
  g_return_if_fail(GTK_IS_WIDGET(backdrop));

  wig_modal_dialog_release_focus(overlay, backdrop);
  gtk_overlay_remove_overlay(overlay, backdrop);
}
