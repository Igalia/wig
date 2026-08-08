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
#include <wpe/webkit.h>

#include "wig-permissions.h"

G_BEGIN_DECLS

#define WIG_TYPE_PERMISSION_REQUEST_POPOVER (wig_permission_request_popover_get_type())
G_DECLARE_FINAL_TYPE(WigPermissionRequestPopover, wig_permission_request_popover, WIG, PERMISSION_REQUEST_POPOVER,
                     GtkPopover)

GtkWidget *wig_permission_request_popover_new(void);
void wig_permission_request_popover_prompt(WigPermissionRequestPopover *self, WigPermissions *permissions,
                                           const char *origin, WigPermissionKind kinds,
                                           WebKitPermissionRequest *request);

G_END_DECLS
