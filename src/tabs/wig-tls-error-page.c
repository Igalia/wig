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

#include "wig-tls-error-page.h"

#include "wig-native-page.h"
#include "wig-page-details.h"

#include <adwaita.h>

struct _WigTlsErrorPage {
  WigNativePage parent;

  GtkWidget *status_page;
  char *host;
  GTlsCertificate *certificate;
};

G_DEFINE_FINAL_TYPE(WigTlsErrorPage, wig_tls_error_page, WIG_TYPE_NATIVE_PAGE)

enum {
  PROCEED_SIGNAL,
  GO_BACK_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static const char *tls_errors_title(GTlsCertificateFlags errors)
{
  if (errors & G_TLS_CERTIFICATE_BAD_IDENTITY)
    return "This certificate is for a different site";
  if (errors & G_TLS_CERTIFICATE_REVOKED)
    return "This certificate was revoked";
  if (errors & G_TLS_CERTIFICATE_UNKNOWN_CA)
    return "This certificate is not trusted";
  if (errors & (G_TLS_CERTIFICATE_EXPIRED | G_TLS_CERTIFICATE_NOT_ACTIVATED))
    return "This certificate is not valid right now";
  return "This connection is not private";
}

static char *tls_errors_list(GTlsCertificateFlags errors)
{
  static const struct {
    GTlsCertificateFlags flag;
    const char *text;
  } known[] = {
    { G_TLS_CERTIFICATE_UNKNOWN_CA, "Issued by an unknown authority" },
    { G_TLS_CERTIFICATE_BAD_IDENTITY, "Does not match the site name" },
    { G_TLS_CERTIFICATE_NOT_ACTIVATED, "Not valid yet" },
    { G_TLS_CERTIFICATE_EXPIRED, "Expired" },
    { G_TLS_CERTIFICATE_REVOKED, "Revoked" },
    { G_TLS_CERTIFICATE_INSECURE, "Uses an insecure algorithm" },
    { G_TLS_CERTIFICATE_GENERIC_ERROR, "Failed verification" },
  };

  g_autoptr(GString) text = g_string_new(NULL);
  for (guint i = 0; i < G_N_ELEMENTS(known); i++) {
    if (!(errors & known[i].flag))
      continue;
    if (text->len)
      g_string_append(text, "\n");
    g_string_append(text, known[i].text);
  }

  if (!text->len)
    g_string_append(text, "Failed verification");

  return g_strdup(text->str);
}

static void wig_tls_error_page_proceed_clicked(WigTlsErrorPage *self)
{
  g_signal_emit(self, signals[PROCEED_SIGNAL], 0);
}

static void wig_tls_error_page_go_back_clicked(WigTlsErrorPage *self)
{
  g_signal_emit(self, signals[GO_BACK_SIGNAL], 0);
}

static void wig_tls_error_page_dispose(GObject *object)
{
  WigTlsErrorPage *self = WIG_TLS_ERROR_PAGE(object);

  g_clear_pointer(&self->status_page, gtk_widget_unparent);
  g_clear_pointer(&self->host, g_free);
  g_clear_object(&self->certificate);

  G_OBJECT_CLASS(wig_tls_error_page_parent_class)->dispose(object);
}

static void wig_tls_error_page_class_init(WigTlsErrorPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_tls_error_page_dispose;

  gtk_widget_class_set_css_name(widget_class, "wig-tls-error-page");

  signals[PROCEED_SIGNAL] = g_signal_new("proceed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
  signals[GO_BACK_SIGNAL] = g_signal_new("go-back", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void wig_tls_error_page_init(WigTlsErrorPage *self)
{
  self->status_page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->status_page), "channel-insecure-symbolic");
  gtk_widget_set_parent(self->status_page, GTK_WIDGET(self));
}

GtkWidget *wig_tls_error_page_new(const char *failing_uri, GTlsCertificate *certificate, GTlsCertificateFlags errors,
                                  gboolean can_go_back)
{
  g_assert(failing_uri != NULL);
  g_assert(G_IS_TLS_CERTIFICATE(certificate));

  WigTlsErrorPage *self = WIG_TLS_ERROR_PAGE(g_object_new(WIG_TYPE_TLS_ERROR_PAGE, "uri", failing_uri, NULL));
  self->certificate = g_object_ref(certificate);

  g_autoptr(GUri) parsed = g_uri_parse(failing_uri, G_URI_FLAGS_NONE, NULL);
  const char *host = parsed ? g_uri_get_host(parsed) : NULL;
  self->host = g_strdup(host && *host ? host : "");

  g_autofree char *description = g_strdup_printf(
      "The identity of %s could not be verified, so anything sent to or from it could be read by "
      "someone else.",
      *self->host ? self->host : "this site");

  adw_status_page_set_title(ADW_STATUS_PAGE(self->status_page), tls_errors_title(errors));
  adw_status_page_set_description(ADW_STATUS_PAGE(self->status_page), description);

  g_autofree char *problems = tls_errors_list(errors);
  g_autofree char *subject = g_tls_certificate_get_subject_name(certificate);
  g_autofree char *issuer = g_tls_certificate_get_issuer_name(certificate);
  g_autoptr(GDateTime) expiry = g_tls_certificate_get_not_valid_after(certificate);
  g_autofree char *expiry_text = expiry ? g_date_time_format(expiry, "%Y-%m-%d %H:%M:%S") : NULL;

  struct {
    const char *name;
    const char *value;
  } details[] = {
    { "Address", failing_uri },
    { "Problems", problems },
    { "Issued to", subject ? subject : "(unknown)" },
    { "Issued by", issuer ? issuer : "(unknown)" },
    { "Expires", expiry_text ? expiry_text : "(unknown)" },
  };

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);

  GtkWidget *grid = wig_page_details_new();
  for (guint i = 0; i < G_N_ELEMENTS(details); i++)
    wig_page_details_add(grid, (int)i, details[i].name, details[i].value);
  gtk_box_append(GTK_BOX(content), grid);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);

  if (can_go_back) {
    GtkWidget *back_button = gtk_button_new_with_label("Go Back");
    gtk_widget_add_css_class(back_button, "suggested-action");
    gtk_widget_add_css_class(back_button, "pill");
    g_signal_connect_swapped(back_button, "clicked", G_CALLBACK(wig_tls_error_page_go_back_clicked), self);
    gtk_box_append(GTK_BOX(buttons), back_button);
  }

  GtkWidget *proceed_button = gtk_button_new_with_label("Continue Anyway");
  gtk_widget_add_css_class(proceed_button, "destructive-action");
  gtk_widget_add_css_class(proceed_button, "pill");
  gtk_widget_set_tooltip_text(proceed_button, "Trust this certificate for this site until wig is closed");
  g_signal_connect_swapped(proceed_button, "clicked", G_CALLBACK(wig_tls_error_page_proceed_clicked), self);
  gtk_box_append(GTK_BOX(buttons), proceed_button);

  gtk_box_append(GTK_BOX(content), buttons);

  adw_status_page_set_child(ADW_STATUS_PAGE(self->status_page), content);

  return GTK_WIDGET(self);
}

const char *wig_tls_error_page_get_uri(WigTlsErrorPage *self)
{
  g_assert(WIG_IS_TLS_ERROR_PAGE(self));

  return wig_native_page_get_uri(WIG_NATIVE_PAGE(self));
}

const char *wig_tls_error_page_get_host(WigTlsErrorPage *self)
{
  g_assert(WIG_IS_TLS_ERROR_PAGE(self));

  return self->host;
}

GTlsCertificate *wig_tls_error_page_get_certificate(WigTlsErrorPage *self)
{
  g_assert(WIG_IS_TLS_ERROR_PAGE(self));

  return self->certificate;
}
