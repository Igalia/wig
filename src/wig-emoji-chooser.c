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

#include "wig-emoji-chooser.h"

#include "wpe-view-gtk.h"

/* The caret rectangle WebKit works out only ever reaches the input method
 * context, which hands it to the platform as a setter with nothing to read it
 * back from, so the page is asked where its caret is. A text field keeps its
 * selection to itself, and the field is the thing the emoji lands in anyway. */
static const char CARET_RECT_SCRIPT[] = "(() => {"
                                        "  const active = document.activeElement;"
                                        "  const selection = getSelection();"
                                        "  let rect = null;"
                                        "  if (active && active.isContentEditable && selection.rangeCount)"
                                        "    rect = selection.getRangeAt(0).getBoundingClientRect();"
                                        "  if (active && (!rect || (!rect.width && !rect.height)))"
                                        "    rect = active.getBoundingClientRect();"
                                        "  return rect ? [rect.x, rect.y, rect.width, rect.height] : null;"
                                        "})()";

static int rect_value(JSCValue *rect, guint index, double zoom)
{
  g_autoptr(JSCValue) value = jsc_value_object_get_property_at_index(rect, index);
  return (int)(jsc_value_to_double(value) * zoom);
}

void wig_emoji_chooser_query_caret(WebKitWebView *web_view, GAsyncReadyCallback callback, gpointer user_data)
{
  webkit_web_view_evaluate_javascript(web_view, CARET_RECT_SCRIPT, -1, NULL, NULL, NULL, callback, user_data);
}

gboolean wig_emoji_chooser_query_caret_finish(WebKitWebView *web_view, GAsyncResult *result, GdkRectangle *caret)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(JSCValue) rect = webkit_web_view_evaluate_javascript_finish(web_view, result, &error);
  if (!rect || !jsc_value_is_array(rect)) {
    g_debug("emoji: no caret rectangle%s%s", error ? ": " : "", error ? error->message : "");
    return FALSE;
  }

  /* The page answers in CSS pixels, which the view scales by the zoom level to
   * reach the widget the popover is placed in. */
  double zoom = webkit_web_view_get_zoom_level(web_view);
  caret->x = rect_value(rect, 0, zoom);
  caret->y = rect_value(rect, 1, zoom);
  caret->width = rect_value(rect, 2, zoom);
  caret->height = rect_value(rect, 3, zoom);

  g_debug("emoji: caret at %d,%d %dx%d", caret->x, caret->y, caret->width, caret->height);
  return TRUE;
}

static void wig_emoji_chooser_picked(WebKitWebView *web_view, const char *text)
{
  g_debug("emoji: inserting '%s'", text);
  webkit_web_view_execute_editing_command_with_argument(web_view, "InsertText", text);
}

/* Shows the emoji chooser at `caret` within the widget the page is drawn into,
 * and inserts what is picked at the caret. A NULL `caret` leaves the chooser
 * over the page as a whole. The popover stays until it is unparented, so the
 * caller keeps it and drops it when it closes. */
GtkWidget *wig_emoji_chooser_show(WebKitWebView *web_view, const GdkRectangle *caret)
{
  GtkWidget *parent = wpe_view_gtk_get_widget(WPE_VIEW_GTK(webkit_web_view_get_wpe_view(web_view)));
  GtkWidget *chooser = gtk_emoji_chooser_new();

  g_signal_connect_object(chooser, "emoji-picked", G_CALLBACK(wig_emoji_chooser_picked), web_view, G_CONNECT_SWAPPED);

  gtk_widget_set_parent(chooser, parent);
  if (caret)
    gtk_popover_set_pointing_to(GTK_POPOVER(chooser), caret);
  gtk_popover_popup(GTK_POPOVER(chooser));

  return chooser;
}
