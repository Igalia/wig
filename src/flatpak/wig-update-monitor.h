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

G_BEGIN_DECLS

#define WIG_TYPE_UPDATE_MONITOR (wig_update_monitor_get_type())
G_DECLARE_FINAL_TYPE(WigUpdateMonitor, wig_update_monitor, WIG, UPDATE_MONITOR, GObject)

typedef enum {
  WIG_UPDATE_STATE_NONE,
  WIG_UPDATE_STATE_AVAILABLE,
  WIG_UPDATE_STATE_DOWNLOADING,
  WIG_UPDATE_STATE_READY,
  WIG_UPDATE_STATE_BLOCKED,
} WigUpdateState;

#define WIG_TYPE_UPDATE_STATE (wig_update_state_get_type())
GType wig_update_state_get_type(void);

WigUpdateMonitor *wig_update_monitor_new(void);
WigUpdateState wig_update_monitor_get_state(WigUpdateMonitor *self);
void wig_update_monitor_download(WigUpdateMonitor *self);
gboolean wig_update_monitor_spawn_restart_helper(GError **error);

G_END_DECLS
