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

#include "wig-download-row.h"

#include "wig-download-context-menu.h"

#define ICON_SIZE 48
#define FALLBACK_ICON_NAME "text-x-generic"

struct _WigDownloadRow {
  GtkWidget parent;

  /* Owned by the manager, which only drops records through a "changed" that
   * makes the downloads button drop this row in the same turn. */
  WigDownloadRecord *record;

  GtkWidget *box;
  GtkWidget *icon;
  char *icon_filename;
  GtkWidget *filename_label;
  GtkWidget *cancel_button;
  GtkWidget *retry_button;
  GtkWidget *folder_button;
  GtkWidget *detail_label;
  GtkWidget *progress_bar;
  GtkWidget *context_menu;
  GSimpleActionGroup *actions;
};

G_DEFINE_FINAL_TYPE(WigDownloadRow, wig_download_row, GTK_TYPE_WIDGET)

enum {
  ERROR_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static const char *download_status_name(WigDownloadState state)
{
  switch (state) {
  case WIG_DOWNLOAD_COMPLETE:
    return "complete";
  case WIG_DOWNLOAD_FAILED:
    return "failed";
  case WIG_DOWNLOAD_CANCELLED:
    return "cancelled";
  case WIG_DOWNLOAD_ACTIVE:
    return "downloading";
  }

  g_assert_not_reached();
  return NULL;
}

static const char *download_status_text(WigDownloadState state)
{
  switch (state) {
  case WIG_DOWNLOAD_COMPLETE:
    return "Completed";
  case WIG_DOWNLOAD_FAILED:
    return "Failed";
  case WIG_DOWNLOAD_CANCELLED:
    return "Canceled";
  case WIG_DOWNLOAD_ACTIVE:
    break;
  }

  g_assert_not_reached();
  return NULL;
}

static void wig_download_row_cancel_clicked(WigDownloadRow *self)
{
  if (self->record->state != WIG_DOWNLOAD_ACTIVE)
    return;

  g_debug("downloads: cancelling '%s'", webkit_download_get_destination(self->record->download));
  webkit_download_cancel(self->record->download);
}

static void wig_download_row_retry_clicked(WigDownloadRow *self)
{
  wig_downloads_manager_retry(self->record->manager, self->record);
}

static GFile *wig_download_row_file(WigDownloadRow *self)
{
  const char *destination = webkit_download_get_destination(self->record->download);

  return destination ? g_file_new_for_path(destination) : NULL;
}

static GtkWindow *wig_download_row_window(WigDownloadRow *self)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));

  return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

static void wig_download_row_trash(WigDownloadRow *self)
{
  g_autoptr(GFile) file = wig_download_row_file(self);
  if (!file)
    return;

  g_autoptr(GError) error = NULL;
  if (!g_file_trash(file, NULL, &error)) {
    g_warning("downloads: trash '%s': %s", g_file_peek_path(file), error->message);
    g_signal_emit(self, signals[ERROR_SIGNAL], 0, error->message);
  } else {
    g_debug("downloads: trashed '%s'", g_file_peek_path(file));
  }

  wig_download_row_update(self);
}

static void wig_download_row_launched(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigDownloadRow) self = WIG_DOWNLOAD_ROW(user_data);
  g_autoptr(GError) error = NULL;

  if (gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, &error))
    return;

  if (g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    return;

  g_warning("downloads: open file: %s", error->message);
  g_signal_emit(self, signals[ERROR_SIGNAL], 0, error->message);
}

static void wig_download_row_folder_opened(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigDownloadRow) self = WIG_DOWNLOAD_ROW(user_data);
  g_autoptr(GError) error = NULL;

  if (gtk_file_launcher_open_containing_folder_finish(GTK_FILE_LAUNCHER(source), result, &error))
    return;

  if (g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    return;

  g_warning("downloads: open containing folder: %s", error->message);
  g_signal_emit(self, signals[ERROR_SIGNAL], 0, error->message);
}

static void wig_download_row_open_file_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigDownloadRow *self = WIG_DOWNLOAD_ROW(user_data);
  g_autoptr(GFile) file = wig_download_row_file(self);
  if (!file)
    return;

  g_debug("downloads: opening '%s'", g_file_peek_path(file));
  g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
  gtk_file_launcher_launch(launcher, wig_download_row_window(self), NULL, wig_download_row_launched,
                           g_object_ref(self));
}

static void wig_download_row_open_folder(WigDownloadRow *self)
{
  g_autoptr(GFile) file = wig_download_row_file(self);
  if (!file)
    return;

  g_debug("downloads: revealing '%s'", g_file_peek_path(file));
  g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
  gtk_file_launcher_open_containing_folder(launcher, wig_download_row_window(self), NULL,
                                           wig_download_row_folder_opened, g_object_ref(self));
}

static void wig_download_row_open_folder_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_download_row_open_folder(WIG_DOWNLOAD_ROW(user_data));
}

static void wig_download_row_copy_link_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigDownloadRow *self = WIG_DOWNLOAD_ROW(user_data);
  const char *uri = webkit_uri_request_get_uri(webkit_download_get_request(self->record->download));

  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), uri);
}

static gboolean wig_download_row_remove_in_idle(gpointer user_data)
{
  g_autoptr(WigDownloadRow) self = WIG_DOWNLOAD_ROW(user_data);

  wig_downloads_manager_remove(self->record->manager, self->record);

  return G_SOURCE_REMOVE;
}

static void wig_download_row_remove_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  /* Dropping the record takes this row down with it, which must not happen
   * while the menu item that asked for it is still being activated. */
  g_idle_add(wig_download_row_remove_in_idle, g_object_ref(user_data));
}

static void wig_download_row_trash_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_download_row_trash(WIG_DOWNLOAD_ROW(user_data));
}

static void wig_download_row_delete_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigDownloadRow *self = WIG_DOWNLOAD_ROW(user_data);
  g_autoptr(GFile) file = wig_download_row_file(self);
  if (!file)
    return;

  g_autoptr(GError) error = NULL;
  if (!g_file_delete(file, NULL, &error)) {
    g_warning("downloads: delete '%s': %s", g_file_peek_path(file), error->message);
    g_signal_emit(self, signals[ERROR_SIGNAL], 0, error->message);
  } else {
    g_debug("downloads: deleted '%s'", g_file_peek_path(file));
  }

  wig_download_row_update(self);
}

static const GActionEntry download_row_actions[] = {
  { "open-file", wig_download_row_open_file_action }, { "open-folder", wig_download_row_open_folder_action },
  { "copy-link", wig_download_row_copy_link_action }, { "remove", wig_download_row_remove_action },
  { "trash", wig_download_row_trash_action },         { "delete", wig_download_row_delete_action },
};

static void wig_download_row_set_action_enabled(WigDownloadRow *self, const char *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self->actions), name);

  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

/* A download still running has no file to speak of yet: WebKit writes it out
 * under a temporary name and only moves it into place at the end. */
static void wig_download_row_secondary_pressed(WigDownloadRow *self, int n_press, double x, double y)
{
  const char *destination = webkit_download_get_destination(self->record->download);
  gboolean active = self->record->state == WIG_DOWNLOAD_ACTIVE;
  gboolean has_file = !active && destination && g_file_test(destination, G_FILE_TEST_EXISTS);

  wig_download_row_set_action_enabled(self, "open-file", has_file);
  wig_download_row_set_action_enabled(self, "open-folder", has_file);
  wig_download_row_set_action_enabled(self, "remove", !active);
  wig_download_row_set_action_enabled(self, "trash", has_file);
  wig_download_row_set_action_enabled(self, "delete", has_file);

  g_clear_pointer(&self->context_menu, gtk_widget_unparent);
  self->context_menu = wig_download_context_menu_new();
  gtk_widget_set_parent(self->context_menu, GTK_WIDGET(self));

  gtk_popover_set_pointing_to(GTK_POPOVER(self->context_menu), &(const GdkRectangle) { (int)x, (int)y, 1, 1 });
  gtk_popover_popup(GTK_POPOVER(self->context_menu));
}

static void wig_download_row_dispose(GObject *object)
{
  WigDownloadRow *self = WIG_DOWNLOAD_ROW(object);

  g_clear_pointer(&self->context_menu, gtk_widget_unparent);
  g_clear_pointer(&self->box, gtk_widget_unparent);
  g_clear_object(&self->actions);

  G_OBJECT_CLASS(wig_download_row_parent_class)->dispose(object);
}

static void wig_download_row_finalize(GObject *object)
{
  WigDownloadRow *self = WIG_DOWNLOAD_ROW(object);

  g_free(self->icon_filename);

  G_OBJECT_CLASS(wig_download_row_parent_class)->finalize(object);
}

static void wig_download_row_class_init(WigDownloadRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_download_row_dispose;
  object_class->finalize = wig_download_row_finalize;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-download-row");

  signals[ERROR_SIGNAL] = g_signal_new("error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 1, G_TYPE_STRING);
}

static GtkWidget *download_row_detail_label(const char *css_class)
{
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_add_css_class(label, css_class);

  return label;
}

static void wig_download_row_init(WigDownloadRow *self)
{
  self->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_parent(self->box, GTK_WIDGET(self));

  self->icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(self->icon), ICON_SIZE);
  gtk_widget_set_valign(self->icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(self->icon, "download-icon");
  gtk_box_append(GTK_BOX(self->box), self->icon);

  GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(details, TRUE);
  gtk_box_append(GTK_BOX(self->box), details);

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  self->filename_label = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(self->filename_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_xalign(GTK_LABEL(self->filename_label), 0.0f);
  gtk_widget_set_hexpand(self->filename_label, TRUE);
  gtk_widget_add_css_class(self->filename_label, "download-name");
  gtk_box_append(GTK_BOX(header), self->filename_label);

  self->cancel_button = gtk_button_new_from_icon_name("cross-large-symbolic");
  gtk_widget_set_tooltip_text(self->cancel_button, "Cancel");
  gtk_widget_add_css_class(self->cancel_button, "flat");
  gtk_widget_add_css_class(self->cancel_button, "destructive-action");
  g_signal_connect_swapped(self->cancel_button, "clicked", G_CALLBACK(wig_download_row_cancel_clicked), self);
  gtk_box_append(GTK_BOX(header), self->cancel_button);

  self->retry_button = gtk_button_new_from_icon_name("arrow-circular-top-right-symbolic");
  gtk_widget_set_tooltip_text(self->retry_button, "Restart Download");
  gtk_widget_add_css_class(self->retry_button, "flat");
  g_signal_connect_swapped(self->retry_button, "clicked", G_CALLBACK(wig_download_row_retry_clicked), self);
  gtk_box_append(GTK_BOX(header), self->retry_button);

  self->folder_button = gtk_button_new_from_icon_name("folder-open-symbolic");
  gtk_widget_set_tooltip_text(self->folder_button, "Open Containing Folder");
  gtk_widget_add_css_class(self->folder_button, "flat");
  g_signal_connect_swapped(self->folder_button, "clicked", G_CALLBACK(wig_download_row_open_folder), self);
  gtk_box_append(GTK_BOX(header), self->folder_button);

  gtk_box_append(GTK_BOX(details), header);

  self->detail_label = download_row_detail_label("download-detail");
  gtk_box_append(GTK_BOX(details), self->detail_label);

  self->progress_bar = gtk_progress_bar_new();
  gtk_box_append(GTK_BOX(details), self->progress_bar);

  self->actions = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(self->actions), download_row_actions, G_N_ELEMENTS(download_row_actions),
                                  self);
  gtk_widget_insert_action_group(GTK_WIDGET(self), "download", G_ACTION_GROUP(self->actions));

  GtkGesture *secondary = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
  g_signal_connect_swapped(secondary, "pressed", G_CALLBACK(wig_download_row_secondary_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(secondary));
}

GtkWidget *wig_download_row_new(WigDownloadRecord *record)
{
  g_assert(record != NULL);

  WigDownloadRow *self = WIG_DOWNLOAD_ROW(g_object_new(WIG_TYPE_DOWNLOAD_ROW, NULL));
  self->record = record;
  wig_download_row_update(self);

  return GTK_WIDGET(self);
}

WigDownloadRecord *wig_download_row_get_record(WigDownloadRow *self)
{
  g_assert(WIG_IS_DOWNLOAD_ROW(self));

  return self->record;
}

/* The name a download is known by before it has been given a destination is the
 * last path segment of its URI, or the whole URI when it has no segments. */
static char *download_uri_filename(const char *uri)
{
  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  const char *path = parsed ? g_uri_get_path(parsed) : NULL;
  const char *slash = path ? strrchr(path, '/') : NULL;

  return g_strdup(slash && slash[1] ? slash + 1 : uri);
}

static char *download_icon_name(WebKitDownload *download, const char *filename)
{
  g_autofree char *content_type = g_content_type_guess(filename, NULL, 0, NULL);

  if (!content_type || g_content_type_is_unknown(content_type)) {
    WebKitURIResponse *response = webkit_download_get_response(download);
    const char *mime_type = response ? webkit_uri_response_get_mime_type(response) : NULL;
    g_autofree char *from_mime = mime_type && *mime_type ? g_content_type_from_mime_type(mime_type) : NULL;
    if (from_mime && !g_content_type_is_unknown(from_mime)) {
      g_free(content_type);
      content_type = g_steal_pointer(&from_mime);
    }
  }

  return content_type ? g_content_type_get_generic_icon_name(content_type) : NULL;
}

/* Rounded to whatever unit the figure is big enough for: a download with hours
 * to go says nothing useful by counting out the seconds. */
static char *format_time_remaining(double seconds)
{
  if (seconds < 60)
    return g_strdup_printf("%ds left", MAX((int)seconds, 1));

  if (seconds < 60 * 60)
    return g_strdup_printf("%dm left", (int)(seconds / 60 + 0.5));

  if (seconds < 60 * 60 * 24)
    return g_strdup_printf("%dh left", (int)(seconds / (60 * 60) + 0.5));

  return g_strdup("over a day left");
}

/* While running this reads as "1m left - 4.2 MB of 8.0 MB (2.1 MB/s)",
 * dropping whichever parts are not known: the size served without a length, and
 * both the rate and what it implies about the time left until one has been
 * measured.  Once the download is over it is how it ended instead. */
static char *download_detail_text(WigDownloadRecord *record, guint64 received, guint64 total)
{
  if (record->state != WIG_DOWNLOAD_ACTIVE) {
    const char *status = download_status_text(record->state);

    if (record->state != WIG_DOWNLOAD_COMPLETE)
      return g_strdup(status);

    g_autofree char *size = g_format_size_full(received, G_FORMAT_SIZE_DEFAULT);
    return g_strdup_printf("%s - %s", status, size);
  }

  g_autoptr(GString) detail = g_string_new(NULL);
  double speed = wig_download_record_get_speed(record);

  if (speed > 0 && total > received) {
    g_autofree char *remaining = format_time_remaining((double)(total - received) / speed);
    g_string_append_printf(detail, "%s - ", remaining);
  }

  g_autofree char *received_str = g_format_size_full(received, G_FORMAT_SIZE_DEFAULT);
  if (total > 0) {
    g_autofree char *total_str = g_format_size_full(total, G_FORMAT_SIZE_DEFAULT);
    g_string_append_printf(detail, "%s of %s", received_str, total_str);
  } else {
    g_string_append(detail, received_str);
  }

  if (speed > 0) {
    g_autofree char *speed_str = g_format_size_full((guint64)speed, G_FORMAT_SIZE_DEFAULT);
    g_string_append_printf(detail, " (%s/s)", speed_str);
  }

  return g_string_free(g_steal_pointer(&detail), FALSE);
}

void wig_download_row_update(WigDownloadRow *self)
{
  g_assert(WIG_IS_DOWNLOAD_ROW(self));

  WebKitDownload *download = self->record->download;
  const char *uri = webkit_uri_request_get_uri(webkit_download_get_request(download));
  const char *destination = webkit_download_get_destination(download);
  gboolean active = self->record->state == WIG_DOWNLOAD_ACTIVE;

  g_autofree char *filename = destination ? g_path_get_basename(destination) : download_uri_filename(uri);
  gtk_label_set_text(GTK_LABEL(self->filename_label), filename);
  gtk_widget_set_tooltip_text(self->filename_label, filename);

  if (g_strcmp0(self->icon_filename, filename) != 0) {
    g_set_str(&self->icon_filename, filename);
    g_autofree char *icon_name = download_icon_name(download, filename);
    gtk_image_set_from_icon_name(GTK_IMAGE(self->icon), icon_name ? icon_name : FALLBACK_ICON_NAME);
  }

  guint64 received = webkit_download_get_received_data_length(download);
  WebKitURIResponse *response = webkit_download_get_response(download);
  guint64 total = response ? webkit_uri_response_get_content_length(response) : 0;
  double progress = webkit_download_get_estimated_progress(download);

  g_autofree char *detail = download_detail_text(self->record, received, total);
  gtk_label_set_text(GTK_LABEL(self->detail_label), detail);

  const char *status = download_status_name(self->record->state);
  for (WigDownloadState state = WIG_DOWNLOAD_ACTIVE; state <= WIG_DOWNLOAD_CANCELLED; state++)
    gtk_widget_remove_css_class(self->detail_label, download_status_name(state));
  gtk_widget_add_css_class(self->detail_label, status);

  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), progress);
  gtk_widget_set_visible(self->progress_bar, active);

  gtk_widget_set_visible(self->cancel_button, active);
  gtk_widget_set_visible(self->retry_button,
                         self->record->state == WIG_DOWNLOAD_FAILED || self->record->state == WIG_DOWNLOAD_CANCELLED);

  gtk_widget_set_visible(self->folder_button, self->record->state == WIG_DOWNLOAD_COMPLETE);
  gtk_widget_set_sensitive(self->folder_button, destination && g_file_test(destination, G_FILE_TEST_EXISTS));
}
