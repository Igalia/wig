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

#include <glib-object.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

#define WIG_TYPE_DOWNLOADS_MANAGER (wig_downloads_manager_get_type())
G_DECLARE_FINAL_TYPE(WigDownloadsManager, wig_downloads_manager, WIG, DOWNLOADS_MANAGER, GObject)

typedef enum {
  WIG_DOWNLOAD_ACTIVE,
  WIG_DOWNLOAD_COMPLETE,
  WIG_DOWNLOAD_FAILED,
  WIG_DOWNLOAD_CANCELLED,
} WigDownloadState;

typedef struct {
  WigDownloadsManager *manager;
  WebKitDownload *download;
  WigDownloadState state;

  gint64 sample_time;
  guint64 sample_received;
  double speed;
} WigDownloadRecord;

double wig_download_record_get_speed(WigDownloadRecord *record);

WigDownloadsManager *wig_downloads_manager_new(WebKitNetworkSession *session);

GPtrArray *wig_downloads_manager_get_records(WigDownloadsManager *self);
gboolean wig_downloads_manager_is_empty(WigDownloadsManager *self);
gboolean wig_downloads_manager_has_finished(WigDownloadsManager *self);
double wig_downloads_manager_get_progress(WigDownloadsManager *self);
void wig_downloads_manager_clear_finished(WigDownloadsManager *self);

void wig_downloads_manager_retry(WigDownloadsManager *self, WigDownloadRecord *record);
void wig_downloads_manager_remove(WigDownloadsManager *self, WigDownloadRecord *record);

G_END_DECLS
