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

#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>
#include <wpe/webkit.h>

#include "wig-downloads-manager.h"
#include "wig-history-store.h"
#include "wig-permissions-manager.h"
#include "wig-session.h"
#include "wig-update-monitor.h"

G_BEGIN_DECLS

#define WIG_TYPE_APPLICATION (wig_application_get_type())
G_DECLARE_FINAL_TYPE(WigApplication, wig_application, WIG, APPLICATION, AdwApplication)

typedef struct {
  char *source;
  WebKitUserScriptInjectionTime injection_time;
  WebKitUserContentInjectedFrames injected_frames;
  WebKitUserScript *script;
} WigUserScriptRecord;

typedef struct {
  char *source;
  WebKitUserStyleLevel level;
  WebKitUserContentInjectedFrames injected_frames;
  WebKitUserStyleSheet *stylesheet;
} WigUserStyleSheetRecord;

WigApplication *wig_application_new(void);
WigApplication *wig_application_get(void);
WPEDisplay *wig_application_get_display(WigApplication *app);
WebKitNetworkSession *wig_application_get_network_session(WigApplication *app);
WebKitWebView *wig_application_create_web_view(WigApplication *app);
WigHistoryStore *wig_application_get_history_store(WigApplication *app);
WigDownloadsManager *wig_application_get_downloads_manager(WigApplication *app);
void wig_application_open_internal_page(WigApplication *app, GtkWindow *win, const char *uri);
gboolean wig_application_open_uri(WigApplication *app, GtkWindow *win, const char *uri);
gboolean wig_application_focus_internal_page(WigApplication *app, const char *uri, WebKitWebView *ignore);

void wig_application_mark_typed_navigation(WigApplication *app, WebKitWebView *web_view, const char *uri);
void wig_application_mark_internal_navigation(WigApplication *app, WebKitWebView *web_view, const char *uri);
gboolean wig_application_take_internal_navigation(WigApplication *app, WebKitWebView *web_view, const char *uri);

GSettings *wig_application_get_settings(WigApplication *app);
WebKitSettings *wig_application_get_web_settings(WigApplication *app);
WebKitUserContentManager *wig_application_get_user_content_manager(WigApplication *app);
GPtrArray *wig_application_get_user_scripts(WigApplication *app);
GPtrArray *wig_application_get_user_style_sheets(WigApplication *app);
WebKitUserContentFilterStore *wig_application_get_content_filter_store(WigApplication *app);
WebKitMemoryPressureSettings *wig_application_get_memory_pressure_settings(WigApplication *app);
WebKitWebsitePolicies *wig_application_create_website_policies(WigApplication *app, const char *uri);
WigSession *wig_application_get_session(WigApplication *app);
gboolean wig_application_is_quitting(WigApplication *app);

void wig_application_track_notification(WigApplication *app, const char *id, WebKitNotification *notif);
void wig_application_untrack_notification(WigApplication *app, const char *id);

WigPermissionsManager *wig_application_get_permissions_manager(WigApplication *app);
WigUpdateMonitor *wig_application_get_update_monitor(WigApplication *app);

G_END_DECLS
