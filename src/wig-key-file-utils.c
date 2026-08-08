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

#include "wig-key-file-utils.h"

static gboolean key_file_error_is_missing(const GError *error)
{
  return g_error_matches(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND)
      || g_error_matches(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND);
}

gboolean wig_key_file_get_boolean(GKeyFile *key_file, const char *group, const char *key, gboolean default_value)
{
  g_autoptr(GError) error = NULL;
  gboolean value = g_key_file_get_boolean(key_file, group, key, &error);
  if (!error)
    return value;

  if (!key_file_error_is_missing(error))
    g_warning("key-file: invalid %s.%s: %s", group, key, error->message);
  return default_value;
}

int wig_key_file_get_integer(GKeyFile *key_file, const char *group, const char *key, int default_value)
{
  g_autoptr(GError) error = NULL;
  int value = g_key_file_get_integer(key_file, group, key, &error);
  if (!error)
    return value;

  if (!key_file_error_is_missing(error))
    g_warning("key-file: invalid %s.%s: %s", group, key, error->message);
  return default_value;
}

gboolean wig_key_file_save(GKeyFile *key_file, const char *path, GError **error)
{
  g_return_val_if_fail(key_file != NULL, FALSE);
  g_return_val_if_fail(path != NULL, FALSE);

  gsize length;
  g_autofree char *data = g_key_file_to_data(key_file, &length, error);
  if (!data)
    return FALSE;

  return g_file_set_contents_full(path, data, (gssize)length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}
