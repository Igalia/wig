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

#include "wig-flatpak.h"

gboolean wig_in_flatpak(void)
{
  static gsize initialized = 0;
  static gboolean in_flatpak = FALSE;

  if (g_once_init_enter(&initialized)) {
    in_flatpak = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
    g_once_init_leave(&initialized, 1);
  }

  return in_flatpak;
}

char *wig_flatpak_dup_id(void)
{
  g_autoptr(GKeyFile) info = g_key_file_new();
  g_autoptr(GError) error = NULL;

  if (!g_key_file_load_from_file(info, "/.flatpak-info", G_KEY_FILE_NONE, &error)) {
    g_warning("flatpak: could not read /.flatpak-info: %s", error->message);
    return NULL;
  }

  char *flatpak_id = g_key_file_get_string(info, "Application", "name", &error);
  if (!flatpak_id)
    g_warning("flatpak: /.flatpak-info has no flatpak id: %s", error->message);
  return flatpak_id;
}
