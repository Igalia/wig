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

#include "wig-web-app-filter.h"

#include "wig-application.h"
#include "wig-pwa-manager.h"
#include "wig-pwa-util.h"

struct _WigWebAppFilter {
  GtkWidget parent;

  WigPwaManager *manager;
  GtkWidget *button;
  char *web_app;
};

G_DEFINE_FINAL_TYPE(WigWebAppFilter, wig_web_app_filter, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_WEB_APP,
  N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GVariant *wig_web_app_filter_list(WigWebAppFilter *self)
{
  g_autoptr(GError) error = NULL;
  GVariant *apps = wig_pwa_manager_list(self->manager, &error);

  if (!apps)
    g_warning("web-app-filter: could not list the installed web apps: %s", error->message);
  return apps;
}

static char *wig_web_app_filter_name(WigWebAppFilter *self, const char *id)
{
  g_autoptr(GVariant) apps = wig_web_app_filter_list(self);
  GVariantIter iterator;

  if (!apps)
    return g_strdup(id);

  g_variant_iter_init(&iterator, apps);
  while (TRUE) {
    g_autoptr(GVariant) app = g_variant_iter_next_value(&iterator);
    if (!app)
      break;

    if (g_strcmp0(wig_pwa_variant_lookup_string(app, "id"), id) == 0) {
      const char *name = wig_pwa_variant_lookup_string(app, "name");
      return g_strdup(name && *name ? name : id);
    }
  }

  return g_strdup(id);
}

static void wig_web_app_filter_update(WigWebAppFilter *self)
{
  g_autoptr(GVariant) apps = wig_web_app_filter_list(self);
  g_autofree char *label = self->web_app ? wig_web_app_filter_name(self, self->web_app) : g_strdup("Browser");

  gtk_menu_button_set_label(GTK_MENU_BUTTON(self->button), label);
  gtk_widget_set_visible(GTK_WIDGET(self), self->web_app || (apps && g_variant_n_children(apps) > 0));
}

static void wig_web_app_filter_build_menu(GtkMenuButton *button, gpointer user_data)
{
  WigWebAppFilter *self = WIG_WEB_APP_FILTER(user_data);
  g_autoptr(GMenu) menu = g_menu_new();
  g_autoptr(GMenu) web_apps = g_menu_new();
  g_autoptr(GVariant) apps = wig_web_app_filter_list(self);

  g_autoptr(GMenuItem) browser = g_menu_item_new("Browser", NULL);
  g_menu_item_set_action_and_target(browser, "web-app-filter.select", "s", "");
  g_menu_append_item(menu, browser);

  GVariantIter iterator;
  if (apps)
    g_variant_iter_init(&iterator, apps);
  while (apps) {
    g_autoptr(GVariant) app = g_variant_iter_next_value(&iterator);
    if (!app)
      break;

    const char *id = wig_pwa_variant_lookup_string(app, "id");
    const char *name = wig_pwa_variant_lookup_string(app, "name");
    if (!id)
      continue;

    g_autoptr(GString) label = g_string_new(name && *name ? name : id);
    g_string_replace(label, "_", "__", 0);
    g_autoptr(GMenuItem) item = g_menu_item_new(label->str, NULL);
    g_menu_item_set_action_and_target(item, "web-app-filter.select", "s", id);
    g_menu_append_item(web_apps, item);
  }

  g_menu_append_section(menu, "Web Apps", G_MENU_MODEL(web_apps));
  gtk_menu_button_set_menu_model(button, G_MENU_MODEL(menu));
}

static void wig_web_app_filter_selected(GSimpleAction *action, GVariant *state, gpointer user_data)
{
  WigWebAppFilter *self = WIG_WEB_APP_FILTER(user_data);
  const char *web_app = g_variant_get_string(state, NULL);

  g_simple_action_set_state(action, state);
  if (!g_set_str(&self->web_app, *web_app ? web_app : NULL))
    return;

  wig_web_app_filter_update(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_WEB_APP]);
}

static void wig_web_app_filter_changed(WigWebAppFilter *self, const char *id, WigPwaManager *manager)
{
  wig_web_app_filter_update(self);
}

static void wig_web_app_filter_removed(WigWebAppFilter *self, const char *id, WigPwaManager *manager)
{
  if (g_strcmp0(id, self->web_app) == 0)
    gtk_widget_activate_action(GTK_WIDGET(self), "web-app-filter.select", "s", "");
  else
    wig_web_app_filter_update(self);
}

static const GActionEntry actions[] = {
  { "select", NULL, "s", "''", wig_web_app_filter_selected },
};

static void wig_web_app_filter_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigWebAppFilter *self = WIG_WEB_APP_FILTER(object);

  switch (prop_id) {
  case PROP_WEB_APP:
    g_value_set_string(value, self->web_app);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void wig_web_app_filter_dispose(GObject *object)
{
  WigWebAppFilter *self = WIG_WEB_APP_FILTER(object);

  g_clear_pointer(&self->button, gtk_widget_unparent);
  g_clear_pointer(&self->web_app, g_free);

  G_OBJECT_CLASS(wig_web_app_filter_parent_class)->dispose(object);
}

static void wig_web_app_filter_class_init(WigWebAppFilterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_web_app_filter_dispose;
  object_class->get_property = wig_web_app_filter_get_property;

  props[PROP_WEB_APP] = g_param_spec_string("web-app", NULL, NULL, NULL,
                                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-web-app-filter");
}

static void wig_web_app_filter_init(WigWebAppFilter *self)
{
  self->manager = wig_application_get_pwa_manager(wig_application_get());

  g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), actions, G_N_ELEMENTS(actions), self);
  gtk_widget_insert_action_group(GTK_WIDGET(self), "web-app-filter", G_ACTION_GROUP(group));

  self->button = gtk_menu_button_new();
  gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(self->button), TRUE);
  gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(self->button), wig_web_app_filter_build_menu, self, NULL);
  gtk_widget_set_parent(self->button, GTK_WIDGET(self));

  g_signal_connect_object(self->manager, "changed", G_CALLBACK(wig_web_app_filter_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->manager, "removed", G_CALLBACK(wig_web_app_filter_removed), self, G_CONNECT_SWAPPED);
  wig_web_app_filter_update(self);
}

GtkWidget *wig_web_app_filter_new(void)
{
  return g_object_new(WIG_TYPE_WEB_APP_FILTER, NULL);
}

const char *wig_web_app_filter_get_web_app(WigWebAppFilter *self)
{
  return self->web_app;
}
