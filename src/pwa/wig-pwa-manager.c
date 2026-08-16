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

#include "wig-pwa-manager.h"

#include "wig-app-installer-native.h"
#include "wig-app-installer-portal.h"
#include "wig-app-installer.h"
#include "wig-flatpak.h"
#include "wig-key-file-utils.h"
#include "wig-pwa-icon.h"
#include "wig-pwa-protocol-handlers.h"
#include "wig-pwa-util.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#define DESKTOP_ICON_SIZE 512
#define MAX_ICON_BYTES (4 * 1024 * 1024)
#define MANIFEST_REFRESH_INTERVAL (12 * 60 * 60)
#define WEB_APPLICATIONS_FILE "web_applications.ini"

#if HAVE_APPLICATION_MANIFEST_SUPPORT
typedef struct {
  char *id;
  char *manifest_url;
  char *start_url;
  char *scope;
  char *temporary_directory;
  char *association_path;
  GLoadableIcon *icon;
  GInputStream *icon_stream;
  GByteArray *icon_data;
  GBytes *icon_bytes;
  char *icon_content_type;
  char *background_color;
  char *old_desktop_file;
  char *old_icon_file;
  GError *download_error;
  GVariant *manifest;
  GVariant *shortcuts;
  WigAppInstallerArtifacts *installer_artifacts;
  GPtrArray *extension_origins;
  GPtrArray *extension_scopes;
  guint extension_index;
  gboolean metadata_changed;
  gboolean needs_desktop_update;
  gboolean needs_icon_update;
  gboolean needs_scope_update;
  gboolean throttled;
} InstallData;
#endif

struct _WigPwaManager {
  GObject parent;

  char *path;
  GVariantDict entries;
  gboolean entries_initialized;
  WebKitNetworkSession *network_session;
  WigAppInstaller *app_installer;
  GHashTable *refresh_attempts; /* char* id -> gint64* seconds */
};

G_DEFINE_FINAL_TYPE(WigPwaManager, wig_pwa_manager, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* Keys holding plain strings, mapped from their name in the entry dictionary to
 * their name in the key file. */
static const struct {
  const char *entry_key;
  const char *file_key;
} string_keys[] = {
  { "manifest_url", "manifest-url" },         { "start_url", "start-url" },       { "scope", "scope" },
  { "background_color", "background-color" }, { "desktop_file", "desktop-file" }, { "icon_file", "icon-file" },
};

static char *entry_group_name(const char *id)
{
  return g_uri_escape_string(id, ":/.-_~", FALSE);
}

/* Manifests and shortcuts are arbitrarily nested, so they are kept as GVariant
 * text rather than flattened into keys of their own. */
static void set_variant_key(GKeyFile *key_file, const char *group, const char *key, GVariant *value)
{
  g_autofree char *text = g_variant_print(value, TRUE);
  g_key_file_set_string(key_file, group, key, text);
}

static GVariant *get_variant_key(GKeyFile *key_file, const char *group, const char *key, const GVariantType *type,
                                 GError **error)
{
  g_autofree char *text = g_key_file_get_string(key_file, group, key, error);
  if (!text)
    return NULL;

  g_autoptr(GVariant) value = g_variant_parse(type, text, NULL, NULL, error);
  if (!value) {
    g_prefix_error(error, "Invalid '%s': ", key);
    return NULL;
  }
  return g_steal_pointer(&value);
}

static const char *entry_launch_client_mode(GVariant *entry)
{
  g_autoptr(GVariant) overrides = g_variant_lookup_value(entry, "overrides", G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) launch_handler = overrides
      ? g_variant_lookup_value(overrides, "launch_handler", G_VARIANT_TYPE_VARDICT)
      : NULL;
  return launch_handler ? wig_pwa_variant_lookup_string(launch_handler, "client_mode") : NULL;
}

static void entry_to_key_file(GKeyFile *key_file, const char *id, GVariant *entry)
{
  g_autofree char *group = entry_group_name(id);

  for (gsize i = 0; i < G_N_ELEMENTS(string_keys); i++) {
    const char *value = wig_pwa_variant_lookup_string(entry, string_keys[i].entry_key);
    if (value)
      g_key_file_set_string(key_file, group, string_keys[i].file_key, value);
  }

  g_autoptr(GVariant) manifest = g_variant_lookup_value(entry, "manifest", G_VARIANT_TYPE_VARDICT);
  if (manifest)
    set_variant_key(key_file, group, "manifest", manifest);

  g_autoptr(GVariant) shortcuts = g_variant_lookup_value(entry, "shortcuts", G_VARIANT_TYPE("aa{sv}"));
  if (shortcuts)
    set_variant_key(key_file, group, "shortcuts", shortcuts);

  g_autoptr(GVariant) extensions = g_variant_lookup_value(entry, "scope_extensions", G_VARIANT_TYPE_STRING_ARRAY);
  gsize n_extensions = 0;
  g_autofree const char **extension_scopes = extensions ? g_variant_get_strv(extensions, &n_extensions) : NULL;
  if (n_extensions > 0)
    g_key_file_set_string_list(key_file, group, "scope-extensions", extension_scopes, n_extensions);

  gint64 last_updated = 0;
  if (g_variant_lookup(entry, "last_updated", "x", &last_updated))
    g_key_file_set_int64(key_file, group, "last-updated", last_updated);

  const char *client_mode = entry_launch_client_mode(entry);
  if (client_mode)
    g_key_file_set_string(key_file, group, "launch-client-mode", client_mode);

  gboolean open_in_browser = FALSE;
  if (g_variant_lookup(entry, "open_primary_scope_in_browser", "b", &open_in_browser))
    g_key_file_set_boolean(key_file, group, "open-primary-scope-in-browser", open_in_browser);
}

static GVariant *entry_from_key_file(GKeyFile *key_file, const char *group, GError **error)
{
  g_autoptr(GVariant) manifest = get_variant_key(key_file, group, "manifest", G_VARIANT_TYPE_VARDICT, error);
  if (!manifest)
    return NULL;

  g_autoptr(GVariant) shortcuts = NULL;
  if (g_key_file_has_key(key_file, group, "shortcuts", NULL)) {
    shortcuts = get_variant_key(key_file, group, "shortcuts", G_VARIANT_TYPE("aa{sv}"), error);
    if (!shortcuts)
      return NULL;
  }

  GVariantDict entry;
  g_variant_dict_init(&entry, NULL);
  g_variant_dict_insert_value(&entry, "manifest", manifest);
  if (shortcuts)
    g_variant_dict_insert_value(&entry, "shortcuts", shortcuts);

  for (gsize i = 0; i < G_N_ELEMENTS(string_keys); i++) {
    g_autofree char *value = g_key_file_get_string(key_file, group, string_keys[i].file_key, NULL);
    if (value)
      g_variant_dict_insert(&entry, string_keys[i].entry_key, "s", value);
  }

  gsize n_extensions = 0;
  g_auto(GStrv) extension_scopes = g_key_file_get_string_list(key_file, group, "scope-extensions", &n_extensions, NULL);
  if (extension_scopes) {
    g_variant_dict_insert_value(&entry, "scope_extensions",
                                g_variant_new_strv((const char *const *)extension_scopes, (gssize)n_extensions));
  }

  if (g_key_file_has_key(key_file, group, "last-updated", NULL))
    g_variant_dict_insert(&entry, "last_updated", "x", wig_key_file_get_int64(key_file, group, "last-updated", 0));

  g_autofree char *client_mode = g_key_file_get_string(key_file, group, "launch-client-mode", NULL);
  if (client_mode) {
    GVariantDict launch_handler;
    g_variant_dict_init(&launch_handler, NULL);
    g_variant_dict_insert(&launch_handler, "client_mode", "s", client_mode);
    GVariantDict overrides;
    g_variant_dict_init(&overrides, NULL);
    g_variant_dict_insert_value(&overrides, "launch_handler", g_variant_dict_end(&launch_handler));
    g_variant_dict_insert_value(&entry, "overrides", g_variant_dict_end(&overrides));
  }

  if (g_key_file_has_key(key_file, group, "open-primary-scope-in-browser", NULL)) {
    g_variant_dict_insert(&entry, "open_primary_scope_in_browser", "b",
                          wig_key_file_get_boolean(key_file, group, "open-primary-scope-in-browser", FALSE));
  }

  return g_variant_ref_sink(g_variant_dict_end(&entry));
}

static gboolean ensure_entries_loaded(WigPwaManager *self, GError **error)
{
  if (self->entries_initialized)
    return TRUE;

  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GError) local_error = NULL;
  if (!g_key_file_load_from_file(key_file, self->path, G_KEY_FILE_NONE, &local_error)
      && !g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    g_propagate_prefixed_error(error, g_steal_pointer(&local_error), "Failed to read '%s': ", self->path);
    return FALSE;
  }

  g_variant_dict_init(&self->entries, NULL);
  self->entries_initialized = TRUE;

  gsize n_groups = 0;
  g_auto(GStrv) groups = g_key_file_get_groups(key_file, &n_groups);
  for (gsize i = 0; i < n_groups; i++) {
    g_autofree char *id = g_uri_unescape_string(groups[i], NULL);
    if (!id || !*id) {
      g_warning("pwa: ignoring invalid web app group '%s'", groups[i]);
      continue;
    }

    g_autoptr(GError) entry_error = NULL;
    g_autoptr(GVariant) entry = entry_from_key_file(key_file, groups[i], &entry_error);
    if (!entry) {
      g_warning("pwa: ignoring invalid web app '%s': %s", id, entry_error->message);
      continue;
    }
    g_variant_dict_insert_value(&self->entries, id, entry);
  }
  return TRUE;
}

static GVariant *lookup_entry(WigPwaManager *self, const char *id, GError **error)
{
  if (!ensure_entries_loaded(self, error))
    return NULL;

  GVariant *entry = g_variant_dict_lookup_value(&self->entries, id, G_VARIANT_TYPE_VARDICT);
  if (!entry) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Web app '%s' is not installed", id);
    return NULL;
  }
  return entry;
}

static GVariant *snapshot_entries(WigPwaManager *self)
{
  GVariant *entries = g_variant_ref_sink(g_variant_dict_end(&self->entries));
  g_variant_dict_init(&self->entries, entries);
  return entries;
}

static void replace_entries(WigPwaManager *self, GVariant *entries)
{
  g_variant_dict_clear(&self->entries);
  g_variant_dict_init(&self->entries, entries);
}

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static void install_data_free(InstallData *data)
{
  if (data->association_path)
    g_unlink(data->association_path);
  if (data->temporary_directory)
    g_rmdir(data->temporary_directory);
  g_clear_pointer(&data->id, g_free);
  g_clear_pointer(&data->manifest_url, g_free);
  g_clear_pointer(&data->start_url, g_free);
  g_clear_pointer(&data->scope, g_free);
  g_clear_pointer(&data->temporary_directory, g_free);
  g_clear_pointer(&data->association_path, g_free);
  g_clear_object(&data->icon);
  g_clear_object(&data->icon_stream);
  g_clear_pointer(&data->icon_data, g_byte_array_unref);
  g_clear_pointer(&data->icon_bytes, g_bytes_unref);
  g_clear_pointer(&data->icon_content_type, g_free);
  g_clear_pointer(&data->background_color, g_free);
  g_clear_pointer(&data->old_desktop_file, g_free);
  g_clear_pointer(&data->old_icon_file, g_free);
  g_clear_error(&data->download_error);
  g_clear_pointer(&data->manifest, g_variant_unref);
  g_clear_pointer(&data->shortcuts, g_variant_unref);
  g_clear_pointer(&data->installer_artifacts, wig_app_installer_artifacts_free);
  g_clear_pointer(&data->extension_origins, g_ptr_array_unref);
  g_clear_pointer(&data->extension_scopes, g_ptr_array_unref);
  g_free(data);
}

static GVariant *build_manifest_entry(const char *manifest_url, const char *start_url, const char *scope,
                                      GVariant *manifest, const char *background_color, GVariant *shortcuts,
                                      GPtrArray *extension_scopes, const char *desktop_file, const char *icon_file)
{
  GVariantBuilder entry_builder;
  g_variant_builder_init(&entry_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&entry_builder, "{sv}", "manifest", manifest);
  g_variant_builder_add(&entry_builder, "{sv}", "manifest_url", g_variant_new_string(manifest_url));
  g_variant_builder_add(&entry_builder, "{sv}", "start_url", g_variant_new_string(start_url));
  g_variant_builder_add(&entry_builder, "{sv}", "scope", g_variant_new_string(scope));
  if (background_color)
    g_variant_builder_add(&entry_builder, "{sv}", "background_color", g_variant_new_string(background_color));
  g_variant_builder_add(&entry_builder, "{sv}", "shortcuts", shortcuts);
  GVariantBuilder extensions_builder;
  g_variant_builder_init(&extensions_builder, G_VARIANT_TYPE_STRING_ARRAY);
  for (guint i = 0; extension_scopes && i < extension_scopes->len; i++)
    g_variant_builder_add(&extensions_builder, "s", (const char *)g_ptr_array_index(extension_scopes, i));
  g_variant_builder_add(&entry_builder, "{sv}", "scope_extensions", g_variant_builder_end(&extensions_builder));
  g_variant_builder_add(&entry_builder, "{sv}", "last_updated",
                        g_variant_new_int64(g_get_real_time() / G_USEC_PER_SEC));
  g_variant_builder_add(&entry_builder, "{sv}", "desktop_file", g_variant_new_string(desktop_file));
  if (icon_file)
    g_variant_builder_add(&entry_builder, "{sv}", "icon_file", g_variant_new_string(icon_file));

  return g_variant_ref_sink(g_variant_builder_end(&entry_builder));
}
#endif

static char *normalize_scope_url(const char *url, GError **error)
{
  g_autoptr(GUri) uri = g_uri_parse(
      url, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_SCHEME_NORMALIZE | G_URI_FLAGS_ENCODED_PATH, error);
  if (!uri)
    return NULL;

  const char *query = strchr(url, '?');
  const char *fragment = strchr(url, '#');
  const char *end = query && fragment ? MIN(query, fragment) : query ? query : fragment;
  return end ? g_strndup(url, (gsize)(end - url)) : g_strdup(url);
}

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static char *normalize_https_origin(const char *url)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_SCHEME_NORMALIZE, &error);
  if (!uri || !g_uri_get_scheme(uri) || g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") != 0 || !g_uri_get_host(uri)
      || g_uri_get_userinfo(uri))
    return NULL;

  g_autoptr(WebKitSecurityOrigin) origin = webkit_security_origin_new_for_uri(url);
  return webkit_security_origin_to_string(origin);
}

static gboolean uris_have_same_origin(const char *first, const char *second)
{
  g_autoptr(WebKitSecurityOrigin) first_origin = webkit_security_origin_new_for_uri(first);
  g_autoptr(WebKitSecurityOrigin) second_origin = webkit_security_origin_new_for_uri(second);
  g_autofree char *first_string = webkit_security_origin_to_string(first_origin);
  g_autofree char *second_string = webkit_security_origin_to_string(second_origin);
  return g_strcmp0(first_string, second_string) == 0;
}

static GPtrArray *manifest_scope_extension_origins(GVariant *manifest)
{
  g_autoptr(GPtrArray) origins = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GVariant) extensions = g_variant_lookup_value(manifest, "scope_extensions", G_VARIANT_TYPE("av"));
  gsize length = extensions ? g_variant_n_children(extensions) : 0;
  for (gsize i = 0; i < length; i++) {
    g_autoptr(GVariant) extension = wig_pwa_variant_array_get_dict(extensions, i);
    if (!extension || g_strcmp0(wig_pwa_variant_lookup_string(extension, "type"), "origin") != 0)
      continue;

    const char *origin_value = wig_pwa_variant_lookup_string(extension, "origin");
    g_autofree char *origin = origin_value ? normalize_https_origin(origin_value) : NULL;
    if (!origin || g_ptr_array_find_with_equal_func(origins, origin, (GEqualFunc)g_str_equal, NULL))
      continue;
    g_ptr_array_add(origins, g_steal_pointer(&origin));
  }
  return g_steal_pointer(&origins);
}
#endif

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static char *manifest_color_to_css(const WebKitColor *color)
{
  if (!color)
    return NULL;

  char red[G_ASCII_DTOSTR_BUF_SIZE];
  char green[G_ASCII_DTOSTR_BUF_SIZE];
  char blue[G_ASCII_DTOSTR_BUF_SIZE];
  char alpha[G_ASCII_DTOSTR_BUF_SIZE];
  return g_strdup_printf("rgba(%s, %s, %s, %s)", g_ascii_formatd(red, sizeof(red), "%g", color->red * 255),
                         g_ascii_formatd(green, sizeof(green), "%g", color->green * 255),
                         g_ascii_formatd(blue, sizeof(blue), "%g", color->blue * 255),
                         g_ascii_formatd(alpha, sizeof(alpha), "%g", color->alpha));
}

static GVariant *manifest_shortcuts_from_webkit(WebKitApplicationManifest *manifest)
{
  GVariantBuilder shortcuts_builder;
  g_variant_builder_init(&shortcuts_builder, G_VARIANT_TYPE("aa{sv}"));
  WebKitApplicationManifestShortcut *const *shortcuts = webkit_application_manifest_get_shortcuts(manifest);
  for (guint i = 0; shortcuts && shortcuts[i]; i++) {
    const char *name = webkit_application_manifest_shortcut_get_name(shortcuts[i]);
    const char *uri = webkit_application_manifest_shortcut_get_uri(shortcuts[i]);
    if (!name || !*name || !uri || !*uri)
      continue;

    WebKitApplicationManifestIcon *const *icons = webkit_application_manifest_shortcut_get_icons(shortcuts[i]);
    const char *icon_uri = icons && icons[0] ? webkit_application_manifest_icon_get_uri(icons[0]) : NULL;
    GVariantBuilder shortcut_builder;
    g_variant_builder_init(&shortcut_builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&shortcut_builder, "{sv}", "name", g_variant_new_string(name));
    g_variant_builder_add(&shortcut_builder, "{sv}", "url", g_variant_new_string(uri));
    g_variant_builder_add(&shortcut_builder, "{sv}", "icon_url", g_variant_new_string(icon_uri ? icon_uri : ""));
    g_variant_builder_add_value(&shortcuts_builder, g_variant_builder_end(&shortcut_builder));
  }
  return g_variant_ref_sink(g_variant_builder_end(&shortcuts_builder));
}

static GVariant *manifest_with_webkit_strings(WebKitApplicationManifest *manifest, GVariant *full_manifest)
{
  GVariantDict dictionary;
  g_variant_dict_init(&dictionary, full_manifest);

  const char *name = webkit_application_manifest_get_name(manifest);
  const char *short_name = webkit_application_manifest_get_short_name(manifest);
  const char *description = webkit_application_manifest_get_description(manifest);
  if (name)
    g_variant_dict_insert(&dictionary, "name", "s", name);
  else
    g_variant_dict_remove(&dictionary, "name");
  if (short_name)
    g_variant_dict_insert(&dictionary, "short_name", "s", short_name);
  else
    g_variant_dict_remove(&dictionary, "short_name");
  if (description)
    g_variant_dict_insert(&dictionary, "description", "s", description);
  else
    g_variant_dict_remove(&dictionary, "description");

  return g_variant_ref_sink(g_variant_dict_end(&dictionary));
}
#endif

static int compare_entry_ids(gconstpointer first, gconstpointer second)
{
  g_autoptr(GVariant) first_id = g_variant_get_child_value(*(GVariant *const *)first, 0);
  g_autoptr(GVariant) second_id = g_variant_get_child_value(*(GVariant *const *)second, 0);
  return g_strcmp0(g_variant_get_string(first_id, NULL), g_variant_get_string(second_id, NULL));
}

static gboolean write_entries(WigPwaManager *self, GVariant *entries, GError **error)
{
  g_autofree char *directory = g_path_get_dirname(self->path);
  if (g_mkdir_with_parents(directory, 0700) == -1) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to create '%s': %s", directory,
                g_strerror(errno));
    return FALSE;
  }

  /* Dictionaries iterate in an arbitrary order; sorting keeps the file stable
   * across rewrites. */
  g_autoptr(GPtrArray) sorted = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  GVariantIter iterator;
  g_variant_iter_init(&iterator, entries);
  while (TRUE) {
    GVariant *dictionary_entry = g_variant_iter_next_value(&iterator);
    if (!dictionary_entry)
      break;
    g_ptr_array_add(sorted, dictionary_entry);
  }
  g_ptr_array_sort(sorted, compare_entry_ids);

  g_autoptr(GKeyFile) key_file = g_key_file_new();
  for (guint i = 0; i < sorted->len; i++) {
    const char *id;
    g_autoptr(GVariant) value = NULL;
    g_variant_get(g_ptr_array_index(sorted, i), "{&sv}", &id, &value);
    entry_to_key_file(key_file, id, value);
  }

  return wig_key_file_save(key_file, self->path, error);
}

static gboolean update_entry(WigPwaManager *self, const char *id, GVariant *entry, GError **error)
{
  g_autoptr(GVariant) current_entries = snapshot_entries(self);
  GVariantDict updated_entries;
  g_variant_dict_init(&updated_entries, current_entries);
  g_variant_dict_insert_value(&updated_entries, id, entry);
  g_autoptr(GVariant) entries = g_variant_ref_sink(g_variant_dict_end(&updated_entries));
  if (!write_entries(self, entries, error))
    return FALSE;

  replace_entries(self, entries);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0, id);
  return TRUE;
}

static gboolean remove_entry(WigPwaManager *self, const char *id, GError **error)
{
  g_autoptr(GVariant) current_entries = snapshot_entries(self);
  GVariantDict updated_entries;
  g_variant_dict_init(&updated_entries, current_entries);
  g_variant_dict_remove(&updated_entries, id);
  g_autoptr(GVariant) entries = g_variant_ref_sink(g_variant_dict_end(&updated_entries));
  if (!write_entries(self, entries, error))
    return FALSE;

  replace_entries(self, entries);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0, id);
  return TRUE;
}

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static gboolean store_manifest(WigPwaManager *self, const char *id, const char *manifest_url, const char *start_url,
                               const char *scope, GVariant *manifest, const char *background_color, GVariant *shortcuts,
                               GPtrArray *extension_scopes, const char *desktop_file, const char *icon_file,
                               GError **error)
{
  if (!ensure_entries_loaded(self, error))
    return FALSE;

  g_autoptr(GVariant) previous_entry = g_variant_dict_lookup_value(&self->entries, id, G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) entry = build_manifest_entry(manifest_url, start_url, scope, manifest, background_color,
                                                   shortcuts, extension_scopes, desktop_file, icon_file);
  if (previous_entry) {
    GVariantDict updated_entry;
    g_variant_dict_init(&updated_entry, entry);
    g_autoptr(GVariant) overrides = g_variant_lookup_value(previous_entry, "overrides", G_VARIANT_TYPE_VARDICT);
    if (overrides)
      g_variant_dict_insert_value(&updated_entry, "overrides", overrides);
    g_autoptr(GVariant) open_in_browser = g_variant_lookup_value(previous_entry, "open_primary_scope_in_browser",
                                                                 G_VARIANT_TYPE_BOOLEAN);
    if (open_in_browser)
      g_variant_dict_insert_value(&updated_entry, "open_primary_scope_in_browser", open_in_browser);
    g_clear_pointer(&entry, g_variant_unref);
    entry = g_variant_ref_sink(g_variant_dict_end(&updated_entry));
  }

  g_autoptr(GVariant) current_entries = snapshot_entries(self);
  GVariantDict updated_entries;
  g_variant_dict_init(&updated_entries, current_entries);
  g_variant_dict_insert_value(&updated_entries, id, entry);
  g_autoptr(GVariant) entries = g_variant_ref_sink(g_variant_dict_end(&updated_entries));
  if (!write_entries(self, entries, error))
    return FALSE;

  replace_entries(self, entries);
  return TRUE;
}

static gboolean variant_values_equal(GVariant *first, GVariant *second)
{
  return first && second ? g_variant_equal(first, second) : first == second;
}

static gboolean entry_variant_equal(GVariant *entry, const char *key, GVariant *value)
{
  g_autoptr(GVariant) existing = g_variant_lookup_value(entry, key, NULL);
  return variant_values_equal(existing, value);
}

static gboolean manifest_member_equal(GVariant *first, GVariant *second, const char *key)
{
  if (!first || !second)
    return first == second;

  g_autoptr(GVariant) first_value = g_variant_lookup_value(first, key, NULL);
  g_autoptr(GVariant) second_value = g_variant_lookup_value(second, key, NULL);
  return variant_values_equal(first_value, second_value);
}

static gboolean install_data_prepare(WigPwaManager *self, InstallData *data, GError **error)
{
  if (!ensure_entries_loaded(self, error))
    return FALSE;

  g_autoptr(GVariant) previous = g_variant_dict_lookup_value(&self->entries, data->id, G_VARIANT_TYPE_VARDICT);
  if (!previous) {
    data->metadata_changed = TRUE;
    data->needs_desktop_update = TRUE;
    data->needs_icon_update = TRUE;
    data->needs_scope_update = TRUE;
  } else {
    g_autoptr(GVariant) previous_manifest = g_variant_lookup_value(previous, "manifest", G_VARIANT_TYPE_VARDICT);
    data->metadata_changed = !entry_variant_equal(previous, "manifest", data->manifest)
        || g_strcmp0(wig_pwa_variant_lookup_string(previous, "manifest_url"), data->manifest_url) != 0
        || g_strcmp0(wig_pwa_variant_lookup_string(previous, "start_url"), data->start_url) != 0
        || g_strcmp0(wig_pwa_variant_lookup_string(previous, "scope"), data->scope) != 0
        || g_strcmp0(wig_pwa_variant_lookup_string(previous, "background_color"), data->background_color) != 0
        || !entry_variant_equal(previous, "shortcuts", data->shortcuts);
    data->needs_icon_update = !manifest_member_equal(previous_manifest, data->manifest, "icons");
    data->needs_desktop_update = !manifest_member_equal(previous_manifest, data->manifest, "name")
        || !manifest_member_equal(previous_manifest, data->manifest, "short_name")
        || !manifest_member_equal(previous_manifest, data->manifest, "description")
        || !manifest_member_equal(previous_manifest, data->manifest, "protocol_handlers");
    data->needs_scope_update = !manifest_member_equal(previous_manifest, data->manifest, "scope_extensions");
  }

  if (!data->metadata_changed)
    return TRUE;

  data->extension_scopes = g_ptr_array_new_with_free_func(g_free);
  if (data->needs_scope_update) {
    data->extension_origins = manifest_scope_extension_origins(data->manifest);
    return TRUE;
  }

  g_autoptr(GVariant) previous_extensions = g_variant_lookup_value(previous, "scope_extensions",
                                                                   G_VARIANT_TYPE_STRING_ARRAY);
  g_auto(GStrv) extension_scopes = previous_extensions ? g_variant_dup_strv(previous_extensions, NULL) : NULL;
  for (guint i = 0; extension_scopes && extension_scopes[i]; i++)
    g_ptr_array_add(data->extension_scopes, g_strdup(extension_scopes[i]));
  data->extension_origins = g_ptr_array_new_with_free_func(g_free);
  return TRUE;
}

static gboolean update_last_updated(WigPwaManager *self, const char *id, GError **error)
{
  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return FALSE;

  GVariantDict updated_entry;
  g_variant_dict_init(&updated_entry, entry);
  g_variant_dict_insert(&updated_entry, "last_updated", "x", g_get_real_time() / G_USEC_PER_SEC);
  g_autoptr(GVariant) value = g_variant_ref_sink(g_variant_dict_end(&updated_entry));
  return update_entry(self, id, value, error);
}

static void association_download_failed(WebKitDownload *download, GError *error, GTask *task)
{
  InstallData *data = g_task_get_task_data(task);
  if (!data->download_error)
    data->download_error = g_error_copy(error);
}

static void task_closure_notify(gpointer data, GClosure *closure)
{
  g_object_unref(data);
}

static void store_finished_install(GTask *task)
{
  InstallData *data = g_task_get_task_data(task);
  WigPwaManager *self = WIG_PWA_MANAGER(g_task_get_source_object(task));
  g_autoptr(GError) error = NULL;
  if (!ensure_entries_loaded(self, &error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  const char *desktop_file = data->installer_artifacts ? data->installer_artifacts->desktop_file
                                                       : data->old_desktop_file;
  const char *icon_file = data->installer_artifacts ? data->installer_artifacts->icon_file : data->old_icon_file;

  if (!desktop_file) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app '%s' has invalid desktop integration",
                            data->id);
    return;
  }

  if (!store_manifest(self, data->id, data->manifest_url, data->start_url, data->scope, data->manifest,
                      data->background_color, data->shortcuts, data->extension_scopes, desktop_file, icon_file,
                      &error)) {
    g_autoptr(GError) rollback_error = NULL;
    if (data->installer_artifacts && !data->old_desktop_file
        && !wig_app_installer_uninstall(self->app_installer, data->id, data->installer_artifacts->desktop_file,
                                        data->installer_artifacts->icon_file, &rollback_error))
      g_warning("pwa: failed to roll back desktop integration: %s", rollback_error->message);
    g_prefix_error(&error, "Failed to store manifest '%s': ", data->manifest_url);
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  if (data->old_icon_file && data->installer_artifacts
      && g_strcmp0(data->old_icon_file, data->installer_artifacts->icon_file) != 0) {
    g_autoptr(GError) cleanup_error = NULL;
    if (!wig_app_installer_remove_icon(self->app_installer, data->id, data->old_icon_file, &cleanup_error))
      g_warning("pwa: failed to remove replaced desktop icon: %s", cleanup_error->message);
  }

  g_debug("pwa: refreshed installed web app %s", data->id);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0, data->id);
  g_task_return_boolean(task, TRUE);
}

static void app_installed(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  InstallData *data = g_task_get_task_data(task);
  WigPwaManager *self = WIG_PWA_MANAGER(g_task_get_source_object(task));
  g_autoptr(GError) error = NULL;
  data->installer_artifacts = wig_app_installer_install_finish(self->app_installer, result, &error);
  if (!data->installer_artifacts) {
    g_prefix_error_literal(&error, "Failed to install app launcher: ");
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }
  store_finished_install(task);
}

static void finish_install(GTask *task)
{
  InstallData *data = g_task_get_task_data(task);
  WigPwaManager *self = WIG_PWA_MANAGER(g_task_get_source_object(task));
  g_autoptr(GError) error = NULL;
  if (!ensure_entries_loaded(self, &error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }
  g_autoptr(GVariant) old_entry = g_variant_dict_lookup_value(&self->entries, data->id, G_VARIANT_TYPE_VARDICT);
  if (old_entry) {
    data->old_desktop_file = g_strdup(wig_pwa_variant_lookup_string(old_entry, "desktop_file"));
    data->old_icon_file = g_strdup(wig_pwa_variant_lookup_string(old_entry, "icon_file"));
  }

  if (!data->needs_desktop_update && !data->needs_icon_update) {
    store_finished_install(task);
    return;
  }

  g_autofree const char **extension_scopes = g_new0(const char *, data->extension_scopes->len + 1);
  for (guint i = 0; i < data->extension_scopes->len; i++)
    extension_scopes[i] = g_ptr_array_index(data->extension_scopes, i);
  g_autoptr(WigPwaNavigationPolicy) navigation_policy = wig_pwa_navigation_policy_new(data->scope, extension_scopes,
                                                                                      &error);
  if (!navigation_policy) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  g_auto(GStrv) protocols = wig_pwa_protocol_handlers_get_schemes(data->manifest, data->manifest_url,
                                                                  navigation_policy);
  g_autofree char *executable = g_file_read_link("/proc/self/exe", &error);
  if (!executable) {
    g_clear_error(&error);
    executable = g_find_program_in_path(g_get_prgname());
  }
  if (!executable) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Could not locate the Wig executable");
    return;
  }

  wig_app_installer_install_async(self->app_installer, data->id, data->start_url, data->manifest, executable,
                                  data->icon_bytes, data->icon_content_type, (const char *const *)protocols,
                                  data->needs_icon_update, data->old_desktop_file, data->old_icon_file,
                                  g_task_get_cancellable(task), app_installed, g_object_ref(task));
}

static char *load_association_scope(const char *path, const char *id, const char *origin, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  json_parser_set_strict(parser, TRUE);
  if (!json_parser_load_from_file(parser, path, error))
    return NULL;

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Origin association must be a JSON object");
    return NULL;
  }

  JsonNode *association_node = json_object_get_member(json_node_get_object(root), id);
  if (!association_node || !JSON_NODE_HOLDS_OBJECT(association_node)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Origin association has no entry for '%s'", id);
    return NULL;
  }

  const char *scope_value = "/";
  JsonNode *scope_node = json_object_get_member(json_node_get_object(association_node), "scope");
  if (scope_node) {
    if (!JSON_NODE_HOLDS_VALUE(scope_node) || json_node_get_value_type(scope_node) != G_TYPE_STRING) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Origin association scope is not a string");
      return NULL;
    }
    scope_value = json_node_get_string(scope_node);
  }

  g_autofree char *base = g_strconcat(origin, "/", NULL);
  g_autofree char *resolved = g_uri_resolve_relative(base, scope_value, G_URI_FLAGS_PARSE_RELAXED, error);
  if (!resolved)
    return NULL;
  if (!uris_have_same_origin(origin, resolved)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Origin association scope resolves to another origin");
    return NULL;
  }
  return normalize_scope_url(resolved, error);
}

static void validate_next_scope_extension(GTask *task);
static void load_desktop_icon(GTask *task);

static void association_download_finished(WebKitDownload *download, GTask *task)
{
  g_autoptr(GTask) owned_task = g_object_ref(task);
  g_signal_handlers_disconnect_by_data(download, task);

  if (g_task_return_error_if_cancelled(task))
    return;

  InstallData *data = g_task_get_task_data(task);
  const char *origin = g_ptr_array_index(data->extension_origins, data->extension_index);
  if (data->download_error) {
    g_debug("pwa: scope extension %s was not associated: %s", origin, data->download_error->message);
    g_clear_error(&data->download_error);
  } else {
    WebKitURIResponse *response = webkit_download_get_response(download);
    guint status_code = response ? webkit_uri_response_get_status_code(response) : 0;
    const char *response_uri = response ? webkit_uri_response_get_uri(response) : NULL;
    if (status_code < 200 || status_code >= 300 || !response_uri || !uris_have_same_origin(origin, response_uri)) {
      g_debug("pwa: scope extension %s returned an invalid association response", origin);
    } else {
      g_autoptr(GError) error = NULL;
      g_autofree char *scope = load_association_scope(data->association_path, data->id, origin, &error);
      if (scope) {
        g_debug("pwa: validated scope extension %s as %s", origin, scope);
        g_ptr_array_add(data->extension_scopes, g_steal_pointer(&scope));
      } else {
        g_debug("pwa: scope extension %s was not associated: %s", origin, error->message);
      }
    }
  }

  if (data->association_path)
    g_unlink(data->association_path);
  g_clear_pointer(&data->association_path, g_free);
  data->extension_index++;
  validate_next_scope_extension(task);
}

static void validate_next_scope_extension(GTask *task)
{
  InstallData *data = g_task_get_task_data(task);
  if (data->extension_index >= data->extension_origins->len) {
    load_desktop_icon(task);
    return;
  }

  WigPwaManager *self = WIG_PWA_MANAGER(g_task_get_source_object(task));
  const char *origin = g_ptr_array_index(data->extension_origins, data->extension_index);
  if (!data->temporary_directory) {
    g_autoptr(GError) error = NULL;
    data->temporary_directory = g_dir_make_tmp("wig-pwa-association-XXXXXX", &error);
    if (!data->temporary_directory) {
      g_task_return_error(task, g_steal_pointer(&error));
      return;
    }
  }
  g_autofree char *association_url = g_strconcat(origin, "/.well-known/web-app-origin-association", NULL);
  data->association_path = g_strdup_printf("%s/association-%u.json", data->temporary_directory, data->extension_index);

  g_autoptr(WebKitDownload) download = webkit_network_session_download_uri(self->network_session, association_url);
  g_signal_connect_data(download, "failed", G_CALLBACK(association_download_failed), g_object_ref(task),
                        task_closure_notify, 0);
  g_signal_connect_data(download, "finished", G_CALLBACK(association_download_finished), g_object_ref(task),
                        task_closure_notify, 0);
  webkit_download_set_destination(download, data->association_path);

  GCancellable *cancellable = g_task_get_cancellable(task);
  if (cancellable) {
    g_signal_connect_object(cancellable, "cancelled", G_CALLBACK(webkit_download_cancel), download, G_CONNECT_SWAPPED);
    if (g_cancellable_is_cancelled(cancellable))
      webkit_download_cancel(download);
  }
}

static void read_desktop_icon(GTask *task);

static void desktop_icon_read_finished(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;

  if (g_task_return_error_if_cancelled(task))
    return;

  InstallData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source_object), result, &error);
  if (!bytes) {
    g_debug("pwa: using fallback desktop icon: %s", error->message);
    g_clear_pointer(&data->icon_data, g_byte_array_unref);
    finish_install(task);
    return;
  }

  gsize length;
  const guint8 *contents = g_bytes_get_data(bytes, &length);
  if (length == 0) {
    data->icon_bytes = g_byte_array_free_to_bytes(g_steal_pointer(&data->icon_data));
    finish_install(task);
    return;
  }
  if (length > MAX_ICON_BYTES || data->icon_data->len > MAX_ICON_BYTES - length) {
    g_debug("pwa: using fallback desktop icon: icon is too large");
    g_clear_pointer(&data->icon_data, g_byte_array_unref);
    finish_install(task);
    return;
  }

  g_byte_array_append(data->icon_data, contents, length);
  read_desktop_icon(task);
}

static void read_desktop_icon(GTask *task)
{
  InstallData *data = g_task_get_task_data(task);
  g_input_stream_read_bytes_async(data->icon_stream, 64 * 1024, G_PRIORITY_DEFAULT, g_task_get_cancellable(task),
                                  desktop_icon_read_finished, g_object_ref(task));
}

static void desktop_icon_loaded(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  if (g_task_return_error_if_cancelled(task))
    return;

  InstallData *data = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  data->icon_stream = g_loadable_icon_load_finish(G_LOADABLE_ICON(source_object), result, &data->icon_content_type,
                                                  &error);
  if (!data->icon_stream) {
    g_debug("pwa: using fallback desktop icon: %s", error->message);
    finish_install(task);
    return;
  }

  data->icon_data = g_byte_array_new();
  read_desktop_icon(task);
}

static void load_desktop_icon(GTask *task)
{
  InstallData *data = g_task_get_task_data(task);
  if (!data->icon) {
    finish_install(task);
    return;
  }

  g_loadable_icon_load_async(data->icon, DESKTOP_ICON_SIZE, g_task_get_cancellable(task), desktop_icon_loaded,
                             g_object_ref(task));
}
#endif

static void wig_pwa_manager_dispose(GObject *object)
{
  WigPwaManager *self = WIG_PWA_MANAGER(object);

  if (self->entries_initialized) {
    g_variant_dict_clear(&self->entries);
    self->entries_initialized = FALSE;
  }
  g_clear_object(&self->network_session);
  g_clear_object(&self->app_installer);

  G_OBJECT_CLASS(wig_pwa_manager_parent_class)->dispose(object);
}

static void wig_pwa_manager_finalize(GObject *object)
{
  WigPwaManager *self = WIG_PWA_MANAGER(object);

  g_clear_pointer(&self->path, g_free);
  g_clear_pointer(&self->refresh_attempts, g_hash_table_unref);

  G_OBJECT_CLASS(wig_pwa_manager_parent_class)->finalize(object);
}

static void wig_pwa_manager_class_init(WigPwaManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = wig_pwa_manager_dispose;
  object_class->finalize = wig_pwa_manager_finalize;

  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void wig_pwa_manager_init(WigPwaManager *self)
{
  self->refresh_attempts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

WigPwaManager *wig_pwa_manager_new(const char *config_dir, const char *data_dir, WebKitNetworkSession *network_session)
{
  g_return_val_if_fail(config_dir != NULL, NULL);
  g_return_val_if_fail(data_dir != NULL, NULL);
  g_return_val_if_fail(WEBKIT_IS_NETWORK_SESSION(network_session), NULL);

  WigPwaManager *self = g_object_new(WIG_TYPE_PWA_MANAGER, NULL);
  g_autofree char *data_home = g_path_get_dirname(data_dir);
  self->app_installer = wig_in_flatpak() ? wig_app_installer_portal_new() : wig_app_installer_native_new(data_home);
  self->path = g_build_filename(config_dir, WEB_APPLICATIONS_FILE, NULL);
  g_set_object(&self->network_session, network_session);
  g_autoptr(GError) error = NULL;
  if (!ensure_entries_loaded(self, &error))
    g_warning("Failed to load installed web apps: %s", error->message);
  return self;
}

#if HAVE_APPLICATION_MANIFEST_SUPPORT
static void install_async(WigPwaManager *self, WebKitApplicationManifest *manifest, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data, gboolean throttled)
{
  g_return_if_fail(WIG_IS_PWA_MANAGER(self));
  g_return_if_fail(WEBKIT_IS_APPLICATION_MANIFEST(manifest));
  g_return_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable));

  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, throttled ? wig_pwa_manager_refresh_async : wig_pwa_manager_install_async);

  const char *id = webkit_application_manifest_get_id(manifest);
  if (!id || !*id) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Web app manifest has no application ID");
    return;
  }

  if (throttled) {
    g_autoptr(GError) error = NULL;
    if (!ensure_entries_loaded(self, &error)) {
      g_task_return_error(task, g_steal_pointer(&error));
      return;
    }
    g_autoptr(GVariant) entry = g_variant_dict_lookup_value(&self->entries, id, G_VARIANT_TYPE_VARDICT);
    if (!entry) {
      g_task_return_boolean(task, TRUE);
      return;
    }
    gint64 last_refreshed = 0;
    g_variant_lookup(entry, "last_updated", "x", &last_refreshed);
    const gint64 *last_attempt = g_hash_table_lookup(self->refresh_attempts, id);
    if (last_attempt)
      last_refreshed = MAX(last_refreshed, *last_attempt);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (last_refreshed > 0 && now - last_refreshed < MANIFEST_REFRESH_INTERVAL) {
      g_debug("pwa: skipping manifest refresh for %s; last attempted less than 12 hours ago", id);
      g_task_return_boolean(task, TRUE);
      return;
    }
    g_hash_table_insert(self->refresh_attempts, g_strdup(id), g_memdup2(&now, sizeof(now)));
  }

  const char *manifest_uri = webkit_application_manifest_get_manifest_uri(manifest);
  if (!manifest_uri || !*manifest_uri) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Web app manifest has no manifest URI");
    return;
  }

  const char *start_uri = webkit_application_manifest_get_start_uri(manifest);
  if (!start_uri || !*start_uri) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Web app manifest has no start URL");
    return;
  }

  const char *scope = webkit_application_manifest_get_scope(manifest);
  if (!scope || !*scope) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Web app manifest has no scope");
    return;
  }

  GVariant *full_manifest = webkit_application_manifest_get_full_manifest(manifest);
  if (!full_manifest || !g_variant_is_of_type(full_manifest, G_VARIANT_TYPE_VARDICT)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "WebKit returned an invalid full web app manifest");
    return;
  }

  InstallData *data = g_new0(InstallData, 1);
  data->id = g_strdup(id);
  data->manifest_url = g_strdup(manifest_uri);
  data->start_url = g_strdup(start_uri);
  data->scope = g_strdup(scope);
  data->throttled = throttled;
  data->manifest = manifest_with_webkit_strings(manifest, full_manifest);
  g_autoptr(GVariant) raw_background_color = g_variant_lookup_value(data->manifest, "background_color",
                                                                    G_VARIANT_TYPE_STRING);
  if (raw_background_color)
    data->background_color = manifest_color_to_css(webkit_application_manifest_get_background_color(manifest));
  data->shortcuts = manifest_shortcuts_from_webkit(manifest);
  g_task_set_task_data(task, data, (GDestroyNotify)install_data_free);

  if (g_task_return_error_if_cancelled(task))
    return;

  g_autoptr(GError) error = NULL;
  if (!install_data_prepare(self, data, &error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }
  if (!data->metadata_changed) {
    if (data->throttled && !update_last_updated(self, data->id, &error)) {
      g_task_return_error(task, g_steal_pointer(&error));
      return;
    }
    g_debug("pwa: installed web app %s is unchanged", data->id);
    g_task_return_boolean(task, TRUE);
    return;
  }

  if (data->needs_icon_update || data->needs_desktop_update) {
    WebKitApplicationManifestIcon *icon = wig_pwa_icon_select(webkit_application_manifest_get_icons(manifest),
                                                              DESKTOP_ICON_SIZE);
    if (icon)
      data->icon = G_LOADABLE_ICON(g_object_ref(icon));
  }

  validate_next_scope_extension(task);
}

void wig_pwa_manager_install_async(WigPwaManager *self, WebKitApplicationManifest *manifest, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer user_data)
{
  install_async(self, manifest, cancellable, callback, user_data, FALSE);
}

void wig_pwa_manager_refresh_async(WigPwaManager *self, WebKitApplicationManifest *manifest, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer user_data)
{
  install_async(self, manifest, cancellable, callback, user_data, TRUE);
}

gboolean wig_pwa_manager_install_finish(WigPwaManager *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  g_return_val_if_fail(g_async_result_is_tagged(result, wig_pwa_manager_install_async)
                           || g_async_result_is_tagged(result, wig_pwa_manager_refresh_async),
                       FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  return g_task_propagate_boolean(G_TASK(result), error);
}
#endif

gboolean wig_pwa_manager_is_installed(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  return ensure_entries_loaded(self, error) && g_variant_dict_contains(&self->entries, id);
}

GVariant *wig_pwa_manager_list(WigPwaManager *self, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  GVariantBuilder apps_builder;
  g_variant_builder_init(&apps_builder, G_VARIANT_TYPE("aa{sv}"));
  if (!ensure_entries_loaded(self, error)) {
    g_variant_builder_clear(&apps_builder);
    return NULL;
  }

  g_autoptr(GVariant) entries = snapshot_entries(self);
  GVariantIter iterator;
  g_variant_iter_init(&iterator, entries);
  while (TRUE) {
    g_autoptr(GVariant) dictionary_entry = g_variant_iter_next_value(&iterator);
    if (!dictionary_entry)
      break;

    const char *id;
    g_autoptr(GVariant) entry = NULL;
    g_variant_get(dictionary_entry, "{&sv}", &id, &entry);
    g_autoptr(GVariant) manifest = entry && g_variant_is_of_type(entry, G_VARIANT_TYPE_VARDICT)
        ? g_variant_lookup_value(entry, "manifest", G_VARIANT_TYPE_VARDICT)
        : NULL;
    const char *manifest_url = entry ? wig_pwa_variant_lookup_string(entry, "manifest_url") : NULL;
    if (!manifest || !manifest_url) {
      g_variant_builder_clear(&apps_builder);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid web app database entry '%s'", id);
      return NULL;
    }

    const char *name = wig_pwa_manifest_get_name(manifest, id);
    const char *description = wig_pwa_variant_lookup_string(manifest, "description");

    GVariantBuilder app_builder;
    g_variant_builder_init(&app_builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&app_builder, "{sv}", "id", g_variant_new_string(id));
    g_variant_builder_add(&app_builder, "{sv}", "name", g_variant_new_string(name));
    g_variant_builder_add(&app_builder, "{sv}", "description", g_variant_new_string(description ? description : ""));
    g_variant_builder_add_value(&apps_builder, g_variant_builder_end(&app_builder));
  }

  return g_variant_ref_sink(g_variant_builder_end(&apps_builder));
}

GVariant *wig_pwa_manager_get_details(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return NULL;

  g_autoptr(GVariant) manifest = g_variant_lookup_value(entry, "manifest", G_VARIANT_TYPE_VARDICT);
  const char *manifest_url = wig_pwa_variant_lookup_string(entry, "manifest_url");
  const char *start_url = wig_pwa_variant_lookup_string(entry, "start_url");
  const char *scope = wig_pwa_variant_lookup_string(entry, "scope");
  if (!manifest || !manifest_url || !*manifest_url || !start_url || !*start_url || !scope || !*scope) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app '%s' has invalid stored details", id);
    return NULL;
  }
  return g_steal_pointer(&entry);
}

GBytes *wig_pwa_manager_get_cached_icon(WigPwaManager *self, const char *id, char **content_type, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(content_type != NULL && *content_type == NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return NULL;
  const char *desktop_file = wig_pwa_variant_lookup_string(entry, "desktop_file");
  const char *icon_file = wig_pwa_variant_lookup_string(entry, "icon_file");
  if (!desktop_file) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Web app has no app launcher");
    return NULL;
  }
  return wig_app_installer_load_icon(self->app_installer, id, desktop_file, icon_file, content_type, error);
}

char *wig_pwa_manager_get_start_url(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return NULL;

  const char *start_url = wig_pwa_variant_lookup_string(entry, "start_url");
  g_autofree char *resolved_start_url = start_url ? g_strdup(start_url) : NULL;
  if (!resolved_start_url) {
    g_autoptr(GVariant) manifest = g_variant_lookup_value(entry, "manifest", G_VARIANT_TYPE_VARDICT);
    const char *manifest_url = wig_pwa_variant_lookup_string(entry, "manifest_url");
    const char *relative_start_url = manifest ? wig_pwa_variant_lookup_string(manifest, "start_url") : NULL;
    if (manifest_url && relative_start_url && *relative_start_url)
      resolved_start_url = g_uri_resolve_relative(manifest_url, relative_start_url, G_URI_FLAGS_PARSE_RELAXED, error);
  }

  if (!resolved_start_url) {
    if (!error || !*error)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app '%s' has no launch URL", id);
    return NULL;
  }

  const char *scheme = g_uri_peek_scheme(resolved_start_url);
  if (!scheme || (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app '%s' has an invalid launch URL", id);
    return NULL;
  }

  return g_steal_pointer(&resolved_start_url);
}

char *wig_pwa_manager_resolve_protocol_url(WigPwaManager *self, const char *id, const char *url, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(url != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return NULL;
  g_autoptr(GVariant) manifest = g_variant_lookup_value(entry, "manifest", G_VARIANT_TYPE_VARDICT);
  const char *manifest_url = wig_pwa_variant_lookup_string(entry, "manifest_url");
  if (!manifest || !manifest_url) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app '%s' has no stored manifest", id);
    return NULL;
  }

  g_autoptr(WigPwaNavigationPolicy) navigation_policy = wig_pwa_manager_get_navigation_policy(self, id, error);
  if (!navigation_policy)
    return NULL;
  return wig_pwa_protocol_handlers_resolve(manifest, manifest_url, navigation_policy, url);
}

WigPwaLaunchClientMode wig_pwa_manager_get_launch_client_mode(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), WIG_PWA_LAUNCH_CLIENT_MODE_AUTO);
  g_return_val_if_fail(id != NULL, WIG_PWA_LAUNCH_CLIENT_MODE_AUTO);
  g_return_val_if_fail(error == NULL || *error == NULL, WIG_PWA_LAUNCH_CLIENT_MODE_AUTO);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;

  g_autoptr(GVariant) manifest = g_variant_lookup_value(entry, "manifest", G_VARIANT_TYPE_VARDICT);
  if (!manifest) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app '%s' has no stored manifest", id);
    return WIG_PWA_LAUNCH_CLIENT_MODE_AUTO;
  }

  g_autoptr(GVariant) overrides = g_variant_lookup_value(entry, "overrides", G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) launch_handler = overrides
      ? g_variant_lookup_value(overrides, "launch_handler", G_VARIANT_TYPE_VARDICT)
      : NULL;
  g_autoptr(GVariant) client_mode = launch_handler ? g_variant_lookup_value(launch_handler, "client_mode", NULL) : NULL;
  if (client_mode)
    return wig_pwa_launch_handler_get_client_mode(overrides);
  return wig_pwa_launch_handler_get_client_mode(manifest);
}

static gboolean launch_client_mode_is_valid(const char *client_mode)
{
  return !client_mode || g_str_equal(client_mode, "auto") || g_str_equal(client_mode, "focus-existing")
      || g_str_equal(client_mode, "navigate-existing") || g_str_equal(client_mode, "navigate-new");
}

gboolean wig_pwa_manager_set_launch_client_mode_override(WigPwaManager *self, const char *id, const char *client_mode,
                                                         GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  if (!launch_client_mode_is_valid(client_mode)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid launch client mode '%s'", client_mode);
    return FALSE;
  }

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return FALSE;
  g_autoptr(GVariant) old_overrides = g_variant_lookup_value(entry, "overrides", G_VARIANT_TYPE_VARDICT);
  GVariantDict overrides_dict;
  g_variant_dict_init(&overrides_dict, old_overrides);
  g_autoptr(GVariant) old_launch_handler = old_overrides
      ? g_variant_lookup_value(old_overrides, "launch_handler", G_VARIANT_TYPE_VARDICT)
      : NULL;
  GVariantDict launch_handler_dict;
  g_variant_dict_init(&launch_handler_dict, old_launch_handler);
  if (client_mode) {
    g_variant_dict_insert(&launch_handler_dict, "client_mode", "s", client_mode);
  } else {
    g_variant_dict_remove(&launch_handler_dict, "client_mode");
  }
  g_autoptr(GVariant) launch_handler = g_variant_ref_sink(g_variant_dict_end(&launch_handler_dict));
  if (g_variant_n_children(launch_handler) > 0)
    g_variant_dict_insert_value(&overrides_dict, "launch_handler", launch_handler);
  else
    g_variant_dict_remove(&overrides_dict, "launch_handler");
  g_autoptr(GVariant) overrides = g_variant_ref_sink(g_variant_dict_end(&overrides_dict));

  GVariantDict entry_dict;
  g_variant_dict_init(&entry_dict, entry);
  if (g_variant_n_children(overrides) > 0)
    g_variant_dict_insert_value(&entry_dict, "overrides", overrides);
  else
    g_variant_dict_remove(&entry_dict, "overrides");
  g_autoptr(GVariant) updated_entry = g_variant_ref_sink(g_variant_dict_end(&entry_dict));
  return update_entry(self, id, updated_entry, error);
}

gboolean wig_pwa_manager_get_open_primary_scope_in_browser(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return FALSE;
  g_autoptr(GVariant) value = g_variant_lookup_value(entry, "open_primary_scope_in_browser", NULL);
  if (!value)
    return FALSE;
  if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Web app '%s' has an invalid primary scope navigation setting", id);
    return FALSE;
  }
  return g_variant_get_boolean(value);
}

gboolean wig_pwa_manager_set_open_primary_scope_in_browser(WigPwaManager *self, const char *id, gboolean enabled,
                                                           GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return FALSE;
  GVariantDict entry_dict;
  g_variant_dict_init(&entry_dict, entry);
  g_variant_dict_insert(&entry_dict, "open_primary_scope_in_browser", "b", enabled);
  g_autoptr(GVariant) updated_entry = g_variant_ref_sink(g_variant_dict_end(&entry_dict));
  return update_entry(self, id, updated_entry, error);
}

char *wig_pwa_manager_find_for_primary_scope(WigPwaManager *self, const char *url, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(url != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  const char *scheme = g_uri_peek_scheme(url);
  if (g_strcmp0(scheme, "https") != 0 && g_strcmp0(scheme, "http") != 0)
    return NULL;

  if (!ensure_entries_loaded(self, error))
    return NULL;

  g_autofree char *best_id = NULL;
  gsize best_scope_length = 0;
  g_autoptr(GVariant) entries = snapshot_entries(self);
  GVariantIter iterator;
  g_variant_iter_init(&iterator, entries);
  while (TRUE) {
    g_autoptr(GVariant) dictionary_entry = g_variant_iter_next_value(&iterator);
    if (!dictionary_entry)
      break;

    const char *id;
    g_autoptr(GVariant) entry = NULL;
    g_variant_get(dictionary_entry, "{&sv}", &id, &entry);
    gboolean open_in_browser = FALSE;
    g_autoptr(GVariant) setting = g_variant_lookup_value(entry, "open_primary_scope_in_browser", NULL);
    if (setting) {
      if (!g_variant_is_of_type(setting, G_VARIANT_TYPE_BOOLEAN)) {
        g_debug("pwa: ignoring '%s' with an invalid primary scope navigation setting", id);
        continue;
      }
      open_in_browser = g_variant_get_boolean(setting);
    }
    if (open_in_browser)
      continue;

    const char *scope = wig_pwa_variant_lookup_string(entry, "scope");
    g_autoptr(GError) policy_error = NULL;
    g_autoptr(WigPwaNavigationPolicy) policy = scope ? wig_pwa_navigation_policy_new(scope, NULL, &policy_error) : NULL;
    if (!policy) {
      g_debug("pwa: ignoring '%s' with an invalid primary scope%s%s", id, policy_error ? ": " : "",
              policy_error ? policy_error->message : "");
      continue;
    }
    if (wig_pwa_navigation_policy_match(policy, url) != WIG_PWA_SCOPE_MATCH_PRIMARY)
      continue;

    gsize scope_length = strlen(scope);
    if (!best_id || scope_length > best_scope_length
        || (scope_length == best_scope_length && g_strcmp0(id, best_id) < 0)) {
      g_free(best_id);
      best_id = g_strdup(id);
      best_scope_length = scope_length;
    }
  }
  return g_steal_pointer(&best_id);
}

char *wig_pwa_manager_get_background_color(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return NULL;
  g_autoptr(GVariant) stored_color = g_variant_lookup_value(entry, "background_color", G_VARIANT_TYPE_STRING);
  if (stored_color)
    return g_variant_dup_string(stored_color, NULL);
  return NULL;
}

WigPwaNavigationPolicy *wig_pwa_manager_get_navigation_policy(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return NULL;

  g_autofree char *start_url = wig_pwa_manager_get_start_url(self, id, error);
  if (!start_url)
    return NULL;

  const char *stored_scope = wig_pwa_variant_lookup_string(entry, "scope");
  g_autofree char *primary_scope = stored_scope ? normalize_scope_url(stored_scope, error) : NULL;
  if (stored_scope && !primary_scope)
    return NULL;

  if (!primary_scope) {
    g_autoptr(GVariant) manifest = g_variant_lookup_value(entry, "manifest", G_VARIANT_TYPE_VARDICT);
    const char *manifest_url = wig_pwa_variant_lookup_string(entry, "manifest_url");
    const char *raw_scope = manifest ? wig_pwa_variant_lookup_string(manifest, "scope") : NULL;
    if (manifest_url && raw_scope && *raw_scope) {
      g_autoptr(GError) local_error = NULL;
      g_autofree char *resolved = g_uri_resolve_relative(manifest_url, raw_scope, G_URI_FLAGS_PARSE_RELAXED,
                                                         &local_error);
      if (resolved)
        primary_scope = normalize_scope_url(resolved, &local_error);
    }

    if (primary_scope) {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(WigPwaNavigationPolicy) candidate = wig_pwa_navigation_policy_new(primary_scope, NULL, &local_error);
      if (!candidate || wig_pwa_navigation_policy_match(candidate, start_url) != WIG_PWA_SCOPE_MATCH_PRIMARY)
        g_clear_pointer(&primary_scope, g_free);
    }
  }

  if (!primary_scope) {
    g_autofree char *fallback = g_uri_resolve_relative(start_url, ".", G_URI_FLAGS_PARSE_RELAXED, error);
    if (!fallback)
      return NULL;
    primary_scope = normalize_scope_url(fallback, error);
    if (!primary_scope)
      return NULL;
  }

  g_autoptr(GVariant) extensions = g_variant_lookup_value(entry, "scope_extensions", G_VARIANT_TYPE_STRING_ARRAY);
  g_auto(GStrv) extension_scopes = extensions ? g_variant_dup_strv(extensions, NULL) : NULL;
  return wig_pwa_navigation_policy_new(primary_scope, (const char *const *)extension_scopes, error);
}

GVariant *wig_pwa_manager_get_shortcuts(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autoptr(GVariant) entry = lookup_entry(self, id, error);
  if (!entry)
    return NULL;

  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_manager_get_navigation_policy(self, id, error);
  if (!policy)
    return NULL;

  GVariantBuilder shortcuts_builder;
  g_variant_builder_init(&shortcuts_builder, G_VARIANT_TYPE("aa{sv}"));
  g_autoptr(GVariant) shortcuts = g_variant_lookup_value(entry, "shortcuts", G_VARIANT_TYPE("aa{sv}"));
  for (gsize i = 0; shortcuts && i < g_variant_n_children(shortcuts); i++) {
    g_autoptr(GVariant) shortcut = g_variant_get_child_value(shortcuts, i);
    const char *name = wig_pwa_variant_lookup_string(shortcut, "name");
    const char *url = wig_pwa_variant_lookup_string(shortcut, "url");
    if (name && *name && url && wig_pwa_navigation_policy_match(policy, url) != WIG_PWA_SCOPE_MATCH_OUTSIDE)
      g_variant_builder_add_value(&shortcuts_builder, shortcut);
  }

  GVariant *result = g_variant_ref_sink(g_variant_builder_end(&shortcuts_builder));
  g_debug("pwa: loaded %zu shortcuts for %s", g_variant_n_children(result), id);
  return result;
}

gboolean wig_pwa_manager_uninstall(WigPwaManager *self, const char *id, GError **error)
{
  g_return_val_if_fail(WIG_IS_PWA_MANAGER(self), FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  if (!ensure_entries_loaded(self, error))
    return FALSE;
  if (!g_variant_dict_contains(&self->entries, id)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Web app '%s' is not installed", id);
    return FALSE;
  }

  g_autoptr(GVariant) installed_entry = g_variant_dict_lookup_value(&self->entries, id, G_VARIANT_TYPE_VARDICT);
  const char *desktop_file = installed_entry ? wig_pwa_variant_lookup_string(installed_entry, "desktop_file") : NULL;
  const char *icon_file = installed_entry ? wig_pwa_variant_lookup_string(installed_entry, "icon_file") : NULL;
  if (!desktop_file) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Web app '%s' has no app launcher", id);
    return FALSE;
  }
  if (!wig_app_installer_uninstall(self->app_installer, id, desktop_file, icon_file, error))
    return FALSE;

  if (!remove_entry(self, id, error))
    return FALSE;

  g_debug("Uninstalled web app %s from %s", id, self->path);
  return TRUE;
}
