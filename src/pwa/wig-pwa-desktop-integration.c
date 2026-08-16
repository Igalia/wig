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

#include "wig-pwa-desktop-integration.h"

#include "wig-pwa-util.h"

#include <errno.h>
#include <glib/gstdio.h>

#define DESKTOP_GROUP "Desktop Entry"
#define MAX_ICON_BYTES (10 * 1024 * 1024)
#define MAX_ICON_DIMENSION 4096

static const guchar png_signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

static const char fallback_icon[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 256 256\">"
                                    "<rect width=\"256\" height=\"256\" rx=\"56\" fill=\"#315aa8\"/>"
                                    "<path d=\"M52 66h35l41 119 41-119h35l-57 154h-38z\" fill=\"#fff\"/>"
                                    "</svg>";

static char *launcher_name(const char *id)
{
  g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
  return g_strconcat("com.igalia.wig-web-apps.", hash, NULL);
}

static char *quote_exec_argument(const char *argument)
{
  g_autoptr(GString) quoted = g_string_new("\"");
  for (const char *p = argument; *p; p++) {
    if (*p == '%') {
      g_string_append(quoted, "%%");
      continue;
    }
    if (*p == '\"' || *p == '\\' || *p == '$' || *p == '`')
      g_string_append_c(quoted, '\\');
    g_string_append_c(quoted, *p);
  }
  g_string_append_c(quoted, '\"');
  return g_string_free(g_steal_pointer(&quoted), FALSE);
}

static gboolean write_bytes(const char *path, GBytes *bytes, GError **error)
{
  gsize size;
  const char *data = g_bytes_get_data(bytes, &size);
  if (size > G_MAXSSIZE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Desktop integration file is too large");
    return FALSE;
  }
  return g_file_set_contents_full(path, data, (gssize)size, G_FILE_SET_CONTENTS_CONSISTENT, 0644, error);
}

static GBytes *validate_icon(GBytes *icon, const char *content_type, GError **error)
{
  if (!icon)
    return NULL;

  gsize length;
  const guchar *contents = g_bytes_get_data(icon, &length);
  if (length > MAX_ICON_BYTES) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Web app icon is too large");
    return NULL;
  }
  if (g_strcmp0(content_type, "image/png") != 0 || length < sizeof(png_signature)
      || memcmp(contents, png_signature, sizeof(png_signature)) != 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "WebKit did not provide a rasterized PNG icon");
    return NULL;
  }
  return g_bytes_ref(icon);
}

static gboolean relative_path_is_safe(const char *path)
{
  if (!path || g_path_is_absolute(path))
    return FALSE;
  g_auto(GStrv) components = g_strsplit(path, G_DIR_SEPARATOR_S, -1);
  for (guint i = 0; components[i]; i++) {
    if (g_str_equal(components[i], ".."))
      return FALSE;
  }
  return TRUE;
}

static gboolean remove_file(const char *path, GError **error)
{
  if (g_unlink(path) == 0 || errno == ENOENT)
    return TRUE;
  g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to remove '%s': %s", path, g_strerror(errno));
  return FALSE;
}

static gboolean icon_file_is_valid(const char *id, const char *icon_file, GError **error)
{
  if (!icon_file || !*icon_file) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Web app has no cached icon");
    return FALSE;
  }

  g_autofree char *launcher = launcher_name(id);
  g_autofree char *png_basename = g_strconcat(launcher, ".png", NULL);
  g_autofree char *svg_basename = g_strconcat(launcher, ".svg", NULL);
  g_autofree char *basename = g_path_get_basename(icon_file);
  if (!relative_path_is_safe(icon_file) || !g_str_has_prefix(icon_file, "icons/hicolor/")
      || (!g_str_equal(basename, png_basename) && !g_str_equal(basename, svg_basename))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid stored web app icon path");
    return FALSE;
  }
  return TRUE;
}

static gboolean write_desktop_file(const char *data_home, const char *id, GVariant *manifest, const char *executable,
                                   const char *const *protocols, GError **error)
{
  g_autofree char *launcher = launcher_name(id);
  g_autofree char *desktop_basename = g_strconcat(launcher, ".desktop", NULL);
  g_autofree char *applications_dir = g_build_filename(data_home, "applications", NULL);
  g_autofree char *desktop_path = g_build_filename(applications_dir, desktop_basename, NULL);
  if (g_mkdir_with_parents(applications_dir, 0755) == -1) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to create '%s': %s", applications_dir,
                g_strerror(errno));
    return FALSE;
  }

  const char *name = wig_pwa_variant_lookup_string(manifest, "name");
  if (!name || !*name)
    name = wig_pwa_variant_lookup_string(manifest, "short_name");
  if (!name || !*name)
    name = id;

  g_autoptr(GKeyFile) desktop = wig_pwa_desktop_entry_new(id, manifest, executable, protocols);
  g_key_file_set_string(desktop, DESKTOP_GROUP, "Name", name);
  g_key_file_set_string(desktop, DESKTOP_GROUP, "Icon", launcher);
  g_key_file_set_string(desktop, DESKTOP_GROUP, "TryExec", executable);

  return g_key_file_save_to_file(desktop, desktop_path, error);
}

GKeyFile *wig_pwa_desktop_entry_new(const char *id, GVariant *manifest, const char *executable,
                                    const char *const *protocols)
{
  const char *description = wig_pwa_variant_lookup_string(manifest, "description");

  GKeyFile *desktop = g_key_file_new();
  g_key_file_set_string(desktop, DESKTOP_GROUP, "Type", "Application");
  if (description && *description)
    g_key_file_set_string(desktop, DESKTOP_GROUP, "Comment", description);
  g_autofree char *quoted_executable = quote_exec_argument(executable);
  g_autofree char *web_app_argument = g_strconcat("--web-app=", id, NULL);
  g_autofree char *quoted_web_app_argument = quote_exec_argument(web_app_argument);
  g_autofree char *exec = g_strconcat(quoted_executable, " ", quoted_web_app_argument, " %U", NULL);
  g_key_file_set_string(desktop, DESKTOP_GROUP, "Exec", exec);
  g_key_file_set_boolean(desktop, DESKTOP_GROUP, "Terminal", FALSE);
  const char *categories[] = { "Network" };
  g_key_file_set_string_list(desktop, DESKTOP_GROUP, "Categories", categories, G_N_ELEMENTS(categories));
  g_key_file_set_string(desktop, DESKTOP_GROUP, "X-Wig-Web-App-ID", id);

  if (protocols && protocols[0]) {
    g_autoptr(GPtrArray) mime_types = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; protocols[i]; i++)
      g_ptr_array_add(mime_types, g_strconcat("x-scheme-handler/", protocols[i], NULL));
    g_key_file_set_string_list(desktop, DESKTOP_GROUP, "MimeType", (const char *const *)mime_types->pdata,
                               mime_types->len);
  }

  return desktop;
}

GBytes *wig_pwa_desktop_fallback_icon(void)
{
  return g_bytes_new_static(fallback_icon, sizeof(fallback_icon) - 1);
}

WigPwaDesktopArtifacts *wig_pwa_desktop_install(const char *data_home, const char *id, GVariant *manifest,
                                                const char *executable, GBytes *icon, const char *icon_content_type,
                                                int icon_size, const char *const *protocols, GError **error)
{
  g_return_val_if_fail(data_home != NULL, NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(manifest && g_variant_is_of_type(manifest, G_VARIANT_TYPE_VARDICT), NULL);
  g_return_val_if_fail(executable != NULL, NULL);
  g_return_val_if_fail(icon_size >= 0 && icon_size <= MAX_ICON_DIMENSION, NULL);
  g_return_val_if_fail(icon == NULL || icon_size > 0, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autofree char *launcher = launcher_name(id);
  g_autofree char *desktop_basename = g_strconcat(launcher, ".desktop", NULL);

  g_autoptr(GError) icon_error = NULL;
  g_autoptr(GBytes) icon_bytes = validate_icon(icon, icon_content_type, &icon_error);
  g_autofree char *icon_relative = NULL;
  if (icon_bytes) {
    g_autofree char *size_directory = g_strdup_printf("%dx%d", icon_size, icon_size);
    g_autofree char *icon_basename = g_strconcat(launcher, ".png", NULL);
    icon_relative = g_build_filename("icons", "hicolor", size_directory, "apps", icon_basename, NULL);
  } else {
    if (!icon_bytes && icon_error)
      g_debug("pwa: using fallback desktop icon: %s", icon_error->message);
    g_autofree char *icon_basename = g_strconcat(launcher, ".svg", NULL);
    icon_relative = g_build_filename("icons", "hicolor", "scalable", "apps", icon_basename, NULL);
    if (!icon_bytes)
      icon_bytes = wig_pwa_desktop_fallback_icon();
  }

  g_autofree char *icon_path = g_build_filename(data_home, icon_relative, NULL);
  g_autofree char *icon_directory = g_path_get_dirname(icon_path);
  if (g_mkdir_with_parents(icon_directory, 0755) == -1) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to create '%s': %s", icon_directory,
                g_strerror(errno));
    return NULL;
  }
  if (!write_bytes(icon_path, icon_bytes, error))
    return NULL;

  if (!write_desktop_file(data_home, id, manifest, executable, protocols, error)) {
    g_unlink(icon_path);
    return NULL;
  }

  WigPwaDesktopArtifacts *artifacts = g_new0(WigPwaDesktopArtifacts, 1);
  artifacts->desktop_file = g_build_filename("applications", desktop_basename, NULL);
  artifacts->icon_file = g_steal_pointer(&icon_relative);
  return artifacts;
}

gboolean wig_pwa_desktop_update(const char *data_home, const char *id, GVariant *manifest, const char *executable,
                                const char *const *protocols, GError **error)
{
  g_return_val_if_fail(data_home != NULL, FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(manifest && g_variant_is_of_type(manifest, G_VARIANT_TYPE_VARDICT), FALSE);
  g_return_val_if_fail(executable != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  return write_desktop_file(data_home, id, manifest, executable, protocols, error);
}

gboolean wig_pwa_desktop_remove_icon(const char *data_home, const char *id, const char *icon_file, GError **error)
{
  g_return_val_if_fail(data_home != NULL, FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (!icon_file)
    return TRUE;
  if (!icon_file_is_valid(id, icon_file, error))
    return FALSE;

  g_autofree char *path = g_build_filename(data_home, icon_file, NULL);
  return remove_file(path, error);
}

GBytes *wig_pwa_desktop_load_icon(const char *data_home, const char *id, const char *icon_file, char **content_type,
                                  GError **error)
{
  g_return_val_if_fail(data_home != NULL, NULL);
  g_return_val_if_fail(id != NULL, NULL);
  g_return_val_if_fail(content_type != NULL && *content_type == NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!icon_file_is_valid(id, icon_file, error))
    return NULL;

  g_autofree char *path = g_build_filename(data_home, icon_file, NULL);
  g_autofree char *contents = NULL;
  gsize length;
  if (!g_file_get_contents(path, &contents, &length, error))
    return NULL;
  if (length > MAX_ICON_BYTES) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Cached web app icon is too large");
    return NULL;
  }

  g_autofree char *mime_type = NULL;
  if (g_str_has_suffix(icon_file, ".png") && length >= sizeof(png_signature)
      && memcmp(contents, png_signature, sizeof(png_signature)) == 0) {
    mime_type = g_strdup("image/png");
  } else if (g_str_has_suffix(icon_file, ".svg") && g_utf8_validate(contents, (gssize)length, NULL)
             && g_strstr_len(contents, (gssize)length, "<svg")) {
    mime_type = g_strdup("image/svg+xml");
  } else {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Cached web app icon has an invalid type");
    return NULL;
  }

  *content_type = g_steal_pointer(&mime_type);
  return g_bytes_new_take(g_steal_pointer(&contents), length);
}

gboolean wig_pwa_desktop_uninstall(const char *data_home, const char *id, const char *icon_file, GError **error)
{
  g_return_val_if_fail(data_home != NULL, FALSE);
  g_return_val_if_fail(id != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  g_autofree char *launcher = launcher_name(id);
  g_autofree char *desktop_basename = g_strconcat(launcher, ".desktop", NULL);
  g_autofree char *desktop_path = g_build_filename(data_home, "applications", desktop_basename, NULL);
  if (!remove_file(desktop_path, error))
    return FALSE;
  return wig_pwa_desktop_remove_icon(data_home, id, icon_file, error);
}

void wig_pwa_desktop_artifacts_free(WigPwaDesktopArtifacts *artifacts)
{
  g_clear_pointer(&artifacts->desktop_file, g_free);
  g_clear_pointer(&artifacts->icon_file, g_free);
  g_free(artifacts);
}
