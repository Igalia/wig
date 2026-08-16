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

#include "wig-app-installer-native.h"

static GVariant *test_manifest_with_name(const char *name)
{
  GVariantBuilder manifest;
  g_variant_builder_init(&manifest, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&manifest, "{sv}", "name", g_variant_new_string(name));
  g_variant_builder_add(&manifest, "{sv}", "description", g_variant_new_string("Desktop integration test"));
  return g_variant_ref_sink(g_variant_builder_end(&manifest));
}

static GVariant *test_manifest(void)
{
  return test_manifest_with_name("Test App");
}

static GBytes *test_png_icon(void)
{
  gsize length;
  guchar *contents = g_base64_decode(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=", &length);
  return g_bytes_new_take(contents, length);
}

typedef struct {
  GMainLoop *loop;
  WigAppInstallerArtifacts *artifacts;
  GError *error;
} InstallResult;

static void install_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
  InstallResult *install_result = user_data;
  install_result->artifacts = wig_app_installer_install_finish(WIG_APP_INSTALLER(source), result,
                                                               &install_result->error);
  g_main_loop_quit(install_result->loop);
}

static WigAppInstallerArtifacts *install(WigAppInstaller *installer, const char *id, GVariant *manifest, GBytes *icon,
                                         const char *icon_content_type, gboolean replace_icon,
                                         const char *current_desktop_file, const char *current_icon_file,
                                         const char *const *protocols, GError **error)
{
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  InstallResult result = { .loop = loop };
  wig_app_installer_install_async(installer, id, "https://example.com/start", manifest, "/usr/bin/wig", icon,
                                  icon_content_type, icon ? 512 : 0, protocols, replace_icon, current_desktop_file,
                                  current_icon_file, NULL, install_finished, &result);
  g_main_loop_run(loop);
  if (!result.artifacts)
    g_propagate_error(error, result.error);
  return result.artifacts;
}

static void test_install_and_uninstall(void)
{
  const char *id = "https://example.com/app/%value";
  const char *protocols[] = { "web+wig", NULL };
  g_autoptr(GVariant) manifest = test_manifest();
  g_autoptr(WigAppInstaller) installer = wig_app_installer_native_new(g_get_user_data_dir());
  g_autoptr(GError) error = NULL;
  g_autoptr(WigAppInstallerArtifacts) artifacts = install(installer, id, manifest, NULL, NULL, TRUE, NULL, NULL,
                                                          protocols, &error);
  g_assert_no_error(error);
  g_assert_nonnull(artifacts);

  g_autofree char *desktop_path = g_build_filename(g_get_user_data_dir(), artifacts->desktop_file, NULL);
  g_autofree char *icon_path = g_build_filename(g_get_user_data_dir(), artifacts->icon_file, NULL);
  g_assert_true(g_file_test(desktop_path, G_FILE_TEST_IS_REGULAR));
  g_assert_true(g_file_test(icon_path, G_FILE_TEST_IS_REGULAR));
  g_autofree char *icon_content_type = NULL;
  g_autoptr(GBytes) cached_icon = wig_app_installer_load_icon(installer, id, artifacts->desktop_file,
                                                              artifacts->icon_file, &icon_content_type, &error);
  g_assert_no_error(error);
  g_assert_nonnull(cached_icon);
  g_assert_cmpstr(icon_content_type, ==, "image/svg+xml");

  g_autoptr(GKeyFile) desktop = g_key_file_new();
  g_assert_true(g_key_file_load_from_file(desktop, desktop_path, G_KEY_FILE_NONE, &error));
  g_assert_no_error(error);
  g_autofree char *name = g_key_file_get_string(desktop, "Desktop Entry", "Name", &error);
  g_assert_no_error(error);
  g_assert_cmpstr(name, ==, "Test App");
  g_autofree char *exec = g_key_file_get_string(desktop, "Desktop Entry", "Exec", &error);
  g_assert_no_error(error);
  g_assert_nonnull(strstr(exec, "--web-app=https://example.com/app/%%value"));
  g_assert_nonnull(strstr(exec, "%U"));
  gsize mime_type_count;
  g_auto(GStrv) mime_types = g_key_file_get_string_list(desktop, "Desktop Entry", "MimeType", &mime_type_count, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(mime_type_count, ==, 1);
  g_assert_cmpstr(mime_types[0], ==, "x-scheme-handler/web+wig");

  g_assert_true(wig_app_installer_uninstall(installer, id, artifacts->desktop_file, artifacts->icon_file, &error));
  g_assert_no_error(error);
  g_assert_false(g_file_test(desktop_path, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(icon_path, G_FILE_TEST_EXISTS));
}

static void test_installs_rasterized_icon(void)
{
  const char *id = "https://example.com/icon-test";
  g_autoptr(GVariant) manifest = test_manifest();
  g_autoptr(GBytes) icon = test_png_icon();
  g_autoptr(WigAppInstaller) installer = wig_app_installer_native_new(g_get_user_data_dir());
  g_autoptr(GError) error = NULL;
  g_autoptr(WigAppInstallerArtifacts) artifacts = install(installer, id, manifest, icon, "image/png", TRUE, NULL, NULL,
                                                          NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(artifacts);
  g_assert_true(g_str_has_suffix(artifacts->icon_file, ".png"));
  g_assert_nonnull(strstr(artifacts->icon_file, "/512x512/"));
  g_autofree char *installed_icon = g_build_filename(g_get_user_data_dir(), artifacts->icon_file, NULL);
  g_assert_true(g_file_test(installed_icon, G_FILE_TEST_IS_REGULAR));
  g_autofree char *icon_content_type = NULL;
  g_autoptr(GBytes) cached_icon = wig_app_installer_load_icon(installer, id, artifacts->desktop_file,
                                                              artifacts->icon_file, &icon_content_type, &error);
  g_assert_no_error(error);
  g_assert_nonnull(cached_icon);
  g_assert_cmpstr(icon_content_type, ==, "image/png");

  g_assert_true(g_file_set_contents(installed_icon, "not a PNG", -1, &error));
  g_assert_no_error(error);
  g_clear_pointer(&cached_icon, g_bytes_unref);
  g_clear_pointer(&icon_content_type, g_free);
  cached_icon = wig_app_installer_load_icon(installer, id, artifacts->desktop_file, artifacts->icon_file,
                                            &icon_content_type, &error);
  g_assert_null(cached_icon);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);

  g_assert_true(wig_app_installer_uninstall(installer, id, artifacts->desktop_file, artifacts->icon_file, &error));
  g_assert_no_error(error);
}

static void test_updates_launcher_without_replacing_icon(void)
{
  const char *id = "https://example.com/launcher-update";
  g_autoptr(GVariant) original_manifest = test_manifest();
  g_autoptr(WigAppInstaller) installer = wig_app_installer_native_new(g_get_user_data_dir());
  g_autoptr(GError) error = NULL;
  g_autoptr(WigAppInstallerArtifacts) artifacts = install(installer, id, original_manifest, NULL, NULL, TRUE, NULL,
                                                          NULL, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(artifacts);

  g_autofree char *desktop_path = g_build_filename(g_get_user_data_dir(), artifacts->desktop_file, NULL);
  g_autofree char *icon_content_type = NULL;
  g_autoptr(GBytes) original_icon = wig_app_installer_load_icon(installer, id, artifacts->desktop_file,
                                                                artifacts->icon_file, &icon_content_type, &error);
  g_assert_no_error(error);

  g_autoptr(GVariant) updated_manifest = test_manifest_with_name("Updated App");
  g_autoptr(WigAppInstallerArtifacts) updated_artifacts = install(
      installer, id, updated_manifest, NULL, NULL, FALSE, artifacts->desktop_file, artifacts->icon_file, NULL, &error);
  g_assert_nonnull(updated_artifacts);
  g_assert_no_error(error);

  g_autoptr(GKeyFile) desktop = g_key_file_new();
  g_assert_true(g_key_file_load_from_file(desktop, desktop_path, G_KEY_FILE_NONE, &error));
  g_assert_no_error(error);
  g_autofree char *name = g_key_file_get_string(desktop, "Desktop Entry", "Name", &error);
  g_assert_no_error(error);
  g_assert_cmpstr(name, ==, "Updated App");

  g_clear_pointer(&icon_content_type, g_free);
  g_autoptr(GBytes) updated_icon = wig_app_installer_load_icon(
      installer, id, updated_artifacts->desktop_file, updated_artifacts->icon_file, &icon_content_type, &error);
  g_assert_no_error(error);
  g_assert_true(g_bytes_equal(original_icon, updated_icon));

  g_assert_true(wig_app_installer_uninstall(installer, id, updated_artifacts->desktop_file,
                                            updated_artifacts->icon_file, &error));
  g_assert_no_error(error);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func("/pwa/desktop-integration/install-uninstall", test_install_and_uninstall);
  g_test_add_func("/pwa/desktop-integration/rasterized-icon", test_installs_rasterized_icon);
  g_test_add_func("/pwa/desktop-integration/update-launcher", test_updates_launcher_without_replacing_icon);
  return g_test_run();
}
