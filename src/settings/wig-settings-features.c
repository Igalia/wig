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

#include "wig-settings-features.h"

#include "wig-application.h"

#define WIG_FEATURE_OVERRIDES_KEY "feature-overrides"

/* What a row needs to act on the feature it stands for. */
typedef struct {
  WebKitFeature *feature;
  WebKitSettings *web_settings;
  GSettings *settings;
} FeatureRow;

struct _WigSettingsFeatures {
  GtkWidget parent;

  WigFeaturesKind kind;
  GtkWidget *page;
  gboolean populated;
};

G_DEFINE_FINAL_TYPE(WigSettingsFeatures, wig_settings_features, GTK_TYPE_WIDGET)

enum {
  MODE_DEFAULT,
  MODE_SESSION,
  MODE_PERMANENT,
};

static WebKitFeature *find_feature(WebKitFeatureList *features, const char *identifier)
{
  for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
    WebKitFeature *feature = webkit_feature_list_get(features, i);
    if (g_str_equal(webkit_feature_get_identifier(feature), identifier))
      return feature;
  }
  return NULL;
}

void wig_features_apply_overrides(WebKitSettings *web_settings, GSettings *settings)
{
  g_autoptr(GVariant) overrides = g_settings_get_value(settings, WIG_FEATURE_OVERRIDES_KEY);
  g_autoptr(WebKitFeatureList) experimental = webkit_settings_get_experimental_features();
  g_autoptr(WebKitFeatureList) development = webkit_settings_get_development_features();

  GVariantIter iter;
  const char *identifier;
  gboolean enabled;
  g_variant_iter_init(&iter, overrides);
  while (g_variant_iter_next(&iter, "{&sb}", &identifier, &enabled)) {
    WebKitFeature *feature = find_feature(experimental, identifier);
    if (!feature)
      feature = find_feature(development, identifier);

    /* Features come and go between WebKit versions, so an override can outlive
     * the feature it names. */
    if (!feature) {
      g_debug("features: no feature named '%s' to override", identifier);
      continue;
    }

    g_debug("features: %s %s (permanent)", identifier, enabled ? "enabled" : "disabled");
    webkit_settings_set_feature_enabled(web_settings, feature, enabled);
  }
}

static void apply_feature_mode(FeatureRow *row, guint mode)
{
  const char *identifier = webkit_feature_get_identifier(row->feature);
  gboolean enabled = mode == MODE_DEFAULT ? webkit_feature_get_default_value(row->feature)
                                          : !webkit_feature_get_default_value(row->feature);

  g_debug("features: %s %s (%d)", identifier, enabled ? "enabled" : "disabled", mode);
  webkit_settings_set_feature_enabled(row->web_settings, row->feature, enabled);

  /* A dictionary can only be written whole, so the other overrides are carried
   * over and this feature is left out unless it is being kept for good. */
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sb}"));

  g_autoptr(GVariant) overrides = g_settings_get_value(row->settings, WIG_FEATURE_OVERRIDES_KEY);
  GVariantIter iter;
  const char *key;
  gboolean value;
  g_variant_iter_init(&iter, overrides);
  while (g_variant_iter_next(&iter, "{&sb}", &key, &value)) {
    if (!g_str_equal(key, identifier))
      g_variant_builder_add(&builder, "{sb}", key, value);
  }

  if (mode == MODE_PERMANENT)
    g_variant_builder_add(&builder, "{sb}", identifier, enabled);

  g_settings_set_value(row->settings, WIG_FEATURE_OVERRIDES_KEY, g_variant_builder_end(&builder));
}

static void feature_row_free(gpointer data, GClosure *closure)
{
  FeatureRow *row = data;

  webkit_feature_unref(row->feature);
  g_free(row);
}

static void feature_mode_changed(GtkDropDown *modes, GParamSpec *pspec, FeatureRow *row)
{
  apply_feature_mode(row, gtk_drop_down_get_selected(modes));
}

/* Where a feature sits between untouched and rewritten: what WebKit ships, the
 * other way for this run, or the other way from now on. */
static guint feature_current_mode(WebKitSettings *web_settings, GSettings *settings, WebKitFeature *feature)
{
  g_autoptr(GVariant) overrides = g_settings_get_value(settings, WIG_FEATURE_OVERRIDES_KEY);
  gboolean stored;

  if (g_variant_lookup(overrides, webkit_feature_get_identifier(feature), "b", &stored))
    return MODE_PERMANENT;

  gboolean enabled = webkit_settings_get_feature_enabled(web_settings, feature);
  return enabled == webkit_feature_get_default_value(feature) ? MODE_DEFAULT : MODE_SESSION;
}

static const char *feature_status_label(WebKitFeatureStatus status)
{
  switch (status) {
  case WEBKIT_FEATURE_STATUS_EMBEDDER:
    return "embedder";
  case WEBKIT_FEATURE_STATUS_UNSTABLE:
    return "unstable";
  case WEBKIT_FEATURE_STATUS_INTERNAL:
    return "internal";
  case WEBKIT_FEATURE_STATUS_DEVELOPER:
    return "developer";
  case WEBKIT_FEATURE_STATUS_TESTABLE:
    return "testable";
  case WEBKIT_FEATURE_STATUS_PREVIEW:
    return "preview";
  case WEBKIT_FEATURE_STATUS_STABLE:
    return "stable";
  case WEBKIT_FEATURE_STATUS_MATURE:
    return "mature";
  default:
    return "unknown";
  }
}

/* How far along a feature is, said in the colours libadwaita already gives a
 * label, so the badge needs no styling of wig's own. */
static const char *feature_status_style(WebKitFeatureStatus status)
{
  switch (status) {
  case WEBKIT_FEATURE_STATUS_MATURE:
  case WEBKIT_FEATURE_STATUS_STABLE:
    return "success";
  case WEBKIT_FEATURE_STATUS_PREVIEW:
    return "accent";
  case WEBKIT_FEATURE_STATUS_TESTABLE:
    return "warning";
  case WEBKIT_FEATURE_STATUS_UNSTABLE:
    return "error";
  default:
    return "dim-label";
  }
}

static GtkWidget *feature_status_badge(WebKitFeature *feature)
{
  WebKitFeatureStatus status = webkit_feature_get_status(feature);
  GtkWidget *badge = gtk_label_new(feature_status_label(status));

  /* The colour comes from the status class, and the badge tints itself with
   * whatever that turns out to be. */
  gtk_widget_add_css_class(badge, "caption");
  gtk_widget_add_css_class(badge, "wig-feature-status");
  gtk_widget_add_css_class(badge, feature_status_style(status));
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

  return badge;
}

static GtkWidget *feature_row_new(WebKitSettings *web_settings, GSettings *settings, WebKitFeature *feature)
{
  gboolean default_value = webkit_feature_get_default_value(feature);
  const char *details = webkit_feature_get_details(feature);

  g_autofree char *default_label = g_strdup_printf("Default (%s)", default_value ? "enabled" : "disabled");
  g_autofree char *session_label = g_strdup_printf("%s until restart", default_value ? "Disabled" : "Enabled");
  g_autofree char *permanent_label = g_strdup_printf("%s permanently", default_value ? "Disabled" : "Enabled");
  const char *labels[] = { default_label, session_label, permanent_label, NULL };
  g_autoptr(GtkStringList) modes = gtk_string_list_new(labels);

  /* The badge belongs at the end of the name's own line rather than out beside
   * the mode, which a row built out of a title and a subtitle has nowhere to put
   * it, so the row is laid out here:
   *
   *   [ name             badge ] [ mode ]
   *   [ details                ] [      ]
   */
  GtkWidget *row = adw_preferences_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), webkit_feature_get_name(feature));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

  GtkWidget *name = gtk_label_new(webkit_feature_get_name(feature));
  gtk_label_set_xalign(GTK_LABEL(name), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(name, TRUE);

  GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append(GTK_BOX(heading), name);
  gtk_box_append(GTK_BOX(heading), feature_status_badge(feature));

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(text), heading);

  if (details && *details) {
    GtkWidget *description = gtk_label_new(details);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_widget_add_css_class(description, "caption");
    gtk_widget_add_css_class(description, "dim-label");
    gtk_box_append(GTK_BOX(text), description);
  }

  GtkWidget *mode = gtk_drop_down_new(G_LIST_MODEL(g_steal_pointer(&modes)), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(mode), feature_current_mode(web_settings, settings, feature));
  gtk_widget_set_valign(mode, GTK_ALIGN_CENTER);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_box_append(GTK_BOX(box), text);
  gtk_box_append(GTK_BOX(box), mode);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

  FeatureRow *data = g_new0(FeatureRow, 1);
  data->feature = webkit_feature_ref(feature);
  data->web_settings = web_settings;
  data->settings = settings;
  /* Connected once the mode is showing where the feature already stands, so
   * saying so is not taken for changing it. */
  g_signal_connect_data(mode, "notify::selected", G_CALLBACK(feature_mode_changed), data, feature_row_free, 0);

  return row;
}

static void wig_settings_features_populate(WigSettingsFeatures *self)
{
  WigApplication *app = wig_application_get();
  WebKitSettings *web_settings = wig_application_get_web_settings(app);
  GSettings *settings = wig_application_get_settings(app);
  g_autoptr(WebKitFeatureList) features = self->kind == WIG_FEATURES_DEVELOPMENT
      ? webkit_settings_get_development_features()
      : webkit_settings_get_experimental_features();

  /* The categories are whatever the features name, in the order they first turn
   * up, so the list reads the way WebKit orders it. */
  g_autoptr(GHashTable) groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
    WebKitFeature *feature = webkit_feature_list_get(features, i);
    const char *category = webkit_feature_get_category(feature);
    if (!category || !*category)
      category = "Other";

    AdwPreferencesGroup *group = g_hash_table_lookup(groups, category);
    if (!group) {
      group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
      adw_preferences_group_set_title(group, category);
      adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), group);
      g_hash_table_insert(groups, g_strdup(category), group);
    }

    adw_preferences_group_add(group, feature_row_new(web_settings, settings, feature));
  }

  g_debug("features: listed %zu %s features", webkit_feature_list_get_length(features),
          self->kind == WIG_FEATURES_DEVELOPMENT ? "development" : "experimental");
}

static void wig_settings_features_map(GtkWidget *widget)
{
  WigSettingsFeatures *self = WIG_SETTINGS_FEATURES(widget);

  GTK_WIDGET_CLASS(wig_settings_features_parent_class)->map(widget);

  if (self->populated)
    return;

  self->populated = TRUE;
  wig_settings_features_populate(self);
}

static void wig_settings_features_dispose(GObject *object)
{
  WigSettingsFeatures *self = WIG_SETTINGS_FEATURES(object);

  g_clear_pointer(&self->page, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_settings_features_parent_class)->dispose(object);
}

static void wig_settings_features_class_init(WigSettingsFeaturesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_features_dispose;
  widget_class->map = wig_settings_features_map;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-features");
}

static void wig_settings_features_init(WigSettingsFeatures *self)
{
  self->page = adw_preferences_page_new();
  gtk_widget_set_parent(self->page, GTK_WIDGET(self));
}

GtkWidget *wig_settings_features_new(WigFeaturesKind kind)
{
  WigSettingsFeatures *self = g_object_new(WIG_TYPE_SETTINGS_FEATURES, NULL);

  self->kind = kind;

  return GTK_WIDGET(self);
}
