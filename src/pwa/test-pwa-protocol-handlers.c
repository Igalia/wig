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

#include "wig-pwa-protocol-handlers.h"

#include <json-glib/json-glib.h>

static GVariant *manifest_from_json(const char *json)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) error = NULL;
  g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
  g_assert_no_error(error);
  GVariant *manifest = json_gvariant_deserialize(json_parser_get_root(parser), NULL, &error);
  g_assert_no_error(error);
  return g_variant_ref_sink(manifest);
}

static WigPwaNavigationPolicy *new_policy(void)
{
  g_autoptr(GError) error = NULL;
  WigPwaNavigationPolicy *policy = wig_pwa_navigation_policy_new("https://example.com/app/", NULL, &error);
  g_assert_no_error(error);
  return policy;
}

static void test_resolves_registered_protocol(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":[{\"protocol\":\"web+wig\",\"url\":\"./handle.html?url=%s\"}]}");
  g_autoptr(WigPwaNavigationPolicy) policy = new_policy();
  g_autofree char *resolved = wig_pwa_protocol_handlers_resolve(manifest, "https://example.com/app/manifest.json",
                                                                policy, "web+wig:hello world");
  g_assert_cmpstr(resolved, ==, "https://example.com/app/handle.html?url=web%2Bwig%3Ahello%20world");
}

static void test_ignores_invalid_handlers(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":["
      "{\"protocol\":\"store\",\"url\":\"./handle.html?url=%s\"},"
      "{\"protocol\":\"web+wig\",\"url\":\"https://example.com/outside?url=%s\"},"
      "{\"protocol\":\"web+wig\",\"url\":\"./missing-placeholder\"}"
      "]}");
  g_autoptr(WigPwaNavigationPolicy) policy = new_policy();
  g_assert_null(
      wig_pwa_protocol_handlers_resolve(manifest, "https://example.com/app/manifest.json", policy, "web+wig:value"));
}

static void test_strips_input_credentials(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":[{\"protocol\":\"web+wig\",\"url\":\"./handle.html?url=%s\"}]}");
  g_autoptr(WigPwaNavigationPolicy) policy = new_policy();
  g_autofree char *resolved = wig_pwa_protocol_handlers_resolve(manifest, "https://example.com/app/manifest.json",
                                                                policy, "web+wig://user:secret@host/value");
  g_assert_nonnull(resolved);
  g_assert_null(strstr(resolved, "user"));
  g_assert_null(strstr(resolved, "secret"));
}

static void test_rejects_untrustworthy_handler(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":[{\"protocol\":\"web+wig\",\"url\":\"./handle.html?url=%s\"}]}");
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_navigation_policy_new("http://example.com/app/", NULL, &error);
  g_assert_no_error(error);
  g_assert_null(
      wig_pwa_protocol_handlers_resolve(manifest, "http://example.com/app/manifest.json", policy, "web+wig:value"));
}

static void test_component_encoding(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":[{\"protocol\":\"web+wig\",\"url\":\"./handle.html?url=%s\"}]}");
  g_autoptr(WigPwaNavigationPolicy) policy = new_policy();
  g_autofree char *resolved = wig_pwa_protocol_handlers_resolve(manifest, "https://example.com/app/manifest.json",
                                                                policy, "web+wig:!'()*~");
  g_assert_cmpstr(resolved, ==, "https://example.com/app/handle.html?url=web%2Bwig%3A!'()*~");
}

static void test_rejects_backslash_scope_escape(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":[{\"protocol\":\"web+wig\",\"url\":\"\\\\outside?url=%s\"}]}");
  g_autoptr(WigPwaNavigationPolicy) policy = new_policy();
  g_assert_null(
      wig_pwa_protocol_handlers_resolve(manifest, "https://example.com/app/manifest.json", policy, "web+wig:value"));
}

static void test_serializes_unicode_input(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":[{\"protocol\":\"web+wig\",\"url\":\"./handle.html?url=%s\"}]}");
  g_autoptr(WigPwaNavigationPolicy) policy = new_policy();
  g_autofree char *resolved = wig_pwa_protocol_handlers_resolve(manifest, "https://example.com/app/manifest.json",
                                                                policy, "web+wig:chicken-kïwi");
  g_assert_cmpstr(resolved, ==, "https://example.com/app/handle.html?url=web%2Bwig%3Achicken-k%25C3%25AFwi");
}

static void test_preserves_handler_path_escapes(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":[{\"protocol\":\"web+wig\",\"url\":\"/app/%41/handle?url=%s\"}]}");
  g_autoptr(GError) error = NULL;
  g_autoptr(WigPwaNavigationPolicy) policy = wig_pwa_navigation_policy_new("https://example.com/app/A/", NULL, &error);
  g_assert_no_error(error);
  g_assert_null(
      wig_pwa_protocol_handlers_resolve(manifest, "https://example.com/app/manifest.json", policy, "web+wig:value"));
}

static void test_strips_ftp_credentials(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json(
      "{\"protocol_handlers\":[{\"protocol\":\"ftp\",\"url\":\"./handle.html?url=%s\"}]}");
  g_autoptr(WigPwaNavigationPolicy) policy = new_policy();
  g_autofree char *resolved = wig_pwa_protocol_handlers_resolve(manifest, "https://example.com/app/manifest.json",
                                                                policy, "ftp:\\\\user:secret@example.net/path");
  g_assert_nonnull(resolved);
  g_assert_null(strstr(resolved, "user"));
  g_assert_null(strstr(resolved, "secret"));
}

static void test_lists_valid_schemes(void)
{
  g_autoptr(GVariant) manifest = manifest_from_json("{\"protocol_handlers\":["
                                                    "{\"protocol\":\"web+wig\",\"url\":\"./handle?url=%s\"},"
                                                    "{\"protocol\":\"WEB+WIG\",\"url\":\"./other?url=%s\"},"
                                                    "{\"protocol\":\"invalid\",\"url\":\"./invalid?url=%s\"}"
                                                    "]}");
  g_autoptr(WigPwaNavigationPolicy) policy = new_policy();
  g_auto(GStrv) schemes = wig_pwa_protocol_handlers_get_schemes(manifest, "https://example.com/app/manifest.json",
                                                                policy);
  g_assert_cmpuint(g_strv_length(schemes), ==, 1);
  g_assert_cmpstr(schemes[0], ==, "web+wig");
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/pwa/protocol-handlers/resolve", test_resolves_registered_protocol);
  g_test_add_func("/pwa/protocol-handlers/invalid", test_ignores_invalid_handlers);
  g_test_add_func("/pwa/protocol-handlers/credentials", test_strips_input_credentials);
  g_test_add_func("/pwa/protocol-handlers/untrustworthy", test_rejects_untrustworthy_handler);
  g_test_add_func("/pwa/protocol-handlers/component-encoding", test_component_encoding);
  g_test_add_func("/pwa/protocol-handlers/backslash-scope-escape", test_rejects_backslash_scope_escape);
  g_test_add_func("/pwa/protocol-handlers/unicode-input", test_serializes_unicode_input);
  g_test_add_func("/pwa/protocol-handlers/handler-path-escapes", test_preserves_handler_path_escapes);
  g_test_add_func("/pwa/protocol-handlers/ftp-credentials", test_strips_ftp_credentials);
  g_test_add_func("/pwa/protocol-handlers/list-schemes", test_lists_valid_schemes);
  return g_test_run();
}
