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

#include "wig-settings.h"

#include <errno.h>

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#define WIG_SETTINGS_SCHEMA_ID "com.igalia.wig"
#define WIG_SETTINGS_PATH "/com/igalia/wig/"

static GSettingsSchema *wig_settings_lookup_schema(void)
{
  g_autofree char *executable = g_file_read_link("/proc/self/exe", NULL);
  if (executable) {
    g_autofree char *directory = g_path_get_dirname(executable);
    g_autoptr(GSettingsSchemaSource) source = g_settings_schema_source_new_from_directory(
        directory, g_settings_schema_source_get_default(), TRUE, NULL);
    if (source) {
      GSettingsSchema *schema = g_settings_schema_source_lookup(source, WIG_SETTINGS_SCHEMA_ID, TRUE);
      if (schema)
        return schema;
    }
  }

  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  return source ? g_settings_schema_source_lookup(source, WIG_SETTINGS_SCHEMA_ID, TRUE) : NULL;
}

GSettings *wig_settings_new(void)
{
  g_autoptr(GSettingsSchema) schema = wig_settings_lookup_schema();
  if (!schema)
    g_error("Settings schema '%s' is not installed", WIG_SETTINGS_SCHEMA_ID);

  g_autofree char *config_dir = g_build_filename(g_get_user_config_dir(), "com.igalia.wig", NULL);
  if (g_mkdir_with_parents(config_dir, 0700) < 0)
    g_error("Failed to create settings directory '%s': %s", config_dir, g_strerror(errno));

  g_autofree char *config_path = g_build_filename(config_dir, "settings.ini", NULL);
  g_autoptr(GSettingsBackend) backend = g_keyfile_settings_backend_new(config_path, WIG_SETTINGS_PATH, "Settings");
  return g_settings_new_full(schema, backend, NULL);
}
