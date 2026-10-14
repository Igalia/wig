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

#include "wig-downloads-manager.h"

#include "wig-application.h"

struct _WigDownloadsManager {
  GObject parent;

  WebKitNetworkSession *session;
  GPtrArray *records;
};

G_DEFINE_FINAL_TYPE(WigDownloadsManager, wig_downloads_manager, G_TYPE_OBJECT)

enum {
  ADDED,
  COMPLETED,
  CHANGED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void wig_download_record_untrack(WigDownloadRecord *record)
{
  g_signal_handlers_disconnect_by_data(record->download, record);
  g_signal_handlers_disconnect_by_data(record->download, record->manager);
  g_clear_object(&record->download);
}

static void wig_download_record_free(WigDownloadRecord *record)
{
  wig_download_record_untrack(record);
  g_free(record->web_app);
  g_free(record);
}

gboolean wig_download_record_belongs_to(WigDownloadRecord *record, const char *web_app)
{
  return g_strcmp0(record->web_app, web_app) == 0;
}

static WigDownloadRecord *wig_downloads_manager_find_record(WigDownloadsManager *self, WebKitDownload *download)
{
  for (guint i = 0; i < self->records->len; i++) {
    WigDownloadRecord *record = g_ptr_array_index(self->records, i);
    if (record->download == download)
      return record;
  }

  return NULL;
}

/* The progress signal fires far too often to difference byte counts across
 * consecutive ones, so a rate is only taken once a sample window has gone by,
 * and is then eased into the running one to keep the figure from jumping about.
 */
#define SPEED_SAMPLE_INTERVAL_US (G_USEC_PER_SEC / 2)
#define SPEED_SMOOTHING 0.3
#define SPEED_STALE_US (G_USEC_PER_SEC * 5)

static void wig_download_record_sample_speed(WigDownloadRecord *record)
{
  gint64 now = g_get_monotonic_time();
  guint64 received = webkit_download_get_received_data_length(record->download);

  if (record->sample_time == 0) {
    record->sample_time = now;
    record->sample_received = received;
    return;
  }

  gint64 elapsed = now - record->sample_time;
  if (elapsed < SPEED_SAMPLE_INTERVAL_US)
    return;

  double sampled = (double)(received - record->sample_received) * G_USEC_PER_SEC / (double)elapsed;
  record->speed = record->speed > 0 ? record->speed + SPEED_SMOOTHING * (sampled - record->speed) : sampled;
  record->sample_time = now;
  record->sample_received = received;
}

/* Returns 0 when there has not been enough of a download yet to say, or data
 * stopped arriving long enough ago that the last rate no longer describes
 * anything. */
double wig_download_record_get_speed(WigDownloadRecord *record)
{
  if (record->state != WIG_DOWNLOAD_ACTIVE || record->sample_time == 0)
    return 0;

  if (g_get_monotonic_time() - record->sample_time > SPEED_STALE_US)
    return 0;

  return record->speed;
}

static void on_download_progress(WebKitDownload *download, GParamSpec *pspec, WigDownloadRecord *record)
{
  wig_download_record_sample_speed(record);
  g_signal_emit(record->manager, signals[CHANGED], 0);
}

static void on_download_finished(WebKitDownload *download, WigDownloadRecord *record)
{
  g_debug("downloads: finished '%s'", webkit_download_get_destination(download));

  /* "failed" runs before "finished", so a cancelled or failed download arrives
   * here with its state already decided. */
  if (record->state == WIG_DOWNLOAD_ACTIVE) {
    record->state = WIG_DOWNLOAD_COMPLETE;
    g_signal_emit(record->manager, signals[COMPLETED], 0, record->web_app);
  }

  g_signal_emit(record->manager, signals[CHANGED], 0);
}

static void on_download_failed(WebKitDownload *download, GError *error, WigDownloadRecord *record)
{
  g_debug("downloads: failed '%s': %s", webkit_download_get_destination(download), error->message);

  if (g_error_matches(error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER))
    record->state = WIG_DOWNLOAD_CANCELLED;
  else
    record->state = WIG_DOWNLOAD_FAILED;
}

static gboolean on_decide_destination(WebKitDownload *download, const char *suggested_filename,
                                      WigDownloadsManager *self)
{
  // FIXME: Show chooser
  if (!suggested_filename || !*suggested_filename) {
    g_warning("downloads: ignoring download with empty filename");
    webkit_download_cancel(download);
    return TRUE;
  }

  const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  g_autofree char *fallback = dir ? NULL : g_build_filename(g_get_home_dir(), "Downloads", NULL);
  g_autofree char *path = g_build_filename(dir ? dir : fallback, suggested_filename, NULL);
  webkit_download_set_destination(download, path);

  g_signal_emit(self, signals[CHANGED], 0);
  return TRUE;
}

/* Hands the record a download to stand for, from the beginning: a restarted one
 * carries none of the progress the attempt it replaces had made. */
static void wig_download_record_track(WigDownloadRecord *record, WebKitDownload *download)
{
  record->download = g_object_ref(download);
  record->state = WIG_DOWNLOAD_ACTIVE;
  record->sample_time = 0;
  record->sample_received = 0;
  record->speed = 0;

  g_signal_connect(download, "decide-destination", G_CALLBACK(on_decide_destination), record->manager);
  g_signal_connect(download, "notify::estimated-progress", G_CALLBACK(on_download_progress), record);
  g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), record);
  g_signal_connect(download, "failed", G_CALLBACK(on_download_failed), record);
}

static void on_download_started(WebKitNetworkSession *session, WebKitDownload *download, WigDownloadsManager *self)
{
  /* A restart has already given its download to the record it replaces, so
   * there is nothing here to add. */
  if (wig_downloads_manager_find_record(self, download))
    return;

  /* Downloads the browser starts for itself, like a web app icon, have no view
   * behind them and are not the user's business. */
  WebKitWebView *web_view = webkit_download_get_web_view(download);
  if (!web_view)
    return;

  WigDownloadRecord *record = g_new0(WigDownloadRecord, 1);
  record->manager = self;
  record->web_app = g_strdup(wig_application_get_web_app_for_web_view(wig_application_get(), web_view));
  wig_download_record_track(record, download);
  g_ptr_array_add(self->records, record);

  g_debug("downloads: started '%s' in %s", webkit_uri_request_get_uri(webkit_download_get_request(download)),
          record->web_app ? record->web_app : "the browser");

  g_signal_emit(self, signals[ADDED], 0, record->web_app);
  g_signal_emit(self, signals[CHANGED], 0);
}

static void wig_downloads_manager_dispose(GObject *object)
{
  WigDownloadsManager *self = WIG_DOWNLOADS_MANAGER(object);

  if (self->session)
    g_signal_handlers_disconnect_by_data(self->session, self);
  g_clear_object(&self->session);
  g_clear_pointer(&self->records, g_ptr_array_unref);

  G_OBJECT_CLASS(wig_downloads_manager_parent_class)->dispose(object);
}

static void wig_downloads_manager_class_init(WigDownloadsManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = wig_downloads_manager_dispose;

  signals[ADDED] = g_signal_new("added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                                1, G_TYPE_STRING);
  signals[COMPLETED] = g_signal_new("completed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void wig_downloads_manager_init(WigDownloadsManager *self)
{
  self->records = g_ptr_array_new_with_free_func((GDestroyNotify)wig_download_record_free);
}

WigDownloadsManager *wig_downloads_manager_new(WebKitNetworkSession *session)
{
  WigDownloadsManager *self = g_object_new(WIG_TYPE_DOWNLOADS_MANAGER, NULL);

  self->session = g_object_ref(session);
  g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), self);

  return self;
}

/* Oldest first; the downloads popover lists them the other way round. */
GPtrArray *wig_downloads_manager_get_records(WigDownloadsManager *self)
{
  return self->records;
}

gboolean wig_downloads_manager_is_empty(WigDownloadsManager *self, const char *web_app)
{
  for (guint i = 0; i < self->records->len; i++) {
    if (wig_download_record_belongs_to(g_ptr_array_index(self->records, i), web_app))
      return FALSE;
  }

  return TRUE;
}

gboolean wig_downloads_manager_has_finished(WigDownloadsManager *self, const char *web_app)
{
  for (guint i = 0; i < self->records->len; i++) {
    WigDownloadRecord *record = g_ptr_array_index(self->records, i);
    if (record->state != WIG_DOWNLOAD_ACTIVE && wig_download_record_belongs_to(record, web_app))
      return TRUE;
  }

  return FALSE;
}

/* Downloads that are already over no longer say anything about how much is left
 * to do, so only the running ones are averaged. */
double wig_downloads_manager_get_progress(WigDownloadsManager *self, const char *web_app)
{
  double total = 0;
  guint active = 0;

  for (guint i = 0; i < self->records->len; i++) {
    WigDownloadRecord *record = g_ptr_array_index(self->records, i);
    if (record->state != WIG_DOWNLOAD_ACTIVE || !wig_download_record_belongs_to(record, web_app))
      continue;

    total += webkit_download_get_estimated_progress(record->download);
    active++;
  }

  return active > 0 ? total / active : 0;
}

/* The record keeps its place in the list and starts over from nothing, rather
 * than a second one appearing above the attempt it replaces. */
void wig_downloads_manager_retry(WigDownloadsManager *self, WigDownloadRecord *record)
{
  g_autofree char *uri = g_strdup(webkit_uri_request_get_uri(webkit_download_get_request(record->download)));
  g_debug("downloads: restarting '%s'", uri);

  /* "download-started" only arrives once this has returned, by which point the
   * record owns the new download and that signal knows to leave it alone. */
  g_autoptr(WebKitDownload) download = webkit_network_session_download_uri(self->session, uri);

  wig_download_record_untrack(record);
  wig_download_record_track(record, download);

  g_signal_emit(self, signals[CHANGED], 0);
}

/* The file the download left behind is not touched. */
void wig_downloads_manager_remove(WigDownloadsManager *self, WigDownloadRecord *record)
{
  g_debug("downloads: removing '%s'", webkit_download_get_destination(record->download));

  if (g_ptr_array_remove(self->records, record))
    g_signal_emit(self, signals[CHANGED], 0);
}

void wig_downloads_manager_clear_finished(WigDownloadsManager *self, const char *web_app)
{
  g_debug("downloads: clearing finished from %s", web_app ? web_app : "the browser");

  for (gint i = (gint)self->records->len - 1; i >= 0; i--) {
    WigDownloadRecord *record = g_ptr_array_index(self->records, (guint)i);
    if (record->state != WIG_DOWNLOAD_ACTIVE && wig_download_record_belongs_to(record, web_app))
      g_ptr_array_remove_index(self->records, (guint)i);
  }

  g_signal_emit(self, signals[CHANGED], 0);
}
