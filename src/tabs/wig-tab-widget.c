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

#include "wig-tab-widget.h"

#include "wig-tab-context-menu.h"
#include "wig-tab-sidebar.h"

struct _WigTabWidget {
  GtkWidget parent;

  WigTab *tab;

  GtkWidget *overlay;
  GtkWidget *content;
  GtkWidget *favicon;
  GtkWidget *spinner;
  GtkWidget *audio_icon;
  GtkWidget *capture_icons[WIG_CAPTURE_DISPLAY + 1];
  GtkWidget *title_label;
  GtkWidget *close_fade;
  GtkWidget *context_menu_popover;

  GtkWidget *snapshot_popover;
  GCancellable *snapshot_cancellable;
  guint snapshot_timeout_id;
};

G_DEFINE_FINAL_TYPE(WigTabWidget, wig_tab_widget, GTK_TYPE_WIDGET)

enum { SIGNAL_CLOSE_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void wig_tab_widget_on_icon_changed(WigTabWidget *self, GParamSpec *pspec, WigTab *tab)
{
  if (self->spinner)
    return;

  /* A pinned tab is nothing but its icon, so it needs one even when the site
   * offers none. */
  g_autoptr(GIcon) fallback = NULL;
  GIcon *icon = wig_tab_get_icon(tab);
  if (!icon && wig_tab_get_pinned(tab)) {
    fallback = g_themed_icon_new("web-browser-symbolic");
    icon = fallback;
  }

  if (icon) {
    if (!self->favicon) {
      self->favicon = gtk_image_new();
      gtk_image_set_pixel_size(GTK_IMAGE(self->favicon), WIG_TAB_FAVICON_SIZE);
      gtk_widget_set_halign(self->favicon, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand(self->favicon, FALSE);
      gtk_widget_add_css_class(self->favicon, "tab-favicon");
      gtk_widget_insert_before(self->favicon, self->content, self->title_label);
    }
    gtk_image_set_from_gicon(GTK_IMAGE(self->favicon), icon);
  } else {
    g_clear_pointer(&self->favicon, gtk_widget_unparent);
  }
}

static void wig_tab_widget_on_loading_changed(WigTabWidget *self, GParamSpec *pspec, WigTab *tab)
{
  gboolean loading = wig_tab_get_loading(tab);

  if (loading) {
    g_clear_pointer(&self->favicon, gtk_widget_unparent);
    if (!self->spinner) {
      self->spinner = gtk_spinner_new();
      gtk_widget_set_size_request(self->spinner, WIG_TAB_FAVICON_SIZE, WIG_TAB_FAVICON_SIZE);
      gtk_widget_set_halign(self->spinner, GTK_ALIGN_START);
      gtk_widget_add_css_class(self->spinner, "tab-favicon");
      gtk_widget_insert_before(self->spinner, self->content, self->title_label);
    }
    gtk_spinner_set_spinning(GTK_SPINNER(self->spinner), TRUE);
  } else {
    if (self->spinner) {
      gtk_spinner_set_spinning(GTK_SPINNER(self->spinner), FALSE);
      g_clear_pointer(&self->spinner, gtk_widget_unparent);
    }
    wig_tab_widget_on_icon_changed(self, NULL, self->tab);
  }
}

static void wig_tab_widget_audio_icon_clicked(GtkGestureClick *gesture, int n_press, double x, double y,
                                              WigTabWidget *self)
{
  wig_tab_set_muted(self->tab, !wig_tab_get_muted(self->tab));
}

static void wig_tab_widget_on_muted_changed(WigTabWidget *self, GParamSpec *pspec, WigTab *tab)
{
  if (!self->audio_icon)
    return;
  gtk_image_set_from_icon_name(GTK_IMAGE(self->audio_icon),
                               wig_tab_get_muted(tab) ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
}

static void wig_tab_widget_on_playing_audio_changed(WigTabWidget *self, GParamSpec *pspec, WigTab *tab)
{
  if (wig_tab_get_playing_audio(tab)) {
    if (!self->audio_icon) {
      self->audio_icon = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
      gtk_image_set_pixel_size(GTK_IMAGE(self->audio_icon), WIG_TAB_FAVICON_SIZE);
      gtk_widget_set_halign(self->audio_icon, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand(self->audio_icon, FALSE);
      gtk_widget_set_cursor(self->audio_icon, gdk_cursor_new_from_name("pointer", NULL));
      gtk_widget_add_css_class(self->audio_icon, "tab-audio");
      gtk_widget_insert_before(self->audio_icon, self->content, self->title_label);

      GtkGestureClick *gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
      g_signal_connect_object(gesture, "pressed", G_CALLBACK(wig_tab_widget_audio_icon_clicked), self,
                              G_CONNECT_DEFAULT);
      gtk_widget_add_controller(self->audio_icon, GTK_EVENT_CONTROLLER(gesture));
    }
    gtk_widget_set_visible(self->audio_icon, TRUE);
    wig_tab_widget_on_muted_changed(self, NULL, tab);
  } else {
    g_clear_pointer(&self->audio_icon, gtk_widget_unparent);
  }
}

static const struct {
  const char *icon_name;
  const char *muted_icon_name;
  const char *style_class;
  const char *tooltip;
  const char *muted_tooltip;
} capture_kinds[WIG_CAPTURE_DISPLAY + 1] = {
  [WIG_CAPTURE_CAMERA] = { "film-camera-symbolic", "film-camera-disabled-symbolic", "tab-capture-camera",
                           "Using the camera, click to pause", "Camera paused, click to resume" },
  [WIG_CAPTURE_MICROPHONE] = { "audio-input-microphone-symbolic", "microphone-disabled-symbolic",
                               "tab-capture-microphone", "Using the microphone, click to pause",
                               "Microphone paused, click to resume" },
  [WIG_CAPTURE_DISPLAY] = { "screen-shared-symbolic", "screen-shared-symbolic", "tab-capture-display",
                            "Sharing the screen, click to pause", "Screen sharing paused, click to resume" },
};

/* Pausing keeps the device, so the page can be let back in with another click;
 * giving it up entirely is not offered here since it cannot be undone. */
static void wig_tab_widget_capture_icon_clicked(GtkGestureClick *gesture, int n_press, double x, double y,
                                                WigTabWidget *self)
{
  GtkWidget *icon = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

  for (WigCaptureKind kind = WIG_CAPTURE_CAMERA; kind <= WIG_CAPTURE_DISPLAY; kind++) {
    if (self->capture_icons[kind] != icon)
      continue;

    gboolean muted = wig_tab_get_capture_state(self->tab, kind) == WEBKIT_MEDIA_CAPTURE_STATE_MUTED;
    wig_tab_set_capture_state(self->tab, kind,
                              muted ? WEBKIT_MEDIA_CAPTURE_STATE_ACTIVE : WEBKIT_MEDIA_CAPTURE_STATE_MUTED);
    return;
  }
}

static void wig_tab_widget_on_capture_changed(WigTabWidget *self, WigTab *tab)
{
  for (WigCaptureKind kind = WIG_CAPTURE_CAMERA; kind <= WIG_CAPTURE_DISPLAY; kind++) {
    WebKitMediaCaptureState state = wig_tab_get_capture_state(tab, kind);

    if (state == WEBKIT_MEDIA_CAPTURE_STATE_NONE) {
      g_clear_pointer(&self->capture_icons[kind], gtk_widget_unparent);
      continue;
    }

    gboolean muted = state == WEBKIT_MEDIA_CAPTURE_STATE_MUTED;
    if (!self->capture_icons[kind]) {
      GtkWidget *icon = gtk_image_new();
      gtk_image_set_pixel_size(GTK_IMAGE(icon), WIG_TAB_FAVICON_SIZE);
      gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand(icon, FALSE);
      gtk_widget_set_cursor(icon, gdk_cursor_new_from_name("pointer", NULL));
      gtk_widget_add_css_class(icon, "tab-capture");
      gtk_widget_add_css_class(icon, capture_kinds[kind].style_class);
      gtk_widget_insert_before(icon, self->content, self->title_label);

      GtkGestureClick *gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
      g_signal_connect_object(gesture, "pressed", G_CALLBACK(wig_tab_widget_capture_icon_clicked), self,
                              G_CONNECT_DEFAULT);
      gtk_widget_add_controller(icon, GTK_EVENT_CONTROLLER(gesture));

      self->capture_icons[kind] = icon;
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(self->capture_icons[kind]),
                                 muted ? capture_kinds[kind].muted_icon_name : capture_kinds[kind].icon_name);
    gtk_widget_set_tooltip_text(self->capture_icons[kind],
                                muted ? capture_kinds[kind].muted_tooltip : capture_kinds[kind].tooltip);
    if (muted)
      gtk_widget_add_css_class(self->capture_icons[kind], "muted");
    else
      gtk_widget_remove_css_class(self->capture_icons[kind], "muted");
  }
}

static void wig_tab_widget_on_pinned_changed(WigTabWidget *self, GParamSpec *pspec, WigTab *tab)
{
  gboolean pinned = wig_tab_get_pinned(tab);

  /* A pinned tab is reduced to its favicon, and cannot be closed by a stray
   * click where its close button would have been. */
  gtk_widget_set_visible(self->title_label, !pinned);
  gtk_widget_set_visible(self->close_fade, !pinned);

  if (pinned)
    gtk_widget_add_css_class(GTK_WIDGET(self), "pinned");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self), "pinned");

  wig_tab_widget_on_icon_changed(self, NULL, tab);
}

static void wig_tab_widget_snapshot_popover_closed(GtkPopover *popover, WigTabWidget *self)
{
  g_clear_pointer(&self->snapshot_popover, gtk_widget_unparent);
}

#define SNAPSHOT_DISPLAY_WIDTH 340

/*
 * wig_tab_widget_scale_image:
 * @image: the source #WebKitImage (GDK_MEMORY_B8G8R8A8_PREMULTIPLIED)
 * @target_width: desired output width in pixels
 *
 * Downscales @image to @target_width pixels wide, preserving the aspect ratio,
 * using a box filter: each output pixel is the arithmetic mean of all source
 * pixels whose area maps onto it.
 *
 * Returns: (transfer full): a new #GdkTexture sized @target_width × height
 */
static GdkTexture *wig_tab_widget_scale_image(WebKitImage *image, int target_width)
{
  int src_w = webkit_image_get_width(image);
  int src_h = webkit_image_get_height(image);
  int target_h = src_w > 0 ? (target_width * src_h / src_w) : target_width;
  gsize src_stride = (gsize)webkit_image_get_stride(image);
  gsize dst_stride = (gsize)target_width * 4;

  gsize src_size;
  const guint8 *src = g_bytes_get_data(webkit_image_as_bytes(image), &src_size);
  guint8 *dst = g_new(guint8, dst_stride * (gsize)target_h);

  for (int dy = 0; dy < target_h; dy++) {
    /* Source row range [sy0, sy1) that maps to destination row dy */
    int sy0 = dy * src_h / target_h;
    int sy1 = (dy + 1) * src_h / target_h;
    if (sy1 <= sy0)
      sy1 = sy0 + 1;

    for (int dx = 0; dx < target_width; dx++) {
      int sx0 = dx * src_w / target_width;
      int sx1 = (dx + 1) * src_w / target_width;
      if (sx1 <= sx0)
        sx1 = sx0 + 1;

      guint32 r = 0, g = 0, b = 0, a = 0;
      int count = (sy1 - sy0) * (sx1 - sx0);
      for (int sy = sy0; sy < sy1; sy++) {
        const guint8 *row = src + (gsize)sy * src_stride + (gsize)sx0 * 4;
        for (int sx = sx0; sx < sx1; sx++, row += 4) {
          b += row[0];
          g += row[1];
          r += row[2];
          a += row[3];
        }
      }
      guint8 *out = dst + (gsize)dy * dst_stride + (gsize)dx * 4;
      out[0] = (guint8)(b / (guint32)count);
      out[1] = (guint8)(g / (guint32)count);
      out[2] = (guint8)(r / (guint32)count);
      out[3] = (guint8)(a / (guint32)count);
    }
  }

  g_autoptr(GBytes) bytes = g_bytes_new_take(dst, dst_stride * (gsize)target_h);
  return gdk_memory_texture_new(target_width, target_h, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, dst_stride);
}

static void wig_tab_widget_snapshot_ready(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(WigTabWidget) self = WIG_TAB_WIDGET(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(WebKitImage) image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source_object), result, &error);

  /* The pointer can leave, or the tab be closed, while the snapshot is in
   * flight; WebKit reports that as a failure rather than a cancellation. */
  if (!self->snapshot_cancellable)
    return;

  if (!image) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("Failed to get tab snapshot: %s", error->message);
    return;
  }

  int scale = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  int target_width = SNAPSHOT_DISPLAY_WIDTH * scale;
  g_autoptr(GdkTexture) texture = wig_tab_widget_scale_image(image, target_width);

  g_clear_pointer(&self->snapshot_popover, gtk_widget_unparent);

  GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_FILL);
  gtk_widget_set_size_request(picture, SNAPSHOT_DISPLAY_WIDTH, -1);

  gboolean in_sidebar = gtk_widget_get_ancestor(GTK_WIDGET(self), WIG_TYPE_TAB_SIDEBAR) != NULL;
  GtkPositionType position = in_sidebar ? GTK_POS_RIGHT : GTK_POS_BOTTOM;

  self->snapshot_popover = gtk_popover_new();
  g_signal_connect(self->snapshot_popover, "closed", G_CALLBACK(wig_tab_widget_snapshot_popover_closed), self);
  gtk_popover_set_autohide(GTK_POPOVER(self->snapshot_popover), FALSE);
  gtk_popover_set_child(GTK_POPOVER(self->snapshot_popover), picture);
  gtk_popover_set_has_arrow(GTK_POPOVER(self->snapshot_popover), FALSE);
  gtk_popover_set_position(GTK_POPOVER(self->snapshot_popover), position);
  gtk_widget_set_parent(self->snapshot_popover, GTK_WIDGET(self));
  gtk_popover_popup(GTK_POPOVER(self->snapshot_popover));
}

static void wig_tab_widget_snapshot_timeout(gpointer user_data)
{
  WigTabWidget *self = WIG_TAB_WIDGET(user_data);
  self->snapshot_timeout_id = 0;

  WebKitWebView *web_view = wig_tab_get_web_view(self->tab);
  if (!web_view)
    return;

  if (wig_tab_get_discarded(self->tab))
    return;

  const char *uri = webkit_web_view_get_uri(web_view);
  if (!uri || g_strcmp0(uri, "about:blank") == 0)
    return;

  if (gtk_widget_has_css_class(GTK_WIDGET(self), "active"))
    return;

  self->snapshot_cancellable = g_cancellable_new();
  webkit_web_view_get_snapshot(web_view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                               self->snapshot_cancellable, wig_tab_widget_snapshot_ready, g_object_ref(self));
}

static void wig_tab_widget_hover_enter(GtkEventControllerMotion *controller, double x, double y, WigTabWidget *self)
{
  for (GtkWidget *w = gtk_widget_get_parent(GTK_WIDGET(self)); w != NULL; w = gtk_widget_get_parent(w)) {
    if (gtk_widget_has_css_class(w, "tab-drag-active"))
      return;
  }
  if (self->snapshot_timeout_id || self->snapshot_cancellable)
    return;
  self->snapshot_timeout_id = g_timeout_add_once(500, wig_tab_widget_snapshot_timeout, self);
}

static void wig_tab_widget_hover_leave(GtkEventControllerMotion *controller, WigTabWidget *self)
{
  g_clear_handle_id(&self->snapshot_timeout_id, g_source_remove);
  if (self->snapshot_cancellable) {
    g_cancellable_cancel(self->snapshot_cancellable);
    g_clear_object(&self->snapshot_cancellable);
  }
  if (self->snapshot_popover) {
    g_signal_handlers_disconnect_by_func(self->snapshot_popover, wig_tab_widget_snapshot_popover_closed, self);
    g_clear_pointer(&self->snapshot_popover, gtk_widget_unparent);
  }
}

static void wig_tab_widget_dispose(GObject *object)
{
  WigTabWidget *self = WIG_TAB_WIDGET(object);

  g_clear_handle_id(&self->snapshot_timeout_id, g_source_remove);
  g_cancellable_cancel(self->snapshot_cancellable);
  g_clear_object(&self->snapshot_cancellable);
  g_clear_object(&self->tab);

  g_clear_pointer(&self->overlay, gtk_widget_unparent);
  self->content = NULL;
  self->title_label = NULL;
  self->spinner = NULL;
  self->audio_icon = NULL;
  self->favicon = NULL;
  for (WigCaptureKind kind = WIG_CAPTURE_CAMERA; kind <= WIG_CAPTURE_DISPLAY; kind++)
    self->capture_icons[kind] = NULL;

  g_clear_pointer(&self->context_menu_popover, gtk_widget_unparent);
  if (self->snapshot_popover) {
    g_signal_handlers_disconnect_by_func(self->snapshot_popover, wig_tab_widget_snapshot_popover_closed, self);
    g_clear_pointer(&self->snapshot_popover, gtk_widget_unparent);
  }
  G_OBJECT_CLASS(wig_tab_widget_parent_class)->dispose(object);
}

static void wig_tab_widget_close_clicked(GtkButton *button, WigTabWidget *self)
{
  g_signal_emit(self, signals[SIGNAL_CLOSE_REQUESTED], 0);
}

static void wig_tab_widget_init(WigTabWidget *self)
{
  self->overlay = gtk_overlay_new();
  gtk_widget_set_parent(self->overlay, GTK_WIDGET(self));

  self->content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->content, "tab-content");
  gtk_overlay_set_child(GTK_OVERLAY(self->overlay), self->content);

  self->title_label = gtk_label_new("New Tab");
  gtk_label_set_single_line_mode(GTK_LABEL(self->title_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->title_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(self->title_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(self->title_label, TRUE);
  gtk_box_append(GTK_BOX(self->content), self->title_label);

  /* The button floats over the title rather than taking space from it; the fade
   * box paints the tab's own background back over the text running underneath. */
  self->close_fade = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->close_fade, "tab-close-fade");
  gtk_widget_set_halign(self->close_fade, GTK_ALIGN_END);
  gtk_widget_set_valign(self->close_fade, GTK_ALIGN_FILL);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->close_fade);

  GtkWidget *close_button = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(close_button, "flat");
  gtk_widget_add_css_class(close_button, "circular");
  gtk_widget_add_css_class(close_button, "tab-close");
  gtk_widget_set_valign(close_button, GTK_ALIGN_CENTER);
  gtk_widget_set_focusable(close_button, FALSE);
  gtk_box_append(GTK_BOX(self->close_fade), close_button);
  g_signal_connect(close_button, "clicked", G_CALLBACK(wig_tab_widget_close_clicked), self);

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "enter", G_CALLBACK(wig_tab_widget_hover_enter), self);
  g_signal_connect(motion, "leave", G_CALLBACK(wig_tab_widget_hover_leave), self);
  gtk_widget_add_controller(GTK_WIDGET(self), motion);
}

static void wig_tab_widget_class_init(WigTabWidgetClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->dispose = wig_tab_widget_dispose;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-tab");

  signals[SIGNAL_CLOSE_REQUESTED] = g_signal_new("close-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                                 NULL, NULL, NULL, G_TYPE_NONE, 0);
}

void wig_tab_widget_show_context_menu(WigTabWidget *self, WigTabList *list)
{
  g_return_if_fail(WIG_IS_TAB_WIDGET(self));
  g_return_if_fail(WIG_IS_TAB_LIST(list));

  g_clear_pointer(&self->context_menu_popover, gtk_widget_unparent);
  self->context_menu_popover = wig_tab_context_menu_popup(list, self->tab);
  gtk_widget_set_parent(self->context_menu_popover, GTK_WIDGET(self));
  gtk_popover_popup(GTK_POPOVER(self->context_menu_popover));
}

GtkWidget *wig_tab_widget_new(WigTab *tab)
{
  WigTabWidget *self = WIG_TAB_WIDGET(g_object_new(WIG_TYPE_TAB_WIDGET, NULL));

  self->tab = g_object_ref(tab);
  g_object_bind_property(G_OBJECT(tab), "title", self->title_label, "label", G_BINDING_SYNC_CREATE);
  g_signal_connect_object(tab, "notify::icon", G_CALLBACK(wig_tab_widget_on_icon_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(tab, "notify::loading", G_CALLBACK(wig_tab_widget_on_loading_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(tab, "notify::playing-audio", G_CALLBACK(wig_tab_widget_on_playing_audio_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(tab, "notify::muted", G_CALLBACK(wig_tab_widget_on_muted_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(tab, "notify::pinned", G_CALLBACK(wig_tab_widget_on_pinned_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(tab, "capture-changed", G_CALLBACK(wig_tab_widget_on_capture_changed), self,
                          G_CONNECT_SWAPPED);
  wig_tab_widget_on_loading_changed(self, NULL, tab);
  wig_tab_widget_on_playing_audio_changed(self, NULL, tab);
  wig_tab_widget_on_pinned_changed(self, NULL, tab);
  wig_tab_widget_on_capture_changed(self, tab);

  return GTK_WIDGET(self);
}

WigTab *wig_tab_widget_get_tab(WigTabWidget *self)
{
  return self->tab;
}
