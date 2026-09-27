/* SPDX-License-Identifier: MIT */

/* Runs inside the web process, where things the page cannot observe are
 * visible: console messages the browser itself produces, such as CSP
 * violations, mixed content warnings, and resource load failures.
 *
 * It keeps no state and applies no policy. Each message is forwarded to the
 * view that produced it, and everything else -- buffering, filtering, and
 * whether an MCP client is listening at all -- stays in the UI process where
 * the rest of that logic already lives. Sending to the view rather than to the
 * context is what makes a message attributable to a tab. */

#include <stdio.h>

#include <gmodule.h>
#include <wpe/webkit-web-process-extension.h>

#include "wig-web-process-extension.h"

static const char *source_name(WebKitConsoleMessageSource source)
{
  switch (source) {
  case WEBKIT_CONSOLE_MESSAGE_SOURCE_JAVASCRIPT:
    return "javascript";
  case WEBKIT_CONSOLE_MESSAGE_SOURCE_NETWORK:
    return "network";
  case WEBKIT_CONSOLE_MESSAGE_SOURCE_CONSOLE_API:
    return "console";
  case WEBKIT_CONSOLE_MESSAGE_SOURCE_SECURITY:
    return "security";
  case WEBKIT_CONSOLE_MESSAGE_SOURCE_OTHER:
  default:
    return "other";
  }
}

static const char *level_name(WebKitConsoleMessageLevel level)
{
  switch (level) {
  case WEBKIT_CONSOLE_MESSAGE_LEVEL_INFO:
    return "info";
  case WEBKIT_CONSOLE_MESSAGE_LEVEL_WARNING:
    return "warn";
  case WEBKIT_CONSOLE_MESSAGE_LEVEL_ERROR:
    return "error";
  case WEBKIT_CONSOLE_MESSAGE_LEVEL_DEBUG:
    return "debug";
  case WEBKIT_CONSOLE_MESSAGE_LEVEL_LOG:
  default:
    return "log";
  }
}

/* Append to the file named by WIG_EXTENSION_TRACE, if any. g_debug() from a web
 * process does not reach anywhere useful, and the sandbox has to be off for the
 * write to land, so this is a deliberate last resort rather than normal
 * logging. */
static void trace(const char *format, ...) G_GNUC_PRINTF(1, 2);

static void trace(const char *format, ...)
{
  const char *path = g_getenv("WIG_EXTENSION_TRACE");
  if (!path)
    return;

  va_list args;
  va_start(args, format);
  g_autofree char *text = g_strdup_vprintf(format, args);
  va_end(args);

  FILE *handle = fopen(path, "a");
  if (!handle)
    return;
  fprintf(handle, "%s\n", text);
  fclose(handle);
}

static void on_console_message_sent(WebKitWebPage *page, WebKitConsoleMessage *message, gpointer user_data)
{
  trace("console-message-sent source=%d level=%d text=%s", webkit_console_message_get_source(message),
        webkit_console_message_get_level(message), webkit_console_message_get_text(message));
  /* console.* calls, uncaught errors, and unhandled rejections are already
   * captured in the page, with their arguments and a stack trace. Only the
   * flattened text reaches here, so forwarding those as well would report every
   * one of them twice and less usefully. */
  WebKitConsoleMessageSource source = webkit_console_message_get_source(message);
  if (source == WEBKIT_CONSOLE_MESSAGE_SOURCE_CONSOLE_API || source == WEBKIT_CONSOLE_MESSAGE_SOURCE_JAVASCRIPT)
    return;

  const char *text = webkit_console_message_get_text(message);
  const char *source_id = webkit_console_message_get_source_id(message);
  GVariant *parameters = g_variant_new(WIG_CONSOLE_MESSAGE_FORMAT, source_name(source),
                                       level_name(webkit_console_message_get_level(message)), text ? text : "",
                                       source_id ? source_id : "", webkit_console_message_get_line(message));

  /* Both the message and its parameters are floating and consumed here. */
  webkit_web_page_send_message_to_view(page, webkit_user_message_new(WIG_CONSOLE_MESSAGE_NAME, parameters), NULL, NULL,
                                       NULL);
}

static void on_page_created(WebKitWebProcessExtension *extension, WebKitWebPage *page, gpointer user_data)
{
  trace("page-created id=%" G_GUINT64_FORMAT, webkit_web_page_get_id(page));
  g_signal_connect(page, "console-message-sent", G_CALLBACK(on_console_message_sent), NULL);
}

G_MODULE_EXPORT void webkit_web_process_extension_initialize(WebKitWebProcessExtension *extension);

G_MODULE_EXPORT void webkit_web_process_extension_initialize(WebKitWebProcessExtension *extension)
{
  g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);
}
