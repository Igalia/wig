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

#include "wig-settings-search.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef enum {
  WIG_FEATURES_EXPERIMENTAL,
  WIG_FEATURES_DEVELOPMENT,
} WigFeaturesKind;

/* A pane listing what WebKit can be asked to turn on or off. There are hundreds
 * of them, so the rows are built the first time the pane is looked at rather
 * than when the settings are opened. */
#define WIG_TYPE_SETTINGS_FEATURES (wig_settings_features_get_type())
G_DECLARE_FINAL_TYPE(WigSettingsFeatures, wig_settings_features, WIG, SETTINGS_FEATURES, GtkWidget)

GtkWidget *wig_settings_features_new(WigFeaturesKind kind);

void wig_settings_features_index(WigFeaturesKind kind, WigSettingsSearch *search, const char *pane,
                                 const char *pane_title);

/* Puts back the overrides that were kept for good, at startup. */
void wig_features_apply_overrides(WebKitSettings *web_settings, GSettings *settings);

G_END_DECLS
