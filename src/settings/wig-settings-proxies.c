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

#include "wig-settings-proxies.h"

#include "wig-application.h"
#include "wig-settings-rows.h"

#include <adwaita.h>

/* Ordered as com.igalia.wig.ProxyMode is. */
static const char *proxy_mode_nicks[] = { "system", "none", "custom", NULL };
static const char *const proxy_mode_labels[] = { "Use System Configured", "None", "Custom", NULL };

#define PROXY_MODE_CUSTOM 2

#define BYPASS_DESCRIPTION                                                                                             \
  "Addresses reached directly rather than through the proxy. A name covers that host alone, "                          \
  "a name beginning with a dot covers the site and everything under it, and an address "                               \
  "range may be written as 192.168.0.0/24."

struct _WigSettingsProxies {
  GtkWidget parent;

  GSettings *settings; /* borrowed from the application */

  GtkWidget *page;
  GtkWidget *custom_group;
  AdwExpanderRow *bypass_row;
  GtkWidget *bypass_entry;
  GtkWidget *bypass_status;
  GPtrArray *bypass_rows; /* borrowed rows currently under the expander */
};

G_DEFINE_FINAL_TYPE(WigSettingsProxies, wig_settings_proxies, GTK_TYPE_WIDGET)

static void proxies_sync_bypass(WigSettingsProxies *self);

static void proxies_set_bypass(WigSettingsProxies *self, GPtrArray *hosts)
{
  g_ptr_array_add(hosts, NULL);
  g_settings_set_strv(self->settings, "proxy-ignore-hosts", (const char *const *)hosts->pdata);
}

static void bypass_removed(GtkButton *button, WigSettingsProxies *self)
{
  const char *removed = gtk_widget_get_name(GTK_WIDGET(button));
  g_auto(GStrv) hosts = g_settings_get_strv(self->settings, "proxy-ignore-hosts");
  g_autoptr(GPtrArray) kept = g_ptr_array_new();

  for (guint i = 0; hosts && hosts[i]; i++) {
    if (!g_str_equal(hosts[i], removed))
      g_ptr_array_add(kept, hosts[i]);
  }

  g_debug("proxy: %s no longer bypasses the proxy", removed);
  proxies_set_bypass(self, kept);
}

/* What is typed is a site to add rather than a line of text to keep, so the
 * entry empties itself once the site is on the list. */
static void bypass_applied(AdwEntryRow *entry, WigSettingsProxies *self)
{
  g_autofree char *added = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
  if (!*added)
    return;

  g_auto(GStrv) hosts = g_settings_get_strv(self->settings, "proxy-ignore-hosts");
  g_autoptr(GPtrArray) kept = g_ptr_array_new();

  for (guint i = 0; hosts && hosts[i]; i++) {
    /* A site named twice is still one site. */
    if (g_str_equal(hosts[i], added)) {
      gtk_editable_set_text(GTK_EDITABLE(entry), "");
      return;
    }
    g_ptr_array_add(kept, hosts[i]);
  }

  g_ptr_array_add(kept, added);
  g_debug("proxy: %s now bypasses the proxy", added);
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  proxies_set_bypass(self, kept);
}

/* A proxy nothing can be sent through would take every page down with it, so an
 * address that names no host keeps the row and says so rather than being
 * stored. */
static void proxy_url_applied(AdwEntryRow *entry, WigSettingsProxies *self)
{
  const char *url = gtk_editable_get_text(GTK_EDITABLE(entry));
  g_autoptr(GUri) parsed = *url ? g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED, NULL) : NULL;
  const char *host = parsed ? g_uri_get_host(parsed) : NULL;

  if (*url && (!host || !*host)) {
    gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
    return;
  }

  gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
  g_settings_set_string(self->settings, "proxy-url", url);
}

static GtkWidget *bypass_row_new(WigSettingsProxies *self, const char *host)
{
  GtkWidget *row = adw_action_row_new();

  /* A site is whatever was typed, which is not markup. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), host);

  GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  /* The button is the only thing that knows which site its row is for. */
  gtk_widget_set_name(remove, host);
  gtk_widget_set_tooltip_text(remove, "Send This Site Through the Proxy Again");
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(remove, "flat");
  g_signal_connect(remove, "clicked", G_CALLBACK(bypass_removed), self);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

  return row;
}

static void proxies_sync_bypass(WigSettingsProxies *self)
{
  g_auto(GStrv) hosts = g_settings_get_strv(self->settings, "proxy-ignore-hosts");

  for (guint i = 0; i < self->bypass_rows->len; i++)
    adw_expander_row_remove(self->bypass_row, g_ptr_array_index(self->bypass_rows, i));

  g_ptr_array_set_size(self->bypass_rows, 0);

  for (guint i = 0; hosts && hosts[i]; i++) {
    GtkWidget *row = bypass_row_new(self, hosts[i]);

    adw_expander_row_add_row(self->bypass_row, row);
    g_ptr_array_add(self->bypass_rows, row);
  }

  guint count = self->bypass_rows->len;
  g_autofree char *summary = count ? g_strdup_printf("%u site%s", count, count == 1 ? "" : "s") : NULL;
  adw_expander_row_set_subtitle(self->bypass_row, summary ? summary : "Everything goes through the proxy");
  gtk_widget_set_visible(self->bypass_status, count == 0);
}

/* The proxy to use and what may go around it are only worth showing once the
 * browser has been told to use one of its own. */
static void proxies_sync_mode(WigSettingsProxies *self)
{
  gboolean custom = g_settings_get_enum(self->settings, "proxy-mode") == PROXY_MODE_CUSTOM;

  gtk_widget_set_visible(self->custom_group, custom);
}

static void proxies_add_custom_group(WigSettingsProxies *self)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

  self->custom_group = GTK_WIDGET(group);
  adw_preferences_group_set_title(group, "Custom Proxy");
  adw_preferences_group_set_description(group, "Where requests are sent, and which of them go direct.");

  GtkWidget *url = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(url), "Proxy URL");
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(url), TRUE);
  g_settings_bind(self->settings, "proxy-url", url, "text", G_SETTINGS_BIND_GET);
  g_signal_connect(url, "apply", G_CALLBACK(proxy_url_applied), self);
  adw_preferences_group_add(group, url);

  self->bypass_row = ADW_EXPANDER_ROW(adw_expander_row_new());
  /* The sites are whatever was typed, which is not markup. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->bypass_row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->bypass_row), "Sites That Bypass the Proxy");
  adw_preferences_group_add(group, GTK_WIDGET(self->bypass_row));

  self->bypass_status = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->bypass_status), "No sites");
  adw_expander_row_add_row(self->bypass_row, self->bypass_status);

  self->bypass_entry = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->bypass_entry), "Add a Site");
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(self->bypass_entry), TRUE);
  g_signal_connect(self->bypass_entry, "apply", G_CALLBACK(bypass_applied), self);
  adw_expander_row_add_row(self->bypass_row, self->bypass_entry);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), group);
}

void wig_settings_proxies_index(WigSettingsSearch *search, const char *pane, const char *pane_title)
{
  wig_settings_search_add(search, "Default Proxy",
                          "Whether the browser follows the system, uses no proxy, or one "
                          "given here.",
                          pane, pane_title);
  wig_settings_search_add(search, "Proxy URL", "Where a custom proxy sends requests.", pane, pane_title);
  wig_settings_search_add(search, "Sites That Bypass the Proxy", BYPASS_DESCRIPTION, pane, pane_title);
}

static void wig_settings_proxies_dispose(GObject *object)
{
  WigSettingsProxies *self = WIG_SETTINGS_PROXIES(object);

  g_clear_pointer(&self->page, gtk_widget_unparent);
  g_clear_pointer(&self->bypass_rows, g_ptr_array_unref);

  G_OBJECT_CLASS(wig_settings_proxies_parent_class)->dispose(object);
}

static void wig_settings_proxies_class_init(WigSettingsProxiesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_proxies_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-proxies");
}

static void wig_settings_proxies_init(WigSettingsProxies *self)
{
  self->settings = wig_application_get_settings(wig_application_get());
  self->bypass_rows = g_ptr_array_new();

  self->page = adw_preferences_page_new();
  gtk_widget_set_parent(self->page, GTK_WIDGET(self));

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, "Proxy");
  adw_preferences_group_add(group,
                            wig_settings_combo_row_new(self->settings, "proxy-mode", "Default Proxy",
                                                       "Follow the proxy the system is configured with, reach every "
                                                       "site directly, or use a proxy of wig's own.",
                                                       proxy_mode_nicks, proxy_mode_labels));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), group);

  proxies_add_custom_group(self);

  /* The settings are what the pane shows, so a change made anywhere, by another
   * window or by the command line, reaches it the same way. */
  g_signal_connect_object(self->settings, "changed::proxy-mode", G_CALLBACK(proxies_sync_mode), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->settings, "changed::proxy-ignore-hosts", G_CALLBACK(proxies_sync_bypass), self,
                          G_CONNECT_SWAPPED);
  proxies_sync_mode(self);
  proxies_sync_bypass(self);
}

GtkWidget *wig_settings_proxies_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_PROXIES, NULL);
}
