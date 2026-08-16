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
  GtkWidget *new_button;
  GtkWidget *entry;
  GtkWidget *allow_button;
  GtkWidget *block_button;
  gboolean expanded_before_search;
  GPtrArray *rules; /* borrowed rows currently under the expander */
} KindRow;

/* Autoplay and HTTPS navigation are both one choice out of a few, made for a
 * site, so a section says which choices it offers and where they are kept rather
 * than being written out twice. A choice is the enumeration value the dropdown
 * lists at that position. */
typedef struct {
  const char *const *labels;
  gboolean (*get)(WigPermissionsManager *manager, const char *site, guint *choice);
  void (*set)(WigPermissionsManager *manager, const char *site, guint choice);
  void (*clear)(WigPermissionsManager *manager, const char *site);
  GList *(*list)(WigPermissionsManager *manager, guint choice);
} ChoicePolicy;

/* Autoplay, HTTPS navigation and the user agent are not permissions: the site
 * never asks for them and is never told about them, they are how the browser has
 * been told to treat it. A user agent is a line of text rather than a choice, so
 * that section has no descriptor of its own. */
typedef struct {
  WigSettingsPermissions *pane; /* borrowed */
  const ChoicePolicy *choice;
  AdwExpanderRow *row;
  GtkWidget *status;
  GtkWidget *new_button;
  GtkWidget *site_entry;
  GtkWidget *value;
  GtkWidget *add_button;
  gboolean expanded_before_search;
  GPtrArray *rules; /* borrowed rows currently under the expander */
} PolicyRow;

struct _WigSettingsPermissions {
  GtkWidget parent;

  WigPermissionsManager *manager; /* borrowed from application */

  GtkWidget *toolbar;
  GtkWidget *search_entry;
  char *filter;
  KindRow kinds[WIG_PERMISSION_N_KINDS];
  PolicyRow autoplay;
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  PolicyRow https_navigation;
#endif
  PolicyRow user_agent;
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

static GtkWidget *section_new_button(AdwExpanderRow *row)
{
  GtkWidget *button = gtk_button_new_from_icon_name("list-add-symbolic");

  gtk_widget_set_tooltip_text(button, "Add a Site");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(button, "flat");
  adw_expander_row_add_suffix(row, button);

  return button;
}

/* A row that has only just been revealed cannot take the focus yet, so it is
 * given the focus once the expander has finished putting it on screen. */
static void section_focus_new_rule(gpointer data)
{
  g_autoptr(GtkWidget) entry = data;

  gtk_widget_grab_focus(entry);
}

static void section_begin_new_rule(AdwExpanderRow *row, GtkWidget *entry, GtkWidget *extra)
{
  gtk_widget_set_visible(entry, TRUE);
  if (extra)
    gtk_widget_set_visible(extra, TRUE);

  /* The row being added to may well be shut, and an empty entry inside a shut
   * row is an answer to nothing. */
  adw_expander_row_set_expanded(row, TRUE);
  g_idle_add_once(section_focus_new_rule, g_object_ref(entry));
}

/* A rule that has been made is a row of its own, so what was typed to make it
 * goes away rather than sitting there half-filled. */
static void section_end_new_rule(GtkWidget *entry, GtkWidget *extra)
{
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  gtk_widget_set_visible(entry, FALSE);

  if (extra) {
    gtk_editable_set_text(GTK_EDITABLE(extra), "");
    gtk_widget_set_visible(extra, FALSE);
  }
}

static gboolean section_hides_site(WigSettingsPermissions *self, const char *site)
{
  return self->filter && !strstr(site, self->filter);
}

/* Every section is a list of sites under one heading, so what it says when it
 * has nothing to show, and whether the search has left it worth showing at all,
 * is the same question wherever it is asked. */
static void section_finish_sync(WigSettingsPermissions *self, AdwExpanderRow *row, GtkWidget *status, guint shown,
                                const char *summary)
{
  adw_expander_row_set_subtitle(row, summary);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(status), self->filter ? "No matching sites" : "No sites");
  gtk_widget_set_visible(status, shown == 0);

  /* Searching narrows the pane to the sections that have something to show,
   * since a section with nothing under it is not an answer to the search, and
   * opens the ones that are left: a match nobody can see is no answer either. */
  gtk_widget_set_visible(GTK_WIDGET(row), !self->filter || shown > 0);
  if (self->filter && shown > 0)
    adw_expander_row_set_expanded(row, TRUE);
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
    if (section_hides_site(self, rule->site))
      continue;

    GtkWidget *row = permissions_rule_row(kind_row, rule);
    adw_expander_row_add_row(kind_row->row, row);
    g_ptr_array_add(kind_row->rules, row);
  }

  g_autofree char *summary = permissions_summary(allowed, blocked);
  section_finish_sync(self, kind_row->row, kind_row->status, kind_row->rules->len, summary);
}

static void choice_row_sync(PolicyRow *policy);
static void user_agent_row_sync(WigSettingsPermissions *self);

static void wig_settings_permissions_sync(WigSettingsPermissions *self)
{
  for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++)
    kind_row_sync(&self->kinds[i]);

  choice_row_sync(&self->autoplay);
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  choice_row_sync(&self->https_navigation);
#endif
  user_agent_row_sync(self);
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
  section_end_new_rule(kind_row->entry, NULL);
}

static void kind_row_new_clicked(GtkButton *button, KindRow *kind_row)
{
  section_begin_new_rule(kind_row->row, kind_row->entry, NULL);
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

/* The dropdown lists the autoplay policies in the order the enumeration gives
 * them, so what is selected is the policy itself. */
static const char *const autoplay_labels[] = { "Allow", "Allow Without Sound", "Block", NULL };

static gboolean autoplay_get(WigPermissionsManager *manager, const char *site, guint *choice)
{
  WebKitAutoplayPolicy autoplay = WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND;

  if (!wig_permissions_manager_get_autoplay(manager, site, &autoplay))
    return FALSE;

  *choice = (guint)autoplay;
  return TRUE;
}

static void autoplay_set(WigPermissionsManager *manager, const char *site, guint choice)
{
  wig_permissions_manager_set_autoplay(manager, site, (WebKitAutoplayPolicy)choice);
}

static GList *autoplay_list(WigPermissionsManager *manager, guint choice)
{
  return wig_permissions_manager_list_autoplay_sites(manager, (WebKitAutoplayPolicy)choice);
}

static const ChoicePolicy autoplay_policy = {
  autoplay_labels, autoplay_get, autoplay_set, wig_permissions_manager_clear_autoplay, autoplay_list,
};

#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
/* Named as the setting in Browsing names them, since a rule here is that setting
 * answered differently for one site. */
static const char *const https_navigation_labels[] = { "Off", "HTTPS-First", "HTTPS-Only", NULL };

static gboolean https_navigation_get(WigPermissionsManager *manager, const char *site, guint *choice)
{
  WebKitHTTPSNavigationPolicy https_navigation = WEBKIT_HTTPS_NAVIGATION_POLICY_KEEP_AS_REQUESTED;

  if (!wig_permissions_manager_get_https_navigation(manager, site, &https_navigation))
    return FALSE;

  *choice = (guint)https_navigation;
  return TRUE;
}

static void https_navigation_set(WigPermissionsManager *manager, const char *site, guint choice)
{
  wig_permissions_manager_set_https_navigation(manager, site, (WebKitHTTPSNavigationPolicy)choice);
}

static GList *https_navigation_list(WigPermissionsManager *manager, guint choice)
{
  return wig_permissions_manager_list_https_navigation_sites(manager, (WebKitHTTPSNavigationPolicy)choice);
}

static const ChoicePolicy https_navigation_policy = {
  https_navigation_labels, https_navigation_get, https_navigation_set, wig_permissions_manager_clear_https_navigation,
  https_navigation_list,
};
#endif

typedef struct {
  char *site;
  guint choice;
} ChoiceRule;

static void choice_rule_clear(ChoiceRule *rule)
{
  g_clear_pointer(&rule->site, g_free);
}

static int compare_choice_rules(gconstpointer first, gconstpointer second)
{
  return g_strcmp0(((const ChoiceRule *)first)->site, ((const ChoiceRule *)second)->site);
}

static void choice_rule_changed(GtkDropDown *dropdown, GParamSpec *pspec, PolicyRow *policy)
{
  policy->choice->set(policy->pane->manager, gtk_widget_get_name(GTK_WIDGET(dropdown)),
                      gtk_drop_down_get_selected(dropdown));
}

static void choice_rule_removed(GtkButton *button, PolicyRow *policy)
{
  policy->choice->clear(policy->pane->manager, gtk_widget_get_name(GTK_WIDGET(button)));
}

static GtkWidget *choice_rule_row(PolicyRow *policy, const ChoiceRule *rule)
{
  GtkWidget *row = adw_action_row_new();

  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), rule->site);

  GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_name(remove, rule->site);
  gtk_widget_set_tooltip_text(remove, "Treat Like Any Other Site");
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(remove, "flat");
  g_signal_connect(remove, "clicked", G_CALLBACK(choice_rule_removed), policy);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

  GtkWidget *dropdown = gtk_drop_down_new_from_strings(policy->choice->labels);
  gtk_widget_set_name(dropdown, rule->site);
  gtk_widget_set_valign(dropdown, GTK_ALIGN_CENTER);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), rule->choice);
  g_signal_connect(dropdown, "notify::selected", G_CALLBACK(choice_rule_changed), policy);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), dropdown);

  return row;
}

static char *policy_summary(guint sites)
{
  return sites ? g_strdup_printf("%u site%s", sites, sites == 1 ? "" : "s") : g_strdup("No sites");
}

static void choice_row_sync(PolicyRow *policy)
{
  WigSettingsPermissions *self = policy->pane;

  for (guint i = 0; i < policy->rules->len; i++)
    adw_expander_row_remove(policy->row, g_ptr_array_index(policy->rules, i));

  g_ptr_array_set_size(policy->rules, 0);

  g_autoptr(GArray) rules = g_array_new(FALSE, FALSE, sizeof(ChoiceRule));
  g_array_set_clear_func(rules, (GDestroyNotify)choice_rule_clear);

  for (guint choice = 0; policy->choice->labels[choice]; choice++) {
    GList *sites = policy->choice->list(self->manager, choice);

    for (GList *l = sites; l; l = l->next) {
      ChoiceRule rule = { l->data, choice };
      g_array_append_val(rules, rule);
    }

    g_list_free(sites);
  }

  g_array_sort(rules, compare_choice_rules);

  for (guint i = 0; i < rules->len; i++) {
    const ChoiceRule *rule = &g_array_index(rules, ChoiceRule, i);
    if (section_hides_site(self, rule->site))
      continue;

    GtkWidget *row = choice_rule_row(policy, rule);
    adw_expander_row_add_row(policy->row, row);
    g_ptr_array_add(policy->rules, row);
  }

  g_autofree char *summary = policy_summary(rules->len);
  section_finish_sync(self, policy->row, policy->status, policy->rules->len, summary);
}

static void user_agent_rule_applied(AdwEntryRow *entry, WigSettingsPermissions *self)
{
  wig_permissions_manager_set_user_agent(self->manager, gtk_widget_get_name(GTK_WIDGET(entry)),
                                         gtk_editable_get_text(GTK_EDITABLE(entry)));
}

static void user_agent_rule_removed(GtkButton *button, WigSettingsPermissions *self)
{
  wig_permissions_manager_set_user_agent(self->manager, gtk_widget_get_name(GTK_WIDGET(button)), NULL);
}

/* The user agent is a line of text rather than a choice, so the row it lives in
 * is the one that edits it: what is typed is not the rule until it is applied. */
static GtkWidget *user_agent_rule_row(WigSettingsPermissions *self, const char *site, const char *user_agent)
{
  GtkWidget *row = adw_entry_row_new();

  gtk_widget_set_name(row, site);
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), site);
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), TRUE);
  gtk_editable_set_text(GTK_EDITABLE(row), user_agent);
  g_signal_connect(row, "apply", G_CALLBACK(user_agent_rule_applied), self);

  GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_name(remove, site);
  gtk_widget_set_tooltip_text(remove, "Tell This Site What Every Other Site Is Told");
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(remove, "flat");
  g_signal_connect(remove, "clicked", G_CALLBACK(user_agent_rule_removed), self);
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(row), remove);

  return row;
}

static void user_agent_row_sync(WigSettingsPermissions *self)
{
  PolicyRow *policy = &self->user_agent;

  for (guint i = 0; i < policy->rules->len; i++)
    adw_expander_row_remove(policy->row, g_ptr_array_index(policy->rules, i));

  g_ptr_array_set_size(policy->rules, 0);

  GList *sites = wig_permissions_manager_list_user_agent_sites(self->manager);
  guint total = g_list_length(sites);

  for (GList *l = sites; l; l = l->next) {
    if (section_hides_site(self, l->data))
      continue;

    GtkWidget *row = user_agent_rule_row(self, l->data, wig_permissions_manager_get_user_agent(self->manager, l->data));
    adw_expander_row_add_row(policy->row, row);
    g_ptr_array_add(policy->rules, row);
  }

  g_list_free_full(sites, g_free);

  g_autofree char *summary = policy_summary(total);
  section_finish_sync(self, policy->row, policy->status, policy->rules->len, summary);
}

static void choice_add_clicked(GtkButton *button, PolicyRow *policy)
{
  g_autofree char *site = permissions_site_for_text(gtk_editable_get_text(GTK_EDITABLE(policy->site_entry)));
  if (!site)
    return;

  policy->choice->set(policy->pane->manager, site, gtk_drop_down_get_selected(GTK_DROP_DOWN(policy->value)));
  section_end_new_rule(policy->site_entry, NULL);
}

static void choice_new_clicked(GtkButton *button, PolicyRow *policy)
{
  section_begin_new_rule(policy->row, policy->site_entry, NULL);
}

static void user_agent_add_clicked(GtkButton *button, WigSettingsPermissions *self)
{
  PolicyRow *policy = &self->user_agent;
  g_autofree char *site = permissions_site_for_text(gtk_editable_get_text(GTK_EDITABLE(policy->site_entry)));
  const char *user_agent = gtk_editable_get_text(GTK_EDITABLE(policy->value));
  if (!site || !*user_agent)
    return;

  wig_permissions_manager_set_user_agent(self->manager, site, user_agent);
  section_end_new_rule(policy->site_entry, policy->value);
}

static void user_agent_new_clicked(GtkButton *button, WigSettingsPermissions *self)
{
  section_begin_new_rule(self->user_agent.row, self->user_agent.site_entry, self->user_agent.value);
}

static void choice_add_changed(GtkEditable *entry, PolicyRow *policy)
{
  g_autofree char *site = permissions_site_for_text(gtk_editable_get_text(entry));

  gtk_widget_set_sensitive(policy->add_button, site != NULL);
}

/* A site told nothing in particular is a site with no rule, so both halves have
 * to be filled in before there is anything to add. */
static void user_agent_add_changed(GtkEditable *entry, WigSettingsPermissions *self)
{
  PolicyRow *policy = &self->user_agent;
  g_autofree char *site = permissions_site_for_text(gtk_editable_get_text(GTK_EDITABLE(policy->site_entry)));
  const char *user_agent = gtk_editable_get_text(GTK_EDITABLE(policy->value));

  gtk_widget_set_sensitive(policy->add_button, site != NULL && *user_agent != '\0');
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

  PolicyRow *policies[] = {
    &self->autoplay,
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
    &self->https_navigation,
#endif
    &self->user_agent,
  };

  if (!was_searching && self->filter) {
    for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++)
      self->kinds[i].expanded_before_search = adw_expander_row_get_expanded(self->kinds[i].row);
    for (guint i = 0; i < G_N_ELEMENTS(policies); i++)
      policies[i]->expanded_before_search = adw_expander_row_get_expanded(policies[i]->row);
  } else if (was_searching && !self->filter) {
    for (guint i = 0; i < WIG_PERMISSION_N_KINDS; i++)
      adw_expander_row_set_expanded(self->kinds[i].row, self->kinds[i].expanded_before_search);
    for (guint i = 0; i < G_N_ELEMENTS(policies); i++)
      adw_expander_row_set_expanded(policies[i]->row, policies[i]->expanded_before_search);
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

  kind_row->new_button = section_new_button(kind_row->row);
  g_signal_connect(kind_row->new_button, "clicked", G_CALLBACK(kind_row_new_clicked), kind_row);

  /* Adding is in the same place under every permission, above answers that come
   * and go, so it does not move about as rules are made and unmade. */
  kind_row->entry = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(kind_row->entry), "Add a Site");
  gtk_widget_set_visible(kind_row->entry, FALSE);
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

/* Adding is in the same place under every policy, above the rules that come and
 * go, so it does not move about as they are made and unmade. What goes between
 * the site and the button is what the policy is worth saying about a site. */
static void policy_row_build(PolicyRow *policy, AdwPreferencesGroup *group, const char *title, const char *icon_name)
{
  policy->rules = g_ptr_array_new();

  policy->row = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(policy->row), title);
  adw_expander_row_add_prefix(policy->row, gtk_image_new_from_icon_name(icon_name));

  policy->new_button = section_new_button(policy->row);

  policy->site_entry = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(policy->site_entry), "Add a Site");
  gtk_widget_set_visible(policy->site_entry, FALSE);
  adw_expander_row_add_row(policy->row, policy->site_entry);

  policy->add_button = gtk_button_new_with_label("Add");
  gtk_widget_set_valign(policy->add_button, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(policy->add_button, FALSE);

  policy->status = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(policy->status), "No sites");

  adw_preferences_group_add(group, GTK_WIDGET(policy->row));
}

static void policy_row_finish(PolicyRow *policy)
{
  adw_expander_row_add_row(policy->row, policy->status);
}

static void wig_settings_permissions_build_choice_row(WigSettingsPermissions *self, PolicyRow *policy,
                                                      const ChoicePolicy *choice, AdwPreferencesGroup *group,
                                                      const char *title, const char *icon_name, guint default_choice)
{
  policy_row_build(policy, group, title, icon_name);
  policy->pane = self;
  policy->choice = choice;

  /* Which way it goes is chosen before the site is added rather than after, so a
   * site is never briefly given something nobody asked for. */
  policy->value = gtk_drop_down_new_from_strings(choice->labels);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(policy->value), default_choice);
  gtk_widget_set_valign(policy->value, GTK_ALIGN_CENTER);
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(policy->site_entry), policy->value);
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(policy->site_entry), policy->add_button);

  g_signal_connect(policy->site_entry, "changed", G_CALLBACK(choice_add_changed), policy);
  g_signal_connect(policy->add_button, "clicked", G_CALLBACK(choice_add_clicked), policy);
  g_signal_connect(policy->new_button, "clicked", G_CALLBACK(choice_new_clicked), policy);

  policy_row_finish(policy);
}

static void wig_settings_permissions_build_user_agent_row(WigSettingsPermissions *self, AdwPreferencesGroup *group)
{
  PolicyRow *policy = &self->user_agent;

  policy_row_build(policy, group, "User Agent", "computer-symbolic");
  policy->pane = self;

  /* A user agent is a line of text rather than a choice, so it needs a row of
   * its own under the site it is for. */
  policy->value = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(policy->value), "User Agent");
  gtk_widget_set_visible(policy->value, FALSE);
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(policy->value), policy->add_button);
  adw_expander_row_add_row(policy->row, policy->value);

  g_signal_connect(policy->site_entry, "changed", G_CALLBACK(user_agent_add_changed), self);
  g_signal_connect(policy->value, "changed", G_CALLBACK(user_agent_add_changed), self);
  g_signal_connect(policy->add_button, "clicked", G_CALLBACK(user_agent_add_clicked), self);
  g_signal_connect(policy->new_button, "clicked", G_CALLBACK(user_agent_new_clicked), self);

  policy_row_finish(policy);
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

  g_clear_pointer(&self->autoplay.rules, g_ptr_array_unref);
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  g_clear_pointer(&self->https_navigation.rules, g_ptr_array_unref);
#endif
  g_clear_pointer(&self->user_agent.rules, g_ptr_array_unref);

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

  AdwPreferencesGroup *policies = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(policies, "Site Policies");
  adw_preferences_group_set_description(policies,
                                        "How wig treats a site whether or not the site asks. A site with no rule "
                                        "here is treated like every other one.");

  wig_settings_permissions_build_choice_row(self, &self->autoplay, &autoplay_policy, policies, "Autoplay",
                                            "media-playback-start-symbolic", (guint)WEBKIT_AUTOPLAY_DENY);
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  wig_settings_permissions_build_choice_row(self, &self->https_navigation, &https_navigation_policy, policies,
                                            "HTTPS Navigation", "channel-secure-symbolic",
                                            (guint)WEBKIT_HTTPS_NAVIGATION_POLICY_HTTPS_ONLY);
#endif
  wig_settings_permissions_build_user_agent_row(self, policies);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), policies);

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
