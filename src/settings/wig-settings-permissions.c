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

#include "wig-settings-permissions.h"

#include "wig-application.h"
#include "wig-permissions-manager.h"

#include <adwaita.h>

/* A rule is an answer the user has already given, so asking is not one of the
 * choices: taking the rule away is what puts a site back to being asked. */
static const char *const rule_labels[] = { "Allow", "Block", NULL };

#define RULE_INDEX_ALLOW 0
#define RULE_INDEX_BLOCK 1

typedef struct {
  char *site;
  WebKitPermissionState state;
} Rule;

/* Indexed by wig_permission_kind_index(). */
typedef struct {
  WigSettingsPermissions *pane; /* borrowed */
  WigPermissionKind kind;
  AdwExpanderRow *row;
  GtkWidget *status;
  GtkWidget *entry;
  GtkWidget *allow_button;
  GtkWidget *block_button;
  gboolean expanded_before_search;
  GPtrArray *rules; /* borrowed rows currently under the expander */
} KindRow;

struct _WigSettingsPermissions {
  GtkWidget parent;

  WigPermissionsManager *manager; /* borrowed from application */

  GtkWidget *toolbar;
  GtkWidget *search_entry;
  char *filter;
  KindRow kinds[WIG_PERMISSION_N_KINDS];
  guint sync_idle_id;
};

G_DEFINE_FINAL_TYPE(WigSettingsPermissions, wig_settings_permissions, GTK_TYPE_WIDGET)

static void rule_clear(Rule *rule)
{
  g_clear_pointer(&rule->site, g_free);
}

static int compare_rules(gconstpointer first, gconstpointer second)
{
  return g_strcmp0(((const Rule *)first)->site, ((const Rule *)second)->site);
}

static guint dropdown_index_for_state(WebKitPermissionState state)
{
  return state == WEBKIT_PERMISSION_STATE_GRANTED ? RULE_INDEX_ALLOW : RULE_INDEX_BLOCK;
}

static WebKitPermissionState state_for_dropdown_index(guint index)
{
  return index == RULE_INDEX_ALLOW ? WEBKIT_PERMISSION_STATE_GRANTED : WEBKIT_PERMISSION_STATE_DENIED;
}

/* A site is typed the way it is spoken rather than as a URI, so what is missing
 * is filled in and the answer comes back in the form the answers on file are
 * kept in. Anything that does not name a host is not a site. */
static char *permissions_site_for_text(const char *text)
{
  g_autofree char *trimmed = g_strdup(text ? text : "");
  g_strstrip(trimmed);
  if (!*trimmed)
    return NULL;

  g_autofree char *uri = g_uri_peek_scheme(trimmed) ? g_strdup(trimmed) : g_strconcat("https://", trimmed, NULL);

  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  const char *host = parsed ? g_uri_get_host(parsed) : NULL;
  if (!host || !*host)
    return NULL;

  g_autoptr(WebKitSecurityOrigin) origin = webkit_security_origin_new_for_uri(uri);
  return origin ? webkit_security_origin_to_string(origin) : NULL;
}

static void wig_settings_permissions_queue_sync(WigSettingsPermissions *self);

static void wig_settings_permissions_set_rule(WigSettingsPermissions *self, WigPermissionKind kind, const char *site,
                                              WebKitPermissionState state)
{
  g_debug("settings-permissions: %s for '%s' is now %s", wig_permission_kind_get_property_name(kind), site,
          state == WEBKIT_PERMISSION_STATE_GRANTED      ? "allowed"
              : state == WEBKIT_PERMISSION_STATE_DENIED ? "blocked"
                                                        : "asked for");

  WigPermissions *permissions = wig_permissions_manager_ensure(self->manager, site);
  wig_permissions_set_state(permissions, kind, state);
}

/* The row a control belongs to is the only thing that knows which site it is
 * for, so each carries the site as its name. */
static void rule_state_changed(GtkDropDown *dropdown, GParamSpec *pspec, KindRow *kind_row)
{
  const char *site = gtk_widget_get_name(GTK_WIDGET(dropdown));

  wig_settings_permissions_set_rule(kind_row->pane, kind_row->kind, site,
                                    state_for_dropdown_index(gtk_drop_down_get_selected(dropdown)));
}

static void rule_removed(GtkButton *button, KindRow *kind_row)
{
  const char *site = gtk_widget_get_name(GTK_WIDGET(button));

  wig_settings_permissions_set_rule(kind_row->pane, kind_row->kind, site, WEBKIT_PERMISSION_STATE_PROMPT);
}

static GtkWidget *permissions_rule_row(KindRow *kind_row, const Rule *rule)
{
  GtkWidget *row = adw_action_row_new();

  /* A site is whatever it calls itself, which is not markup. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), rule->site);

  GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_name(remove, rule->site);
  gtk_widget_set_tooltip_text(remove, "Ask Again");
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(remove, "flat");
  g_signal_connect(remove, "clicked", G_CALLBACK(rule_removed), kind_row);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

  GtkWidget *dropdown = gtk_drop_down_new_from_strings(rule_labels);
  gtk_widget_set_name(dropdown, rule->site);
  gtk_widget_set_valign(dropdown, GTK_ALIGN_CENTER);
  /* Selected before anything is listening, so setting up a row does not read as
   * the user having changed it. */
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), dropdown_index_for_state(rule->state));
  g_signal_connect(dropdown, "notify::selected", G_CALLBACK(rule_state_changed), kind_row);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), dropdown);

  return row;
}

static char *permissions_summary(guint allowed, guint blocked)
{
  if (allowed && blocked)
    return g_strdup_printf("%u allowed, %u blocked", allowed, blocked);
  if (allowed)
    return g_strdup_printf("%u allowed", allowed);
  if (blocked)
    return g_strdup_printf("%u blocked", blocked);

  return g_strdup("No sites");
}

static void kind_row_collect(KindRow *kind_row, GArray *rules, WebKitPermissionState state)
{
  GList *sites = wig_permissions_manager_list_sites(kind_row->pane->manager, kind_row->kind, state);

  for (GList *l = sites; l; l = l->next) {
    Rule rule = { l->data, state };
    g_array_append_val(rules, rule);
  }

  /* The sites themselves are handed to the array, which frees them with it. */
  g_list_free(sites);
}

static void kind_row_sync(KindRow *kind_row)
{
  WigSettingsPermissions *self = kind_row->pane;

  for (guint i = 0; i < kind_row->rules->len; i++)
    adw_expander_row_remove(kind_row->row, g_ptr_array_index(kind_row->rules, i));

  g_ptr_array_set_size(kind_row->rules, 0);

  g_autoptr(GArray) rules = g_array_new(FALSE, FALSE, sizeof(Rule));
  g_array_set_clear_func(rules, (GDestroyNotify)rule_clear);
  kind_row_collect(kind_row, rules, WEBKIT_PERMISSION_STATE_GRANTED);
  guint allowed = rules->len;
  kind_row_collect(kind_row, rules, WEBKIT_PERMISSION_STATE_DENIED);
  guint blocked = rules->len - allowed;

  /* Sorted by site alone rather than by the answer, so changing an answer leaves
   * the row where the user is looking at it. */
  g_array_sort(rules, compare_rules);

  for (guint i = 0; i < rules->len; i++) {
    const Rule *rule = &g_array_index(rules, Rule, i);
    if (self->filter && !strstr(rule->site, self->filter))
      continue;

    GtkWidget *row = permissions_rule_row(kind_row, rule);
    adw_expander_row_add_row(kind_row->row, row);
    g_ptr_array_add(kind_row->rules, row);
  }

  g_autofree char *summary = permissions_summary(allowed, blocked);
  adw_expander_row_set_subtitle(kind_row->row, summary);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(kind_row->status), self->filter ? "No matching sites" : "No sites");
  gtk_widget_set_visible(kind_row->status, kind_row->rules->len == 0);

  /* Searching narrows the pane to the permissions that have something to show,
   * since a permission with nothing under it is not an answer to the search, and
   * opens the ones that are left: a match nobody can see is no answer either. */
  gtk_widget_set_visible(GTK_WIDGET(kind_row->row), !self->filter || kind_row->rules->len > 0);
  if (self->filter && kind_row->rules->len > 0)
    adw_expander_row_set_expanded(kind_row->row, TRUE);
}

static void wig_settings_permissions_sync(WigSettingsPermissions *self)
{
  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++)
    kind_row_sync(&self->kinds[i]);
}

static gboolean wig_settings_permissions_sync_idle(gpointer user_data)
{
  WigSettingsPermissions *self = WIG_SETTINGS_PERMISSIONS(user_data);

  self->sync_idle_id = 0;
  wig_settings_permissions_sync(self);

  return G_SOURCE_REMOVE;
}

/* Answering a row takes the row away and puts a new one in its place, which
 * cannot be done while the control that was clicked is still emitting. */
static void wig_settings_permissions_queue_sync(WigSettingsPermissions *self)
{
  if (!self->sync_idle_id)
    self->sync_idle_id = g_idle_add(wig_settings_permissions_sync_idle, self);
}

static void kind_row_add_rule(KindRow *kind_row, WebKitPermissionState state)
{
  g_autofree char *site = permissions_site_for_text(gtk_editable_get_text(GTK_EDITABLE(kind_row->entry)));
  if (!site)
    return;

  wig_settings_permissions_set_rule(kind_row->pane, kind_row->kind, site, state);
  gtk_editable_set_text(GTK_EDITABLE(kind_row->entry), "");
}

static void kind_row_allow_clicked(GtkButton *button, KindRow *kind_row)
{
  kind_row_add_rule(kind_row, WEBKIT_PERMISSION_STATE_GRANTED);
}

static void kind_row_block_clicked(GtkButton *button, KindRow *kind_row)
{
  kind_row_add_rule(kind_row, WEBKIT_PERMISSION_STATE_DENIED);
}

/* Nothing is offered for text that does not name a site, so a typing mistake is
 * shown by the buttons staying out of reach rather than by being told off. */
static void kind_row_entry_changed(GtkEditable *entry, KindRow *kind_row)
{
  g_autofree char *site = permissions_site_for_text(gtk_editable_get_text(entry));

  gtk_widget_set_sensitive(kind_row->allow_button, site != NULL);
  gtk_widget_set_sensitive(kind_row->block_button, site != NULL);
}

/* Searching opens every permission it has something to show under, so what the
 * user had open before is put back when they stop searching rather than left
 * however the search happened to leave it. */
static void wig_settings_permissions_search_changed(WigSettingsPermissions *self, GtkSearchEntry *entry)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  gboolean was_searching = self->filter != NULL;

  g_clear_pointer(&self->filter, g_free);
  /* Sites are kept as origins, which are already lowercase, so only what was
   * typed has to be brought down to meet them. */
  if (text && *text)
    self->filter = g_ascii_strdown(text, -1);

  if (!was_searching && self->filter) {
    for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++)
      self->kinds[i].expanded_before_search = adw_expander_row_get_expanded(self->kinds[i].row);
  } else if (was_searching && !self->filter) {
    for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++)
      adw_expander_row_set_expanded(self->kinds[i].row, self->kinds[i].expanded_before_search);
  }

  wig_settings_permissions_sync(self);
}

static void wig_settings_permissions_build_kind_row(WigSettingsPermissions *self, AdwPreferencesGroup *group,
                                                    WigPermissionKind kind)
{
  KindRow *kind_row = &self->kinds[wig_permission_kind_index(kind)];

  kind_row->pane = self;
  kind_row->kind = kind;
  kind_row->rules = g_ptr_array_new();

  kind_row->row = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(kind_row->row), wig_permission_kind_get_label(kind));
  adw_expander_row_add_prefix(kind_row->row, gtk_image_new_from_icon_name(wig_permission_kind_get_icon_name(kind)));

  /* Adding is in the same place under every permission, above answers that come
   * and go, so it does not move about as rules are made and unmade. */
  kind_row->entry = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(kind_row->entry), "Add a Site");
  g_signal_connect(kind_row->entry, "changed", G_CALLBACK(kind_row_entry_changed), kind_row);

  kind_row->allow_button = gtk_button_new_with_label("Allow");
  gtk_widget_set_valign(kind_row->allow_button, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(kind_row->allow_button, FALSE);
  gtk_widget_add_css_class(kind_row->allow_button, "suggested-action");
  g_signal_connect(kind_row->allow_button, "clicked", G_CALLBACK(kind_row_allow_clicked), kind_row);
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(kind_row->entry), kind_row->allow_button);

  kind_row->block_button = gtk_button_new_with_label("Block");
  gtk_widget_set_valign(kind_row->block_button, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(kind_row->block_button, FALSE);
  gtk_widget_add_css_class(kind_row->block_button, "destructive-action");
  g_signal_connect(kind_row->block_button, "clicked", G_CALLBACK(kind_row_block_clicked), kind_row);
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(kind_row->entry), kind_row->block_button);

  adw_expander_row_add_row(kind_row->row, kind_row->entry);

  kind_row->status = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(kind_row->status), "No sites");
  adw_expander_row_add_row(kind_row->row, kind_row->status);

  adw_preferences_group_add(group, GTK_WIDGET(kind_row->row));
}

/* The sites under a permission are answers rather than settings, so only the
 * permissions themselves are worth finding from the settings search. */
void wig_settings_permissions_index(WigSettingsSearch *search, const char *pane, const char *pane_title)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1)
    wig_settings_search_add(search, wig_permission_kind_get_label(kind), NULL, pane, pane_title);
}

static void wig_settings_permissions_dispose(GObject *object)
{
  WigSettingsPermissions *self = WIG_SETTINGS_PERMISSIONS(object);

  g_clear_handle_id(&self->sync_idle_id, g_source_remove);
  g_clear_pointer(&self->toolbar, gtk_widget_unparent);
  g_clear_pointer(&self->filter, g_free);

  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++)
    g_clear_pointer(&self->kinds[i].rules, g_ptr_array_unref);

  G_OBJECT_CLASS(wig_settings_permissions_parent_class)->dispose(object);
}

static void wig_settings_permissions_class_init(WigSettingsPermissionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_permissions_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-permissions");
}

static void wig_settings_permissions_init(WigSettingsPermissions *self)
{
  self->manager = wig_application_get_permissions_manager(wig_application_get());

  GtkWidget *page = adw_preferences_page_new();

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, "Site Permissions");
  adw_preferences_group_set_description(
      group, "What sites have been allowed or blocked. A site with no rule here is asked the first time it needs one.");

  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1)
    wig_settings_permissions_build_kind_row(self, group, kind);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), group);

  /* The pane's own search is for the sites under the permissions, which the
   * settings search does not reach: it looks for settings, and a site is an
   * answer to one rather than a setting of its own. */
  self->search_entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->search_entry, TRUE);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->search_entry), "Search Sites");
  g_signal_connect_object(self->search_entry, "search-changed", G_CALLBACK(wig_settings_permissions_search_changed),
                          self, G_CONNECT_SWAPPED);

  /* Held to the same width the rows are, so the entry searching them lines up
   * with them rather than with the pane. */
  GtkWidget *entry_clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(entry_clamp), self->search_entry);
  gtk_widget_set_margin_top(entry_clamp, 12);
  gtk_widget_set_margin_bottom(entry_clamp, 6);
  gtk_widget_set_margin_start(entry_clamp, 12);
  gtk_widget_set_margin_end(entry_clamp, 12);

  self->toolbar = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(self->toolbar), ADW_TOOLBAR_FLAT);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(self->toolbar), entry_clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(self->toolbar), page);
  gtk_widget_set_parent(self->toolbar, GTK_WIDGET(self));

  g_signal_connect_object(self->manager, "changed", G_CALLBACK(wig_settings_permissions_queue_sync), self,
                          G_CONNECT_SWAPPED);

  wig_settings_permissions_sync(self);
}

GtkWidget *wig_settings_permissions_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_PERMISSIONS, NULL);
}
