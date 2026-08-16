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

#include "wig-settings-web-apps.h"

#include "wig-application.h"
#include "wig-pwa-manager.h"
#include "wig-pwa-util.h"

#include <adwaita.h>

/* What the launch mode combo stores for each of its entries, in the same order.
 * The first stands for whatever the manifest asked for, which is the absence of
 * an override rather than a mode of its own. */
static const char *const launch_mode_overrides[] = {
  NULL, "auto", "focus-existing", "navigate-existing", "navigate-new",
};

static const char *const launch_mode_labels[] = {
  NULL, "Auto", "Focus Existing", "Navigate Existing", "Navigate New", NULL,
};

#define APP_ICON_FALLBACK "application-x-executable-symbolic"

struct _WigSettingsWebApps {
  GtkWidget parent;

  WigPwaManager *manager; /* borrowed from the application */

  GtkWidget *page;
  AdwPreferencesGroup *group;
  GtkWidget *status;
  GPtrArray *rows; /* borrowed rows currently in the group */
  gboolean populated;
  /* Writing a setting has the manager announce a change the pane is already
   * showing, and building the rows again underneath the one being used would
   * take it away mid-gesture. */
  gboolean applying;
};

G_DEFINE_FINAL_TYPE(WigSettingsWebApps, wig_settings_web_apps, GTK_TYPE_WIDGET)

static void web_apps_list(WigSettingsWebApps *self);

static const char *launch_mode_name(WigPwaLaunchClientMode mode)
{
  switch (mode) {
  case WIG_PWA_LAUNCH_CLIENT_MODE_FOCUS_EXISTING:
    return "Focus Existing";
  case WIG_PWA_LAUNCH_CLIENT_MODE_NAVIGATE_EXISTING:
    return "Navigate Existing";
  case WIG_PWA_LAUNCH_CLIENT_MODE_NAVIGATE_NEW:
    return "Navigate New";
  case WIG_PWA_LAUNCH_CLIENT_MODE_AUTO:
  default:
    return "Auto";
  }
}

/* The row a control belongs to is the only thing that knows which app it is
 * for. */
static const char *widget_app_id(gpointer widget)
{
  return gtk_widget_get_name(GTK_WIDGET(widget));
}

static void web_apps_launch(GtkButton *button, WigSettingsWebApps *self)
{
  const char *id = widget_app_id(button);
  g_autoptr(GError) error = NULL;

  g_debug("web-apps: opening '%s'", id);
  if (!wig_application_open_web_app(wig_application_get(), id, NULL, &error))
    g_warning("web-apps: could not open '%s': %s", id, error->message);
}

static void web_apps_uninstall(GtkButton *button, WigSettingsWebApps *self)
{
  const char *id = widget_app_id(button);
  g_autoptr(GError) error = NULL;

  g_debug("web-apps: uninstalling '%s'", id);
  /* The manager announces what it removed, which is what puts the list right. */
  if (!wig_pwa_manager_uninstall(self->manager, id, &error))
    g_warning("web-apps: could not uninstall '%s': %s", id, error->message);
}

static void web_apps_launch_mode_changed(AdwComboRow *row, GParamSpec *pspec, WigSettingsWebApps *self)
{
  const char *id = widget_app_id(row);
  guint selected = adw_combo_row_get_selected(row);
  g_autoptr(GError) error = NULL;

  if (selected >= G_N_ELEMENTS(launch_mode_overrides))
    return;

  self->applying = TRUE;
  if (!wig_pwa_manager_set_launch_client_mode_override(self->manager, id, launch_mode_overrides[selected], &error))
    g_warning("web-apps: could not set the launch mode of '%s': %s", id, error->message);
  self->applying = FALSE;
}

static void web_apps_open_in_browser_changed(AdwSwitchRow *row, GParamSpec *pspec, WigSettingsWebApps *self)
{
  const char *id = widget_app_id(row);
  gboolean in_browser = adw_switch_row_get_active(row);
  g_autoptr(GError) error = NULL;

  self->applying = TRUE;
  if (!wig_pwa_manager_set_open_primary_scope_in_browser(self->manager, id, in_browser, &error))
    g_warning("web-apps: could not set where '%s' opens its own addresses: %s", id, error->message);
  self->applying = FALSE;
}

static GtkWidget *web_apps_icon(WigSettingsWebApps *self, const char *id)
{
  g_autofree char *content_type = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = wig_pwa_manager_get_cached_icon(self->manager, id, &content_type, &error);

  if (!bytes) {
    if (error)
      g_debug("web-apps: no usable icon for '%s': %s", id, error->message);
    return gtk_image_new_from_icon_name(APP_ICON_FALLBACK);
  }

  g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes(bytes, &error);
  if (!texture) {
    g_debug("web-apps: could not decode the icon of '%s': %s", id, error->message);
    return gtk_image_new_from_icon_name(APP_ICON_FALLBACK);
  }

  GtkWidget *image = gtk_image_new_from_paintable(GDK_PAINTABLE(texture));
  gtk_image_set_pixel_size(GTK_IMAGE(image), 32);

  return image;
}

static GtkWidget *web_apps_info_row(const char *title, const char *value)
{
  GtkWidget *row = adw_action_row_new();

  /* What a manifest holds is the site's own text rather than markup, and so is
   * everything installing it wrote down. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value && *value ? value : "Not stored");
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);

  return row;
}

static char *web_apps_last_updated(GVariant *details)
{
  gint64 last_updated = 0;

  g_variant_lookup(details, "last_updated", "x", &last_updated);
  if (last_updated <= 0)
    return NULL;

  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local(last_updated);
  return date ? g_date_time_format(date, "%c") : NULL;
}

static char *web_apps_scope_extensions(GVariant *details)
{
  g_autoptr(GVariant) extensions = g_variant_lookup_value(details, "scope_extensions", G_VARIANT_TYPE_STRING_ARRAY);

  if (!extensions || g_variant_n_children(extensions) == 0)
    return NULL;

  g_auto(GStrv) values = g_variant_dup_strv(extensions, NULL);
  return g_strjoinv(", ", values);
}

static GtkWidget *web_apps_launch_mode_row(WigSettingsWebApps *self, const char *id, GVariant *details)
{
  g_autoptr(GVariant) manifest = g_variant_lookup_value(details, "manifest", G_VARIANT_TYPE_VARDICT);
  WigPwaLaunchClientMode manifest_mode = manifest ? wig_pwa_launch_handler_get_client_mode(manifest)
                                                  : WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;
  const char *override = wig_pwa_variant_lookup_string(details, "launch_client_mode");

  /* The first entry says what following the manifest amounts to, so choosing it
   * is not a step into the unknown. */
  g_autofree char *manifest_label = g_strdup_printf("Manifest Default (%s)", launch_mode_name(manifest_mode));
  g_autofree const char **labels = g_memdup2(launch_mode_labels, sizeof(launch_mode_labels));
  labels[0] = manifest_label;

  guint selected = 0;
  for (guint i = 1; i < G_N_ELEMENTS(launch_mode_overrides); i++) {
    if (g_strcmp0(override, launch_mode_overrides[i]) == 0)
      selected = i;
  }

  g_autoptr(GtkStringList) model = gtk_string_list_new((const char *const *)labels);
  GtkWidget *row = adw_combo_row_new();

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Launch Mode");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "What opening the app again does with the window it already has.");
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
  adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);

  /* Connected once the mode in use is on show, so putting it there is not taken
   * for a change to it. */
  gtk_widget_set_name(row, id);
  g_signal_connect(row, "notify::selected", G_CALLBACK(web_apps_launch_mode_changed), self);

  return row;
}

static GtkWidget *web_apps_open_in_browser_row(WigSettingsWebApps *self, const char *id)
{
  g_autoptr(GError) error = NULL;
  gboolean in_browser = wig_pwa_manager_get_open_primary_scope_in_browser(self->manager, id, &error);

  if (error) {
    g_debug("web-apps: could not read where '%s' opens its own addresses: %s", id, error->message);
    in_browser = FALSE;
  }

  GtkWidget *row = adw_switch_row_new();

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Open Links in the Browser");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                              "Send addresses inside the app's own scope to a browser tab rather than to the app.");
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), in_browser);

  gtk_widget_set_name(row, id);
  g_signal_connect(row, "notify::active", G_CALLBACK(web_apps_open_in_browser_changed), self);

  return row;
}

static void web_apps_add_detail_rows(WigSettingsWebApps *self, AdwExpanderRow *row, const char *id)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) details = wig_pwa_manager_get_details(self->manager, id, &error);

  if (!details) {
    g_warning("web-apps: could not read the details of '%s': %s", id, error->message);
    adw_expander_row_add_row(row, web_apps_info_row("Details", error->message));
    return;
  }

  g_autoptr(GVariant) manifest = g_variant_lookup_value(details, "manifest", G_VARIANT_TYPE_VARDICT);
  g_autofree char *last_updated = web_apps_last_updated(details);
  g_autofree char *extensions = web_apps_scope_extensions(details);

  adw_expander_row_add_row(row, web_apps_launch_mode_row(self, id, details));
  adw_expander_row_add_row(row, web_apps_open_in_browser_row(self, id));

  const struct {
    const char *title;
    const char *value;
  } info[] = {
    { "Application ID", id },
    { "Short Name", manifest ? wig_pwa_variant_lookup_string(manifest, "short_name") : NULL },
    { "Manifest URL", wig_pwa_variant_lookup_string(details, "manifest_url") },
    { "Start URL", wig_pwa_variant_lookup_string(details, "start_url") },
    { "Scope", wig_pwa_variant_lookup_string(details, "scope") },
    { "Verified Scope Extensions", extensions ? extensions : "None" },
    { "Background Color", wig_pwa_variant_lookup_string(details, "background_color") },
    { "Desktop File", wig_pwa_variant_lookup_string(details, "desktop_file") },
    { "Icon File", wig_pwa_variant_lookup_string(details, "icon_file") },
    { "Last Updated", last_updated },
  };

  for (guint i = 0; i < G_N_ELEMENTS(info); i++)
    adw_expander_row_add_row(row, web_apps_info_row(info[i].title, info[i].value));
}

static GtkWidget *web_apps_app_row(WigSettingsWebApps *self, const char *id, const char *name, const char *description)
{
  GtkWidget *row = adw_expander_row_new();

  /* An app is named by its manifest, which is the site's text and not markup. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
  adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), description && *description ? description : id);
  adw_expander_row_add_prefix(ADW_EXPANDER_ROW(row), web_apps_icon(self, id));

  g_autofree char *uninstall_tooltip = g_strdup_printf("Uninstall %s", name);
  GtkWidget *uninstall = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_name(uninstall, id);
  gtk_widget_set_tooltip_text(uninstall, uninstall_tooltip);
  gtk_widget_set_valign(uninstall, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(uninstall, "flat");
  g_signal_connect(uninstall, "clicked", G_CALLBACK(web_apps_uninstall), self);
  /* A suffix goes in ahead of the ones already there, so the trash is added
   * first and ends up to the right of the button that opens the app. */
  adw_expander_row_add_suffix(ADW_EXPANDER_ROW(row), uninstall);

  g_autofree char *launch_tooltip = g_strdup_printf("Open %s", name);
  GtkWidget *launch = gtk_button_new_from_icon_name("media-playback-start-symbolic");
  gtk_widget_set_name(launch, id);
  gtk_widget_set_tooltip_text(launch, launch_tooltip);
  gtk_widget_set_valign(launch, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(launch, "flat");
  g_signal_connect(launch, "clicked", G_CALLBACK(web_apps_launch), self);
  adw_expander_row_add_suffix(ADW_EXPANDER_ROW(row), launch);

  web_apps_add_detail_rows(self, ADW_EXPANDER_ROW(row), id);

  return row;
}

static void web_apps_set_status(WigSettingsWebApps *self, const char *message)
{
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->status), message);
  gtk_widget_set_visible(self->status, message != NULL);
}

static void web_apps_list(WigSettingsWebApps *self)
{
  self->populated = TRUE;

  for (guint i = 0; i < self->rows->len; i++)
    adw_preferences_group_remove(self->group, g_ptr_array_index(self->rows, i));

  g_ptr_array_set_size(self->rows, 0);

  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) apps = wig_pwa_manager_list(self->manager, &error);
  if (!apps) {
    g_warning("web-apps: could not list the installed apps: %s", error->message);
    web_apps_set_status(self, error->message);
    return;
  }

  GVariantIter iterator;
  g_variant_iter_init(&iterator, apps);
  while (TRUE) {
    g_autoptr(GVariant) app = g_variant_iter_next_value(&iterator);
    if (!app)
      break;

    const char *id = wig_pwa_variant_lookup_string(app, "id");
    if (!id)
      continue;

    const char *name = wig_pwa_variant_lookup_string(app, "name");
    GtkWidget *row = web_apps_app_row(self, id, name && *name ? name : id,
                                      wig_pwa_variant_lookup_string(app, "description"));

    adw_preferences_group_add(self->group, row);
    g_ptr_array_add(self->rows, row);
  }

  g_debug("web-apps: listed %u installed app(s)", self->rows->len);
  web_apps_set_status(self, self->rows->len == 0 ? "No web apps installed" : NULL);
}

static void web_apps_changed(WigSettingsWebApps *self, const char *id, WigPwaManager *manager)
{
  if (self->applying)
    return;

  if (!gtk_widget_get_mapped(GTK_WIDGET(self))) {
    self->populated = FALSE;
    return;
  }

  web_apps_list(self);
}

void wig_settings_web_apps_index(WigSettingsSearch *search, const char *pane, const char *pane_title)
{
  wig_settings_search_add(search, "Web Apps", "The web applications installed from a page.", pane, pane_title);
  wig_settings_search_add(search, "Launch Mode", "What opening an app again does with the window it already has.", pane,
                          pane_title);
  wig_settings_search_add(search, "Uninstall Web App", "Remove an installed app and its launcher.", pane, pane_title);
}

static void wig_settings_web_apps_map(GtkWidget *widget)
{
  WigSettingsWebApps *self = WIG_SETTINGS_WEB_APPS(widget);

  GTK_WIDGET_CLASS(wig_settings_web_apps_parent_class)->map(widget);

  /* Every row decodes an icon, so the list waits until the pane is looked at. */
  if (!self->populated)
    web_apps_list(self);
}

static void wig_settings_web_apps_dispose(GObject *object)
{
  WigSettingsWebApps *self = WIG_SETTINGS_WEB_APPS(object);

  g_clear_pointer(&self->page, gtk_widget_unparent);
  g_clear_pointer(&self->rows, g_ptr_array_unref);

  G_OBJECT_CLASS(wig_settings_web_apps_parent_class)->dispose(object);
}

static void wig_settings_web_apps_class_init(WigSettingsWebAppsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_web_apps_dispose;
  widget_class->map = wig_settings_web_apps_map;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-web-apps");
}

static void wig_settings_web_apps_init(WigSettingsWebApps *self)
{
  self->manager = wig_application_get_pwa_manager(wig_application_get());
  self->rows = g_ptr_array_new();

  self->page = adw_preferences_page_new();
  gtk_widget_set_parent(self->page, GTK_WIDGET(self));

  self->group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(self->group, "Installed Web Apps");
  adw_preferences_group_set_description(self->group,
                                        "Sites installed as applications of their own, each with a window and a "
                                        "launcher outside the browser.");

  /* Standing in for the list while there is nothing in it, and for the reason
   * when there was nothing to be read. */
  self->status = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->status), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->status), "No web apps installed");
  adw_preferences_group_add(self->group, self->status);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), self->group);

  g_signal_connect_object(self->manager, "changed", G_CALLBACK(web_apps_changed), self, G_CONNECT_SWAPPED);
}

GtkWidget *wig_settings_web_apps_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_WEB_APPS, NULL);
}
