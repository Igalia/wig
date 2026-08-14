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

#include "wig-crash-page.h"

#include <adwaita.h>

struct _WigCrashPage {
  GtkWidget parent;

  GtkWidget *status_page;
  char *details;
};

G_DEFINE_FINAL_TYPE(WigCrashPage, wig_crash_page, GTK_TYPE_WIDGET)

enum {
  RELOAD_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static const char *termination_reason_title(WebKitWebProcessTerminationReason reason)
{
  switch (reason) {
  case WEBKIT_WEB_PROCESS_CRASHED:
    return "This page crashed";
  case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:
    return "This page ran out of memory";
  case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:
    return "This page was stopped";
  }
  return "This page stopped working";
}

static const char *termination_reason_description(WebKitWebProcessTerminationReason reason)
{
  switch (reason) {
  case WEBKIT_WEB_PROCESS_CRASHED:
    return "The web process rendering this page ended unexpectedly.";
  case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:
    return "The web process rendering this page used more memory than it is allowed to and was ended.";
  case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:
    return "The web process rendering this page was ended on request.";
  }
  return "The web process rendering this page is gone.";
}

static const char *termination_reason_nick(WebKitWebProcessTerminationReason reason)
{
  switch (reason) {
  case WEBKIT_WEB_PROCESS_CRASHED:
    return "WEBKIT_WEB_PROCESS_CRASHED";
  case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:
    return "WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT";
  case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:
    return "WEBKIT_WEB_PROCESS_TERMINATED_BY_API";
  }
  return "unknown";
}

static void wig_crash_page_reload_clicked(WigCrashPage *self)
{
  g_signal_emit(self, signals[RELOAD_SIGNAL], 0);
}

static void wig_crash_page_add_detail(GtkGrid *grid, int row, const char *name, const char *value)
{
  GtkWidget *name_label = gtk_label_new(name);
  gtk_label_set_xalign(GTK_LABEL(name_label), 1.0f);
  gtk_widget_add_css_class(name_label, "dim-label");
  gtk_widget_set_valign(name_label, GTK_ALIGN_START);
  gtk_grid_attach(grid, name_label, 0, row, 1, 1);

  GtkWidget *value_label = gtk_label_new(value);
  gtk_label_set_xalign(GTK_LABEL(value_label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(value_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(value_label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_hexpand(value_label, TRUE);
  gtk_widget_add_css_class(value_label, "crash-detail-value");
  gtk_grid_attach(grid, value_label, 1, row, 1, 1);
}

static void wig_crash_page_dispose(GObject *object)
{
  WigCrashPage *self = WIG_CRASH_PAGE(object);

  g_clear_pointer(&self->status_page, gtk_widget_unparent);
  g_clear_pointer(&self->details, g_free);

  G_OBJECT_CLASS(wig_crash_page_parent_class)->dispose(object);
}

static void wig_crash_page_class_init(WigCrashPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_crash_page_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-crash-page");

  signals[RELOAD_SIGNAL] = g_signal_new("reload", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 0);
}

static void wig_crash_page_init(WigCrashPage *self)
{
  self->status_page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->status_page), "computer-fail-symbolic");
  gtk_widget_set_parent(self->status_page, GTK_WIDGET(self));
}

GtkWidget *wig_crash_page_new(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, guint tab_id)
{
  g_assert(WEBKIT_IS_WEB_VIEW(web_view));

  WigCrashPage *self = WIG_CRASH_PAGE(g_object_new(WIG_TYPE_CRASH_PAGE, NULL));

  adw_status_page_set_title(ADW_STATUS_PAGE(self->status_page), termination_reason_title(reason));
  adw_status_page_set_description(ADW_STATUS_PAGE(self->status_page), termination_reason_description(reason));

  const char *uri = webkit_web_view_get_uri(web_view);
  gboolean was_responsive = webkit_web_view_get_is_web_process_responsive(web_view);

  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree char *time_text = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");

  struct {
    const char *name;
    const char *value;
  } details[] = {
    { "Address", uri && *uri ? uri : "(none)" },
    { "Reason", termination_reason_nick(reason) },
    { "Web process", was_responsive ? "Responsive" : "Unresponsive" },
    { "Time", time_text },
  };

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_widget_add_css_class(grid, "crash-details");
  GString *plain = g_string_new(NULL);
  for (guint i = 0; i < G_N_ELEMENTS(details); i++) {
    wig_crash_page_add_detail(GTK_GRID(grid), (int)i, details[i].name, details[i].value);
    g_string_append_printf(plain, "%s: %s\n", details[i].name, details[i].value);
  }
  self->details = g_string_free_and_steal(plain);
  gtk_box_append(GTK_BOX(content), grid);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
  GtkWidget *reload_button = gtk_button_new_with_label("Reload");
  gtk_widget_add_css_class(reload_button, "suggested-action");
  gtk_widget_add_css_class(reload_button, "pill");
  g_signal_connect_swapped(reload_button, "clicked", G_CALLBACK(wig_crash_page_reload_clicked), self);
  gtk_box_append(GTK_BOX(buttons), reload_button);
  gtk_box_append(GTK_BOX(content), buttons);

  adw_status_page_set_child(ADW_STATUS_PAGE(self->status_page), content);

  return GTK_WIDGET(self);
}
