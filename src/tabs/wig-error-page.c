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

#include "wig-error-page.h"

#include "wig-native-page.h"
#include "wig-page-details.h"

#include <adwaita.h>
#include <wpe/webkit.h>

struct _WigErrorPage {
  WigNativePage parent;

  GtkWidget *status_page;
  gboolean resumable;
  gboolean offline;
};

G_DEFINE_FINAL_TYPE(WigErrorPage, wig_error_page, WIG_TYPE_NATIVE_PAGE)

enum {
  RELOAD_SIGNAL,
  GO_BACK_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static const char *error_title(const GError *error)
{
  if (error->domain == G_RESOLVER_ERROR)
    return "This domain cannot be found";

  if (error->domain == G_IO_ERROR) {
    switch (error->code) {
    case G_IO_ERROR_CONNECTION_REFUSED:
      return "This site refused to connect";
    case G_IO_ERROR_TIMED_OUT:
      return "This site took too long to respond";
    case G_IO_ERROR_HOST_UNREACHABLE:
    case G_IO_ERROR_NETWORK_UNREACHABLE:
      return "This site cannot be reached";
    default:
      return "This site cannot be reached";
    }
  }

  if (error->domain == WEBKIT_NETWORK_ERROR) {
    switch (error->code) {
    case WEBKIT_NETWORK_ERROR_UNKNOWN_PROTOCOL:
      return "This address cannot be opened";
    case WEBKIT_NETWORK_ERROR_FILE_DOES_NOT_EXIST:
      return "This file does not exist";
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
    case WEBKIT_NETWORK_ERROR_HTTPS_UPGRADE_REDIRECT_LOOP:
      return "This site keeps redirecting";
    case WEBKIT_NETWORK_ERROR_HTTP_NAVIGATION_WITH_HTTPS_ONLY:
      return "This site does not support HTTPS";
#endif
    default:
      return "This site cannot be reached";
    }
  }

  if (error->domain == WEBKIT_POLICY_ERROR) {
    switch (error->code) {
    case WEBKIT_POLICY_ERROR_CANNOT_SHOW_MIME_TYPE:
      return "This content cannot be shown";
    case WEBKIT_POLICY_ERROR_CANNOT_SHOW_URI:
      return "This address cannot be opened";
    case WEBKIT_POLICY_ERROR_CANNOT_USE_RESTRICTED_PORT:
      return "This port is not allowed";
    default:
      return "This page cannot be shown";
    }
  }

  return "This page did not load";
}

static const char *error_description(const GError *error)
{
  if (error->domain == G_RESOLVER_ERROR)
    return "No server answers to that name. Check the address for typos, and check that you are online.";
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED))
    return "The server is reachable but turned the connection away, so nothing is listening on that port.";
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_HTTP_NAVIGATION_WITH_HTTPS_ONLY))
    return "wig is set to use HTTPS only, and this site offered an unencrypted connection instead.";
#endif
  if (g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_CANNOT_USE_RESTRICTED_PORT))
    return "The address names a network port that browsers refuse to connect to.";

  return "The page may be temporarily unavailable, or the address may have moved.";
}

static const char *error_icon_name(const GError *error)
{
  if (error->domain == G_RESOLVER_ERROR || error->domain == G_IO_ERROR)
    return "network-error-symbolic";
  if (error->domain == WEBKIT_NETWORK_ERROR && error->code != WEBKIT_NETWORK_ERROR_FILE_DOES_NOT_EXIST)
    return "network-error-symbolic";

  return "dialog-warning-symbolic";
}

static gboolean error_is_resumable(const char *failing_uri, const GError *error)
{
  g_autoptr(GUri) uri = g_uri_parse(failing_uri, G_URI_FLAGS_NONE, NULL);
  const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;

  if (g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0)
    return FALSE;

  if (error->domain == G_RESOLVER_ERROR)
    return TRUE;

  if (error->domain == G_IO_ERROR) {
    switch (error->code) {
    case G_IO_ERROR_HOST_NOT_FOUND:
    case G_IO_ERROR_HOST_UNREACHABLE:
    case G_IO_ERROR_NETWORK_UNREACHABLE:
    case G_IO_ERROR_CONNECTION_REFUSED:
    case G_IO_ERROR_CONNECTION_CLOSED:
    case G_IO_ERROR_NOT_CONNECTED:
    case G_IO_ERROR_TIMED_OUT:
    case G_IO_ERROR_PARTIAL_INPUT:
      return TRUE;
    default:
      return FALSE;
    }
  }

  /* Everything from a refused connection to one that dropped mid-response
   * reaches us as TRANSPORT or FAILED, with the specifics only in the message. */
  if (error->domain == WEBKIT_NETWORK_ERROR)
    return error->code == WEBKIT_NETWORK_ERROR_TRANSPORT || error->code == WEBKIT_NETWORK_ERROR_FAILED;

  return FALSE;
}

static void wig_error_page_reload_clicked(WigErrorPage *self)
{
  g_signal_emit(self, signals[RELOAD_SIGNAL], 0);
}

static void wig_error_page_go_back_clicked(WigErrorPage *self)
{
  g_signal_emit(self, signals[GO_BACK_SIGNAL], 0);
}

static void wig_error_page_dispose(GObject *object)
{
  WigErrorPage *self = WIG_ERROR_PAGE(object);

  g_clear_pointer(&self->status_page, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_error_page_parent_class)->dispose(object);
}

static void wig_error_page_class_init(WigErrorPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_error_page_dispose;

  gtk_widget_class_set_css_name(widget_class, "wig-error-page");

  signals[RELOAD_SIGNAL] = g_signal_new("reload", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 0);
  signals[GO_BACK_SIGNAL] = g_signal_new("go-back", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void wig_error_page_init(WigErrorPage *self)
{
  self->status_page = adw_status_page_new();
  gtk_widget_set_parent(self->status_page, GTK_WIDGET(self));
}

GtkWidget *wig_error_page_new(const char *failing_uri, const GError *error, gboolean can_go_back, gboolean offline)
{
  g_assert(failing_uri != NULL);
  g_assert(error != NULL);

  WigErrorPage *self = WIG_ERROR_PAGE(g_object_new(WIG_TYPE_ERROR_PAGE, "uri", failing_uri, NULL));

  self->resumable = error_is_resumable(failing_uri, error);
  self->offline = offline && self->resumable;

  if (self->offline) {
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->status_page), "network-offline-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(self->status_page), "You are offline");
    adw_status_page_set_description(ADW_STATUS_PAGE(self->status_page),
                                    "This page will reload by itself once you are back online.");
  } else {
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->status_page), error_icon_name(error));
    adw_status_page_set_title(ADW_STATUS_PAGE(self->status_page), error_title(error));
    adw_status_page_set_description(ADW_STATUS_PAGE(self->status_page), error_description(error));
  }

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);

  if (!self->offline) {
    g_autofree char *code = g_strdup_printf("%s %d", g_quark_to_string(error->domain), error->code);

    struct {
      const char *name;
      const char *value;
    } details[] = {
      { "Address", failing_uri },
      { "Details", error->message && *error->message ? error->message : "(none)" },
      { "Code", code },
    };

    GtkWidget *grid = wig_page_details_new();
    for (guint i = 0; i < G_N_ELEMENTS(details); i++)
      wig_page_details_add(grid, (int)i, details[i].name, details[i].value);
    gtk_box_append(GTK_BOX(content), grid);
  }

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);

  GtkWidget *reload_button = gtk_button_new_with_label("Reload");
  gtk_widget_add_css_class(reload_button, "suggested-action");
  gtk_widget_add_css_class(reload_button, "pill");
  g_signal_connect_swapped(reload_button, "clicked", G_CALLBACK(wig_error_page_reload_clicked), self);
  gtk_box_append(GTK_BOX(buttons), reload_button);

  if (can_go_back) {
    GtkWidget *back_button = gtk_button_new_with_label("Go Back");
    gtk_widget_add_css_class(back_button, "pill");
    g_signal_connect_swapped(back_button, "clicked", G_CALLBACK(wig_error_page_go_back_clicked), self);
    gtk_box_append(GTK_BOX(buttons), back_button);
  }

  gtk_box_append(GTK_BOX(content), buttons);

  adw_status_page_set_child(ADW_STATUS_PAGE(self->status_page), content);

  return GTK_WIDGET(self);
}

const char *wig_error_page_get_uri(WigErrorPage *self)
{
  g_assert(WIG_IS_ERROR_PAGE(self));

  return wig_native_page_get_uri(WIG_NATIVE_PAGE(self));
}

gboolean wig_error_page_get_resumable(WigErrorPage *self)
{
  return self->resumable;
}
