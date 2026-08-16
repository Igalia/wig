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

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

/* Favicons are rendered at a single fixed icon size.  The tab's minimum width is
 * derived from it (so a fully collapsed tab is exactly a favicon plus padding),
 * and it is the size the page-icon picker matches against. */
// FIXME: This should be dynamic based upon scale (gtk_widget_get_scale_factor())
// My comment in wig_tab_on_page_icons_changed() may be relevant.
// We should detach this model from the presentation in the view.
#define WIG_TAB_FAVICON_SIZE 16

#define WIG_TYPE_TAB (wig_tab_get_type())
G_DECLARE_FINAL_TYPE(WigTab, wig_tab, WIG, TAB, GObject)

typedef enum {
  WIG_CAPTURE_CAMERA,
  WIG_CAPTURE_MICROPHONE,
  WIG_CAPTURE_DISPLAY,
} WigCaptureKind;

WigTab *wig_tab_new(WebKitWebView *web_view);
guint wig_tab_get_id(WigTab *self);
WebKitWebView *wig_tab_get_web_view(WigTab *self);
GtkWidget *wig_tab_get_widget(WigTab *self);
GIcon *wig_tab_get_icon(WigTab *self);
void wig_tab_set_icon(WigTab *self, GIcon *icon);
const char *wig_tab_get_title(WigTab *self);
const char *wig_tab_get_uri(WigTab *self);
const char *wig_tab_get_page_uri(WigTab *self);
gboolean wig_tab_start_search(WigTab *self);
gboolean wig_tab_get_discarded(WigTab *self);
GtkWidget *wig_tab_get_native_page(WigTab *self);
void wig_tab_mark_discarded(WigTab *self);
void wig_tab_load_discarded(WigTab *self);
gboolean wig_tab_get_pinned(WigTab *self);
void wig_tab_set_pinned(WigTab *self, gboolean pinned);
gboolean wig_tab_get_closing(WigTab *self);
void wig_tab_set_closing(WigTab *self, gboolean closing);
gboolean wig_tab_get_close_pending(WigTab *self);
void wig_tab_set_close_pending(WigTab *self, gboolean pending);
gboolean wig_tab_get_loading(WigTab *self);
gboolean wig_tab_get_playing_audio(WigTab *self);
gboolean wig_tab_get_muted(WigTab *self);
void wig_tab_set_muted(WigTab *self, gboolean muted);
WebKitMediaCaptureState wig_tab_get_capture_state(WigTab *self, WigCaptureKind kind);
void wig_tab_set_capture_state(WigTab *self, WigCaptureKind kind, WebKitMediaCaptureState state);
gboolean wig_tab_get_selected(WigTab *self);
void wig_tab_set_selected(WigTab *self, gboolean selected);
gboolean wig_tab_get_search_active(WigTab *self);
void wig_tab_set_search_active(WigTab *self, gboolean search_active);
guint wig_tab_get_search_match_count(WigTab *self);
void wig_tab_set_search_match_count(WigTab *self, guint match_count);
void wig_tab_set_hovered_link(WigTab *self, const char *uri, const char *page_origin);

G_END_DECLS
