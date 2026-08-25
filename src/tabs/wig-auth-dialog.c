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

#include "wig-auth-dialog.h"

#include "wig-modal-dialog.h"

typedef struct {
  WebKitAuthenticationRequest *request;
  GtkWidget *username_entry;
  GtkWidget *password_entry;
  GtkWidget *remember_check;
  WebKitCredentialPersistence remember_persistence;
  GtkWidget *backdrop;
  GtkOverlay *overlay;
  gboolean responded;
} AuthDialogData;

/* Whether a scheme is satisfied by a username and password. Certificate-based
 * schemes (client certificate, server trust) cannot be, so we reject those. */
static gboolean scheme_uses_password(WebKitAuthenticationScheme scheme)
{
  switch (scheme) {
  case WEBKIT_AUTHENTICATION_SCHEME_DEFAULT:
  case WEBKIT_AUTHENTICATION_SCHEME_HTTP_BASIC:
  case WEBKIT_AUTHENTICATION_SCHEME_HTTP_DIGEST:
  case WEBKIT_AUTHENTICATION_SCHEME_HTML_FORM:
  case WEBKIT_AUTHENTICATION_SCHEME_NTLM:
  case WEBKIT_AUTHENTICATION_SCHEME_NEGOTIATE:
    return TRUE;
  default:
    return FALSE;
  }
}

/* If the dialog is torn down without the user answering (e.g. the tab is
 * closed), cancel the request so the pending load does not hang. */
static void auth_dialog_data_free(AuthDialogData *data)
{
  if (!data->responded)
    webkit_authentication_request_cancel(data->request);
  g_clear_object(&data->request);
  g_free(data);
}

static void wig_auth_dialog_respond(gboolean authenticate, AuthDialogData *data)
{
  if (data->responded)
    return;
  data->responded = TRUE;

  if (authenticate) {
    const char *username = gtk_editable_get_text(GTK_EDITABLE(data->username_entry));
    const char *password = gtk_editable_get_text(GTK_EDITABLE(data->password_entry));
    gboolean remember = gtk_check_button_get_active(GTK_CHECK_BUTTON(data->remember_check));
    WebKitCredentialPersistence persistence = remember ? data->remember_persistence
                                                       : WEBKIT_CREDENTIAL_PERSISTENCE_NONE;
    WebKitCredential *credential = webkit_credential_new(username, password, persistence);
    webkit_authentication_request_authenticate(data->request, credential);
    webkit_credential_free(credential);
  } else {
    webkit_authentication_request_cancel(data->request);
  }

  wig_modal_dialog_dismiss(data->overlay, data->backdrop);
}

static void wig_auth_dialog_login_clicked(GtkButton *button, AuthDialogData *data)
{
  wig_auth_dialog_respond(TRUE, data);
}

static void wig_auth_dialog_cancel_clicked(GtkButton *button, AuthDialogData *data)
{
  wig_auth_dialog_respond(FALSE, data);
}

static void wig_auth_dialog_cancel(gpointer user_data)
{
  wig_auth_dialog_respond(FALSE, user_data);
}

static void wig_auth_dialog_entry_activated(GtkEntry *entry, AuthDialogData *data)
{
  wig_auth_dialog_respond(TRUE, data);
}

static char *wig_auth_dialog_describe(WebKitAuthenticationRequest *request)
{
  const char *host = webkit_authentication_request_get_host(request);
  guint port = webkit_authentication_request_get_port(request);
  const char *realm = webkit_authentication_request_get_realm(request);
  const char *kind = webkit_authentication_request_is_for_proxy(request) ? "The proxy" : "The site";

  g_autofree char *authority = port ? g_strdup_printf("%s:%u", host ? host : "", port) : g_strdup(host ? host : "");
  g_autoptr(GString) description = g_string_new(NULL);
  g_string_append_printf(description, "%s %s requires a username and password.", kind, authority);
  if (realm && *realm)
    g_string_append_printf(description, "\nRealm: %s", realm);
  return g_string_free(g_steal_pointer(&description), FALSE);
}

/* Adds a labelled entry as a new row of `grid`, returning the entry. */
static GtkWidget *wig_auth_dialog_add_field(GtkGrid *grid, int row, const char *label_text, AuthDialogData *data)
{
  GtkWidget *label = gtk_label_new(label_text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_grid_attach(grid, label, 0, row, 1, 1);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_hexpand(entry, TRUE);
  g_signal_connect(entry, "activate", G_CALLBACK(wig_auth_dialog_entry_activated), data);
  gtk_grid_attach(grid, entry, 1, row, 1, 1);
  return entry;
}

void wig_auth_dialog_show(GtkOverlay *overlay, WebKitAuthenticationRequest *request)
{
  WebKitAuthenticationScheme scheme = webkit_authentication_request_get_scheme(request);
  if (!scheme_uses_password(scheme)) {
    g_debug("auth: rejecting authentication scheme %d that needs no password", scheme);
    webkit_authentication_request_cancel(request);
    return;
  }

  AuthDialogData *data = g_new0(AuthDialogData, 1);
  data->request = g_object_ref(request);
  data->overlay = overlay;

  GtkWidget *card = NULL;
  data->backdrop = wig_modal_dialog_present(overlay, &card, wig_auth_dialog_cancel, data);
  g_signal_connect_swapped(data->backdrop, "destroy", G_CALLBACK(auth_dialog_data_free), data);

  GtkWidget *heading = gtk_label_new("Authentication Required");
  gtk_widget_add_css_class(heading, "title-3");
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_box_append(GTK_BOX(card), heading);

  g_autofree char *description = wig_auth_dialog_describe(request);
  GtkWidget *description_label = gtk_label_new(description);
  gtk_label_set_wrap(GTK_LABEL(description_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(description_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(description_label), 50);
  gtk_label_set_xalign(GTK_LABEL(description_label), 0.0f);
  gtk_box_append(GTK_BOX(card), description_label);

  if (webkit_authentication_request_is_retry(request)) {
    GtkWidget *retry_label = gtk_label_new("The previous attempt failed. Please try again.");
    gtk_widget_add_css_class(retry_label, "error");
    gtk_label_set_xalign(GTK_LABEL(retry_label), 0.0f);
    gtk_box_append(GTK_BOX(card), retry_label);
  }

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_box_append(GTK_BOX(card), grid);
  data->username_entry = wig_auth_dialog_add_field(GTK_GRID(grid), 0, "Username", data);
  data->password_entry = wig_auth_dialog_add_field(GTK_GRID(grid), 1, "Password", data);
  gtk_entry_set_visibility(GTK_ENTRY(data->password_entry), FALSE);

  g_autoptr(WebKitCredential) proposed = webkit_authentication_request_get_proposed_credential(request);
  const char *proposed_username = proposed ? webkit_credential_get_username(proposed) : NULL;
  const char *proposed_password = proposed ? webkit_credential_get_password(proposed) : NULL;
  if (proposed_username)
    gtk_editable_set_text(GTK_EDITABLE(data->username_entry), proposed_username);
  if (proposed_password)
    gtk_editable_set_text(GTK_EDITABLE(data->password_entry), proposed_password);

  /* When WebKit can persist credentials (libsecret), offer to save them to the
   * keyring; otherwise the best we can do is keep them for the running session. */
  gboolean can_save = webkit_authentication_request_can_save_credentials(request);
  data->remember_persistence = can_save ? WEBKIT_CREDENTIAL_PERSISTENCE_PERMANENT
                                        : WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION;
  data->remember_check = gtk_check_button_new_with_label(can_save ? "Remember password" : "Remember for this session");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(data->remember_check), !can_save);
  gtk_box_append(GTK_BOX(card), data->remember_check);

  GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(button_row, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(card), button_row);

  GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
  g_signal_connect(cancel_button, "clicked", G_CALLBACK(wig_auth_dialog_cancel_clicked), data);
  gtk_box_append(GTK_BOX(button_row), cancel_button);

  GtkWidget *login_button = gtk_button_new_with_label("Log In");
  gtk_widget_add_css_class(login_button, "suggested-action");
  g_signal_connect(login_button, "clicked", G_CALLBACK(wig_auth_dialog_login_clicked), data);
  gtk_box_append(GTK_BOX(button_row), login_button);

  gtk_widget_grab_focus(proposed_username && *proposed_username ? data->password_entry : data->username_entry);
}
