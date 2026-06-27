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

#include "wig-permissions.h"

#include <adwaita.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

#define WIG_TYPE_PERMISSIONS_BUTTON (wig_permissions_button_get_type())
G_DECLARE_FINAL_TYPE(WigPermissionsButton, wig_permissions_button, WIG, PERMISSIONS_BUTTON, GtkWidget)

GtkWidget *wig_permissions_button_new(void);
void wig_permissions_button_set_permissions(WigPermissionsButton *self, WigPermissions *permissions);
void wig_permissions_button_prompt(WigPermissionsButton *self, WigPermissions *permissions, WigPermissionKind kind,
                                   WebKitPermissionRequest *request);

G_END_DECLS
