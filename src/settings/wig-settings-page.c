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

#include "wig-settings-page.h"

#include "wig-application.h"
#include "wig-settings-rows.h"

#include <adwaita.h>

struct _WigSettingsPage {
  WigNativePage parent;

  GtkWidget *preferences;
};

G_DEFINE_FINAL_TYPE(WigSettingsPage, wig_settings_page, WIG_TYPE_NATIVE_PAGE)

gboolean uri_is_settings_page(const char *uri)
{
  if (!uri)
    return FALSE;

  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  return parsed && g_strcmp0(g_uri_get_scheme(parsed), "wig") == 0
      && g_strcmp0(g_uri_get_path(parsed), "settings") == 0;
}

static void wig_settings_page_add_browsing_group(WigSettingsPage *self, GSettings *settings)
{
  static const char *tab_layout_nicks[] = { "horizontal", "vertical", NULL };
  static const char *const tab_layout_labels[] = { "Tab Bar", "Sidebar", NULL };

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, "Browsing");

  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "restore-tabs", "Restore Tabs on Startup",
                                                        "Open the windows and tabs that were open when wig was "
                                                        "last closed."));
  adw_preferences_group_add(group,
                            wig_settings_combo_row_new(settings, "tab-layout", "Tab Layout",
                                                       "Show tabs in a bar above the page or in a sidebar beside it.",
                                                       tab_layout_nicks, tab_layout_labels));

#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  static const char *https_nicks[] = { "keep-as-requested", "https-first", "https-only", NULL };
  static const char *const https_labels[] = { "Off", "HTTPS-First", "HTTPS-Only", NULL };

  adw_preferences_group_add(group,
                            wig_settings_combo_row_new(settings, "https-navigation-policy", "HTTPS Navigation",
                                                       "Upgrade addresses typed as http to https. HTTPS-First "
                                                       "quietly falls back to http when the secure load fails; "
                                                       "HTTPS-Only shows an error instead.",
                                                       https_nicks, https_labels));
#endif

  wig_settings_search_engine_rows_add(group, settings);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->preferences), group);
}

static void wig_settings_page_add_content_group(WigSettingsPage *self, GSettings *settings)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, "Content");

  adw_preferences_group_add(
      group,
      wig_settings_switch_row_new(settings, "enable-javascript", "JavaScript", "Run the scripts a page carries."));
  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "auto-load-images", "Load Images",
                                                        "Fetch the images a page asks for. Turning this off saves "
                                                        "data and leaves the rest of the page as it is."));
  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "javascript-can-open-windows-automatically",
                                                        "Allow Pop-up Windows",
                                                        "Let a page open a window on its own, rather than only when "
                                                        "a click asks for one."));
  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "media-playback-requires-user-gesture",
                                                        "Require a Click Before Media Plays",
                                                        "Hold audio and video until they are started, instead of "
                                                        "letting a page play them on its own."));
  adw_preferences_group_add(group,
                            wig_settings_switch_row_new(settings, "zoom-text-only", "Zoom Text Only",
                                                        "Resize only the text when zooming, leaving images and "
                                                        "layout at their own size."));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->preferences), group);
}

static void wig_settings_page_dispose(GObject *object)
{
  WigSettingsPage *self = WIG_SETTINGS_PAGE(object);

  g_clear_pointer(&self->preferences, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_settings_page_parent_class)->dispose(object);
}

static void wig_settings_page_class_init(WigSettingsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_page_dispose;

  gtk_widget_class_set_css_name(widget_class, "wig-settings-page");
}

static void wig_settings_page_init(WigSettingsPage *self)
{
  GSettings *settings = wig_application_get_settings(wig_application_get());

  self->preferences = adw_preferences_page_new();
  gtk_widget_set_parent(self->preferences, GTK_WIDGET(self));

  wig_settings_page_add_browsing_group(self, settings);
  wig_settings_page_add_content_group(self, settings);

  wig_native_page_set_title(WIG_NATIVE_PAGE(self), WIG_SETTINGS_PAGE_TITLE);
  wig_native_page_set_uri(WIG_NATIVE_PAGE(self), WIG_SETTINGS_PAGE_URI);
}

GtkWidget *wig_settings_page_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_PAGE, NULL);
}
