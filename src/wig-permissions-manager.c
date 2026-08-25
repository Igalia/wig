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

#include "wig-permissions-manager.h"

#include "wig-key-file-utils.h"

#include <errno.h>
#include <glib/gstdio.h>

#define WIG_PERMISSIONS_FORMAT_VERSION 1
#define WIG_PERMISSIONS_GROUP "Permissions"
#define WIG_PERMISSIONS_SAVE_DELAY_SECONDS 5
#define WIG_PERMISSIONS_VISIT_PRECISION_SECONDS (7 * 24 * 60 * 60)
#define WIG_PERMISSIONS_EXPIRY_SECONDS (60 * 24 * 60 * 60)
#define WIG_PERMISSIONS_EXPIRY_INTERVAL_SECONDS (24 * 60 * 60)

/* Notifications are exempt as they are in Chromium. */
#define WIG_PERMISSIONS_EXPIRABLE_KINDS (WIG_PERMISSION_ALL_KINDS ^ WIG_PERMISSION_NOTIFICATION)

typedef struct {
  WigPermissionsManager *manager; /* borrowed */
  char *origin;
  WigPermissions *permissions;
  gboolean has_autoplay;
  WebKitAutoplayPolicy autoplay;
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  gboolean has_https_navigation;
  WebKitHTTPSNavigationPolicy https_navigation;
#endif
  char *user_agent;
  gint64 last_visited;
  gulong notify_id;
} WigPermissionOrigin;

struct _WigPermissionsManager {
  GObject parent;

  char *path;
  GHashTable *origins; /* borrowed char* origin -> owned WigPermissionOrigin* */
  guint save_timeout_id;
  guint expire_timeout_id;
  gboolean dirty;
  gboolean unsupported_format;
};

G_DEFINE_FINAL_TYPE(WigPermissionsManager, wig_permissions_manager, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static gint64 current_timestamp(void)
{
  return g_get_real_time() / G_USEC_PER_SEC;
}

/* Visit times only decide whether an origin has gone untouched for months, so
 * they are floored to a week. This exposes less information that can be inferred
 * from permissions. This matches Chromium's behavior. */
static gint64 current_visit_timestamp(void)
{
  gint64 timestamp = current_timestamp();
  return timestamp - (timestamp % WIG_PERMISSIONS_VISIT_PRECISION_SECONDS);
}

static WigPermissionKind permission_kind_for_property(const char *property_name)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    if (g_str_equal(property_name, wig_permission_kind_get_property_name(kind)))
      return kind;
  }
  return 0;
}

static gboolean permission_origin_has_decisions(WigPermissionOrigin *record)
{
  for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
    if (wig_permissions_get_state(record->permissions, kind) != WEBKIT_PERMISSION_STATE_PROMPT)
      return TRUE;
  }
  return FALSE;
}

static gboolean permission_origin_has_policies(WigPermissionOrigin *record)
{
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
  if (record->has_https_navigation)
    return TRUE;
#endif

  return record->has_autoplay || record->user_agent != NULL;
}

static gboolean permission_origin_has_data(WigPermissionOrigin *record)
{
  return record->last_visited || permission_origin_has_decisions(record) || permission_origin_has_policies(record);
}

static void permission_origin_free(WigPermissionOrigin *record)
{
  if (record->notify_id)
    g_signal_handler_disconnect(record->permissions, record->notify_id);
  g_clear_object(&record->permissions);
  g_free(record->user_agent);
  g_free(record->origin);
  g_free(record);
}

static void wig_permissions_manager_queue_save(WigPermissionsManager *self);

static void permission_state_changed(WigPermissions *permissions, GParamSpec *pspec, WigPermissionOrigin *record)
{
  if (!permission_kind_for_property(g_param_spec_get_name(pspec)))
    return;

  record->manager->dirty = TRUE;
  wig_permissions_manager_queue_save(record->manager);
  g_signal_emit(record->manager, signals[SIGNAL_CHANGED], 0, record->origin);
}

static WigPermissionOrigin *permission_origin_new(WigPermissionsManager *self, const char *origin)
{
  WigPermissionOrigin *record = g_new0(WigPermissionOrigin, 1);
  record->manager = self;
  record->origin = g_strdup(origin);
  record->permissions = wig_permissions_new();
  return record;
}

static void permission_origin_connect(WigPermissionOrigin *record)
{
  record->notify_id = g_signal_connect(record->permissions, "notify", G_CALLBACK(permission_state_changed), record);
}

static char *permission_origin_group_name(const char *origin)
{
  return g_uri_escape_string(origin, ":/.-_~", FALSE);
}

static const char *permission_state_to_string(WebKitPermissionState state)
{
  switch (state) {
  case WEBKIT_PERMISSION_STATE_GRANTED:
    return "granted";
  case WEBKIT_PERMISSION_STATE_DENIED:
    return "denied";
  case WEBKIT_PERMISSION_STATE_PROMPT:
  default:
    return "prompt";
  }
}

static const char *autoplay_to_string(WebKitAutoplayPolicy autoplay)
{
  switch (autoplay) {
  case WEBKIT_AUTOPLAY_ALLOW:
    return "allow";
  case WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND:
    return "allow-without-sound";
  case WEBKIT_AUTOPLAY_DENY:
    return "deny";
  }

  return "allow-without-sound";
}

static gboolean autoplay_from_string(const char *autoplay, WebKitAutoplayPolicy *result)
{
  if (g_strcmp0(autoplay, "allow") == 0)
    *result = WEBKIT_AUTOPLAY_ALLOW;
  else if (g_strcmp0(autoplay, "allow-without-sound") == 0)
    *result = WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND;
  else if (g_strcmp0(autoplay, "deny") == 0)
    *result = WEBKIT_AUTOPLAY_DENY;
  else
    return FALSE;

  return TRUE;
}

#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
/* Named as the setting's own values are, so a site overriding it reads the same
 * way in the file as the setting it is overriding. */
static const char *https_navigation_to_string(WebKitHTTPSNavigationPolicy https_navigation)
{
  switch (https_navigation) {
  case WEBKIT_HTTPS_NAVIGATION_POLICY_HTTPS_FIRST:
    return "https-first";
  case WEBKIT_HTTPS_NAVIGATION_POLICY_HTTPS_ONLY:
    return "https-only";
  case WEBKIT_HTTPS_NAVIGATION_POLICY_KEEP_AS_REQUESTED:
    break;
  }

  return "keep-as-requested";
}

static gboolean https_navigation_from_string(const char *https_navigation, WebKitHTTPSNavigationPolicy *result)
{
  if (g_strcmp0(https_navigation, "keep-as-requested") == 0)
    *result = WEBKIT_HTTPS_NAVIGATION_POLICY_KEEP_AS_REQUESTED;
  else if (g_strcmp0(https_navigation, "https-first") == 0)
    *result = WEBKIT_HTTPS_NAVIGATION_POLICY_HTTPS_FIRST;
  else if (g_strcmp0(https_navigation, "https-only") == 0)
    *result = WEBKIT_HTTPS_NAVIGATION_POLICY_HTTPS_ONLY;
  else
    return FALSE;

  return TRUE;
}
#endif

static gboolean permission_state_from_string(const char *state, WebKitPermissionState *result)
{
  if (!state) {
    *result = WEBKIT_PERMISSION_STATE_PROMPT;
    return TRUE;
  }
  if (g_str_equal(state, "granted")) {
    *result = WEBKIT_PERMISSION_STATE_GRANTED;
    return TRUE;
  }
  if (g_str_equal(state, "denied")) {
    *result = WEBKIT_PERMISSION_STATE_DENIED;
    return TRUE;
  }
  if (g_str_equal(state, "prompt")) {
    *result = WEBKIT_PERMISSION_STATE_PROMPT;
    return TRUE;
  }
  return FALSE;
}

/* Grants are given up by origins the user has stopped visiting, so a permission
 * handed out once cannot outlive any interest in the site. Denials are kept:
 * they are a standing answer, not something the user needs re-asked about. */
static void wig_permissions_manager_expire(WigPermissionsManager *self)
{
  if (self->unsupported_format)
    return;

  /* Visit times are floored to a week, so an origin survives between 60 and 67
   * days of disuse. Erring late keeps a grant from lapsing early. */
  gint64 threshold = current_timestamp() - WIG_PERMISSIONS_EXPIRY_SECONDS;
  guint expired = 0;
  guint forgotten = 0;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->origins);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    WigPermissionOrigin *record = value;
    if (!record->last_visited || record->last_visited > threshold)
      continue;

    for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
      if (!(WIG_PERMISSIONS_EXPIRABLE_KINDS & kind)
          || wig_permissions_get_state(record->permissions, kind) != WEBKIT_PERMISSION_STATE_GRANTED) {
        continue;
      }

      g_debug("permissions: expiring %s for %s, unvisited since %" G_GINT64_FORMAT,
              wig_permission_kind_get_property_name(kind), record->origin, record->last_visited);
      wig_permissions_set_state(record->permissions, kind, WEBKIT_PERMISSION_STATE_PROMPT);
      expired++;
    }

    /* With no answer left to remember there is no reason to keep recording that
     * the origin was ever visited, unless it is still being treated specially. */
    if (!permission_origin_has_decisions(record) && !permission_origin_has_policies(record)) {
      g_hash_table_iter_remove(&iter);
      forgotten++;
    }
  }

  if (expired || forgotten) {
    g_debug("permissions: expired %u permission(s), forgot %u origin(s)", expired, forgotten);
    self->dirty = TRUE;
    wig_permissions_manager_queue_save(self);
  }
}

static gboolean wig_permissions_manager_expire_timeout(gpointer user_data)
{
  wig_permissions_manager_expire(WIG_PERMISSIONS_MANAGER(user_data));
  return G_SOURCE_CONTINUE;
}

gboolean wig_permissions_manager_load(WigPermissionsManager *self, GError **error)
{
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GError) local_error = NULL;
  if (!g_key_file_load_from_file(key_file, self->path, G_KEY_FILE_NONE, &local_error)) {
    if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      return TRUE;
    g_propagate_prefixed_error(error, g_steal_pointer(&local_error), "cannot read '%s': ", self->path);
    return FALSE;
  }

  int version = g_key_file_get_integer(key_file, WIG_PERMISSIONS_GROUP, "version", &local_error);
  if (local_error) {
    g_propagate_prefixed_error(error, g_steal_pointer(&local_error),
                               "cannot read format version from '%s': ", self->path);
    return FALSE;
  }
  if (version != WIG_PERMISSIONS_FORMAT_VERSION) {
    /* A file from a newer wig is left untouched rather than downgraded. */
    self->unsupported_format = TRUE;
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_PARSE,
                "'%s' is in format version %d, not %d; it will not be modified", self->path, version,
                WIG_PERMISSIONS_FORMAT_VERSION);
    return FALSE;
  }

  gsize n_groups;
  g_auto(GStrv) groups = g_key_file_get_groups(key_file, &n_groups);
  for (gsize i = 0; i < n_groups; i++) {
    if (g_str_equal(groups[i], WIG_PERMISSIONS_GROUP))
      continue;

    g_autofree char *origin = g_uri_unescape_string(groups[i], NULL);
    if (!origin) {
      g_warning("permissions: ignoring invalid origin group '%s'", groups[i]);
      continue;
    }

    if (!*origin) {
      g_warning("permissions: ignoring empty origin group");
      continue;
    }

    WigPermissionOrigin *record = permission_origin_new(self, origin);
    record->last_visited = MAX(0, wig_key_file_get_int64(key_file, groups[i], "last-visited", 0));
    for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
      const char *name = wig_permission_kind_get_property_name(kind);
      g_autoptr(GError) state_error = NULL;
      g_autofree char *state = g_key_file_get_string(key_file, groups[i], name, &state_error);
      if (state_error && !g_error_matches(state_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        g_warning("permissions: invalid %s.%s: %s", groups[i], name, state_error->message);
      WebKitPermissionState parsed_state;
      if (!permission_state_from_string(state, &parsed_state)) {
        g_warning("permissions: ignoring invalid %s.%s value '%s'", groups[i], name, state);
        parsed_state = WEBKIT_PERMISSION_STATE_PROMPT;
      }
      wig_permissions_set_state(record->permissions, kind, parsed_state);
    }

    g_autofree char *autoplay = g_key_file_get_string(key_file, groups[i], "autoplay", NULL);
    if (autoplay) {
      record->has_autoplay = autoplay_from_string(autoplay, &record->autoplay);
      if (!record->has_autoplay)
        g_warning("permissions: ignoring invalid %s.autoplay value '%s'", groups[i], autoplay);
    }

#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
    g_autofree char *https_navigation = g_key_file_get_string(key_file, groups[i], "https-navigation", NULL);
    if (https_navigation) {
      record->has_https_navigation = https_navigation_from_string(https_navigation, &record->https_navigation);
      if (!record->has_https_navigation)
        g_warning("permissions: ignoring invalid %s.https-navigation value '%s'", groups[i], https_navigation);
    }
#endif

    record->user_agent = g_key_file_get_string(key_file, groups[i], "user-agent", NULL);
    if (record->user_agent && !*record->user_agent)
      g_clear_pointer(&record->user_agent, g_free);

    if (!permission_origin_has_data(record)) {
      permission_origin_free(record);
      continue;
    }

    /* An entry with no visit recorded has nothing to measure disuse against, so
     * count this run as the visit rather than let it sit un-expirable. */
    if (!record->last_visited) {
      record->last_visited = current_visit_timestamp();
      self->dirty = TRUE;
    }

    permission_origin_connect(record);
    g_hash_table_insert(self->origins, g_strdup(record->origin), record);
  }

  g_debug("permissions: loaded %u origin(s) from '%s'", g_hash_table_size(self->origins), self->path);
  wig_permissions_manager_expire(self);
  return TRUE;
}

void wig_permissions_manager_save(WigPermissionsManager *self)
{
  g_clear_handle_id(&self->save_timeout_id, g_source_remove);
  if (!self->dirty || self->unsupported_format)
    return;

  g_autofree char *dir = g_path_get_dirname(self->path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_warning("permissions: cannot create '%s': %s", dir, g_strerror(errno));
    return;
  }

  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_key_file_set_integer(key_file, WIG_PERMISSIONS_GROUP, "version", WIG_PERMISSIONS_FORMAT_VERSION);

  g_autoptr(GList) origins = g_hash_table_get_keys(self->origins);
  origins = g_list_sort(g_steal_pointer(&origins), (GCompareFunc)g_strcmp0);
  guint saved = 0;
  for (GList *l = origins; l; l = l->next) {
    WigPermissionOrigin *record = g_hash_table_lookup(self->origins, l->data);
    if (!permission_origin_has_data(record))
      continue;

    g_autofree char *group = permission_origin_group_name(record->origin);
    if (record->last_visited)
      g_key_file_set_int64(key_file, group, "last-visited", record->last_visited);

    for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
      const char *name = wig_permission_kind_get_property_name(kind);
      WebKitPermissionState state = wig_permissions_get_state(record->permissions, kind);
      if (state != WEBKIT_PERMISSION_STATE_PROMPT && !wig_permissions_is_session_only(record->permissions, kind))
        g_key_file_set_string(key_file, group, name, permission_state_to_string(state));
    }

    if (record->has_autoplay)
      g_key_file_set_string(key_file, group, "autoplay", autoplay_to_string(record->autoplay));
#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
    if (record->has_https_navigation)
      g_key_file_set_string(key_file, group, "https-navigation", https_navigation_to_string(record->https_navigation));
#endif
    if (record->user_agent)
      g_key_file_set_string(key_file, group, "user-agent", record->user_agent);

    saved++;
  }

  g_autoptr(GError) error = NULL;
  if (!wig_key_file_save(key_file, self->path, &error)) {
    g_warning("permissions: cannot write '%s': %s", self->path, error->message);
    return;
  }

  self->dirty = FALSE;
  g_debug("permissions: wrote %u origin(s) to '%s'", saved, self->path);
}

static gboolean wig_permissions_manager_save_timeout(gpointer user_data)
{
  WigPermissionsManager *self = WIG_PERMISSIONS_MANAGER(user_data);
  self->save_timeout_id = 0;
  wig_permissions_manager_save(self);
  return G_SOURCE_REMOVE;
}

static void wig_permissions_manager_queue_save(WigPermissionsManager *self)
{
  if (!self->unsupported_format && !self->save_timeout_id) {
    self->save_timeout_id = g_timeout_add_seconds(WIG_PERMISSIONS_SAVE_DELAY_SECONDS,
                                                  wig_permissions_manager_save_timeout, self);
  }
}

static void wig_permissions_manager_dispose(GObject *object)
{
  WigPermissionsManager *self = WIG_PERMISSIONS_MANAGER(object);

  g_clear_handle_id(&self->save_timeout_id, g_source_remove);
  g_clear_handle_id(&self->expire_timeout_id, g_source_remove);
  g_clear_pointer(&self->origins, g_hash_table_unref);
  g_clear_pointer(&self->path, g_free);

  G_OBJECT_CLASS(wig_permissions_manager_parent_class)->dispose(object);
}

static void wig_permissions_manager_class_init(WigPermissionsManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_permissions_manager_dispose;

  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void wig_permissions_manager_init(WigPermissionsManager *self)
{
  self->origins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)permission_origin_free);
  /* Sessions can outlast the expiry threshold, so checking only at startup
   * would let grants sit long past it. */
  self->expire_timeout_id = g_timeout_add_seconds(WIG_PERMISSIONS_EXPIRY_INTERVAL_SECONDS,
                                                  wig_permissions_manager_expire_timeout, self);
}

WigPermissionsManager *wig_permissions_manager_new(const char *state_dir)
{
  g_assert(state_dir != NULL);

  WigPermissionsManager *self = g_object_new(WIG_TYPE_PERMISSIONS_MANAGER, NULL);
  self->path = g_build_filename(state_dir, "permissions.ini", NULL);
  return self;
}

WigPermissions *wig_permissions_manager_lookup(WigPermissionsManager *self, const char *origin)
{
  g_assert(origin != NULL);

  WigPermissionOrigin *record = g_hash_table_lookup(self->origins, origin);
  return record && permission_origin_has_decisions(record) ? record->permissions : NULL;
}

static WigPermissionOrigin *permission_origin_ensure(WigPermissionsManager *self, const char *origin)
{
  WigPermissionOrigin *record = g_hash_table_lookup(self->origins, origin);
  if (record)
    return record;

  record = permission_origin_new(self, origin);
  record->last_visited = current_visit_timestamp();
  permission_origin_connect(record);
  g_hash_table_insert(self->origins, g_strdup(record->origin), record);

  self->dirty = TRUE;
  wig_permissions_manager_queue_save(self);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0, origin);

  return record;
}

WigPermissions *wig_permissions_manager_ensure(WigPermissionsManager *self, const char *origin)
{
  g_assert(origin != NULL);

  return permission_origin_ensure(self, origin)->permissions;
}

void wig_permissions_manager_visit(WigPermissionsManager *self, const char *origin)
{
  g_assert(origin != NULL);

  WigPermissionOrigin *record = g_hash_table_lookup(self->origins, origin);
  if (!record)
    return;

  gint64 timestamp = current_visit_timestamp();
  if (record->last_visited >= timestamp)
    return;

  g_debug("permissions: %s visited, last-visited %" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT, origin,
          record->last_visited, timestamp);
  record->last_visited = timestamp;
  self->dirty = TRUE;
  wig_permissions_manager_queue_save(self);
}

/* A policy is the browser's own doing, so unlike a permission it is recorded for
 * an origin the user has never been to and kept until they say otherwise. */
static void wig_permissions_manager_policy_changed(WigPermissionsManager *self, const char *origin)
{
  self->dirty = TRUE;
  wig_permissions_manager_queue_save(self);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0, origin);
}

gboolean wig_permissions_manager_get_autoplay(WigPermissionsManager *self, const char *origin,
                                              WebKitAutoplayPolicy *autoplay)
{
  WigPermissionOrigin *record = origin ? g_hash_table_lookup(self->origins, origin) : NULL;
  if (!record || !record->has_autoplay)
    return FALSE;

  /* Asking only whether the site has a rule of its own is a fair question. */
  if (autoplay)
    *autoplay = record->autoplay;
  return TRUE;
}

void wig_permissions_manager_set_autoplay(WigPermissionsManager *self, const char *origin,
                                          WebKitAutoplayPolicy autoplay)
{
  g_assert(origin != NULL);

  WigPermissionOrigin *record = permission_origin_ensure(self, origin);
  if (record->has_autoplay && record->autoplay == autoplay)
    return;

  g_debug("permissions: autoplay for %s is now %s", origin, autoplay_to_string(autoplay));
  record->has_autoplay = TRUE;
  record->autoplay = autoplay;
  wig_permissions_manager_policy_changed(self, origin);
}

void wig_permissions_manager_clear_autoplay(WigPermissionsManager *self, const char *origin)
{
  g_assert(origin != NULL);

  WigPermissionOrigin *record = g_hash_table_lookup(self->origins, origin);
  if (!record || !record->has_autoplay)
    return;

  g_debug("permissions: autoplay for %s is left to whatever every other site gets", origin);
  record->has_autoplay = FALSE;
  wig_permissions_manager_policy_changed(self, origin);
}

#if HAVE_HTTPS_NAVIGATION_POLICY_SUPPORT
gboolean wig_permissions_manager_get_https_navigation(WigPermissionsManager *self, const char *origin,
                                                      WebKitHTTPSNavigationPolicy *https_navigation)
{
  WigPermissionOrigin *record = origin ? g_hash_table_lookup(self->origins, origin) : NULL;
  if (!record || !record->has_https_navigation)
    return FALSE;

  if (https_navigation)
    *https_navigation = record->https_navigation;
  return TRUE;
}

void wig_permissions_manager_set_https_navigation(WigPermissionsManager *self, const char *origin,
                                                  WebKitHTTPSNavigationPolicy https_navigation)
{
  g_assert(origin != NULL);

  WigPermissionOrigin *record = permission_origin_ensure(self, origin);
  if (record->has_https_navigation && record->https_navigation == https_navigation)
    return;

  g_debug("permissions: https navigation for %s is now %s", origin, https_navigation_to_string(https_navigation));
  record->has_https_navigation = TRUE;
  record->https_navigation = https_navigation;
  wig_permissions_manager_policy_changed(self, origin);
}

void wig_permissions_manager_clear_https_navigation(WigPermissionsManager *self, const char *origin)
{
  g_assert(origin != NULL);

  WigPermissionOrigin *record = g_hash_table_lookup(self->origins, origin);
  if (!record || !record->has_https_navigation)
    return;

  g_debug("permissions: https navigation for %s is left to the setting", origin);
  record->has_https_navigation = FALSE;
  wig_permissions_manager_policy_changed(self, origin);
}

GList *wig_permissions_manager_list_https_navigation_sites(WigPermissionsManager *self,
                                                           WebKitHTTPSNavigationPolicy https_navigation)
{
  GList *sites = NULL;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->origins);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    WigPermissionOrigin *record = value;
    if (record->has_https_navigation && record->https_navigation == https_navigation)
      sites = g_list_prepend(sites, g_strdup(record->origin));
  }

  return g_list_sort(sites, (GCompareFunc)g_strcmp0);
}
#endif

const char *wig_permissions_manager_get_user_agent(WigPermissionsManager *self, const char *origin)
{
  WigPermissionOrigin *record = origin ? g_hash_table_lookup(self->origins, origin) : NULL;
  return record ? record->user_agent : NULL;
}

void wig_permissions_manager_set_user_agent(WigPermissionsManager *self, const char *origin, const char *user_agent)
{
  g_assert(origin != NULL);

  if (user_agent && !*user_agent)
    user_agent = NULL;

  WigPermissionOrigin *record = permission_origin_ensure(self, origin);
  if (!g_set_str(&record->user_agent, user_agent))
    return;

  g_debug("permissions: user agent for %s is now '%s'", origin, user_agent ? user_agent : "(the usual one)");
  wig_permissions_manager_policy_changed(self, origin);
}

GList *wig_permissions_manager_list_autoplay_sites(WigPermissionsManager *self, WebKitAutoplayPolicy autoplay)
{
  GList *sites = NULL;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->origins);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    WigPermissionOrigin *record = value;
    if (record->has_autoplay && record->autoplay == autoplay)
      sites = g_list_prepend(sites, g_strdup(record->origin));
  }

  return g_list_sort(sites, (GCompareFunc)g_strcmp0);
}

GList *wig_permissions_manager_list_user_agent_sites(WigPermissionsManager *self)
{
  GList *sites = NULL;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->origins);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    WigPermissionOrigin *record = value;
    if (record->user_agent)
      sites = g_list_prepend(sites, g_strdup(record->origin));
  }

  return g_list_sort(sites, (GCompareFunc)g_strcmp0);
}

GList *wig_permissions_manager_list_sites(WigPermissionsManager *self, WigPermissionKind kind,
                                          WebKitPermissionState state)
{
  GList *sites = NULL;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->origins);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    WigPermissionOrigin *record = value;
    if (wig_permissions_get_state(record->permissions, kind) == state)
      sites = g_list_prepend(sites, g_strdup(record->origin));
  }

  return g_list_sort(sites, (GCompareFunc)g_strcmp0);
}

/* An origin that cannot be read back as one is dropped: the caller wants origins
 * to hand to WebKit, and one it would refuse is no use to it. Anything managing
 * the answers on file asks for the sites instead, which are kept as they are. */
GList *wig_permissions_manager_list_origins(WigPermissionsManager *self, WigPermissionKind kind,
                                            WebKitPermissionState state)
{
  GList *sites = wig_permissions_manager_list_sites(self, kind, state);
  GList *origins = NULL;

  for (GList *l = sites; l; l = l->next) {
    WebKitSecurityOrigin *origin = webkit_security_origin_new_for_uri(l->data);
    if (origin)
      origins = g_list_prepend(origins, origin);
  }

  g_list_free_full(sites, g_free);
  return origins;
}

void wig_permissions_manager_handle_request(WigPermissionsManager *self, const char *origin,
                                            WebKitPermissionRequest *request, WigPermissionRequestPopover *popover)
{
  g_assert(origin != NULL);

  WigPermissionKind undecided = wig_permission_kinds_for_request(request);
  g_assert(undecided != 0);

  g_debug("permission request %s for %s: kinds 0x%x", G_OBJECT_TYPE_NAME(request), origin, undecided);

  WigPermissions *permissions = wig_permissions_manager_ensure(self, origin);

  /* A request covering several devices is answered as a whole, so any one of
   * them being denied denies all of it, and it can only be allowed outright
   * once every one of them is granted. */
  if (permissions) {
    for (WigPermissionKind kind = 1; kind <= WIG_PERMISSION_ALL_KINDS; kind <<= 1) {
      if (!(undecided & kind))
        continue;

      switch (wig_permissions_get_state(permissions, kind)) {
      case WEBKIT_PERMISSION_STATE_DENIED:
        webkit_permission_request_deny(request);
        return;
      case WEBKIT_PERMISSION_STATE_GRANTED:
        undecided &= ~kind;
        break;
      case WEBKIT_PERMISSION_STATE_PROMPT:
      default:
        break;
      }
    }

    if (undecided == 0) {
      webkit_permission_request_allow(request);
      return;
    }
  }

  /* Remember the origin (making the button visible) and prompt the user for
   * whichever permissions are still undecided. */
  wig_permission_request_popover_prompt(popover, permissions, origin, undecided, request);
}
