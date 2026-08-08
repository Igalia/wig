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

#include "wig-downloads-paintable.h"

#include <adwaita.h>

#define ICON_SIZE 16
#define RING_WIDTH 1.5
#define RING_GAP 1.0
#define CHECK_REVEAL_MS 350
#define CHECK_HOLD_MS 1200

struct _WigDownloadsPaintable {
  GObject parent;

  GtkWidget *widget;
  GtkIconPaintable *arrow;
  GtkIconPaintable *check;

  double progress;
  gboolean active;

  /* 0 shows the arrow, 1 shows the check, in between they trade places. */
  double check_progress;
  AdwAnimation *check_animation;
  guint check_hold_id;
};

static void wig_downloads_paintable_init_paintable(GdkPaintableInterface *iface);
static void wig_downloads_paintable_init_symbolic(GtkSymbolicPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(WigDownloadsPaintable, wig_downloads_paintable, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE, wig_downloads_paintable_init_paintable)
                                  G_IMPLEMENT_INTERFACE(GTK_TYPE_SYMBOLIC_PAINTABLE,
                                                        wig_downloads_paintable_init_symbolic))

static void wig_downloads_paintable_load_icons(WigDownloadsPaintable *self)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(self->widget));
  int scale = gtk_widget_get_scale_factor(self->widget);
  GtkTextDirection direction = gtk_widget_get_direction(self->widget);

  g_set_object(&self->arrow,
               gtk_icon_theme_lookup_icon(theme, "wig-download-symbolic", NULL, ICON_SIZE, scale, direction,
                                          GTK_ICON_LOOKUP_FORCE_SYMBOLIC));
  g_set_object(&self->check,
               gtk_icon_theme_lookup_icon(theme, "wig-download-done-symbolic", NULL, ICON_SIZE, scale, direction,
                                          GTK_ICON_LOOKUP_FORCE_SYMBOLIC));

  g_autoptr(GFile) arrow_file = gtk_icon_paintable_get_file(self->arrow);
  g_autoptr(GFile) check_file = gtk_icon_paintable_get_file(self->check);
  g_autofree char *arrow_uri = arrow_file ? g_file_get_uri(arrow_file) : NULL;
  g_autofree char *check_uri = check_file ? g_file_get_uri(check_file) : NULL;
  g_debug("downloads button: arrow icon '%s', check icon '%s'", arrow_uri ? arrow_uri : "(none)",
          check_uri ? check_uri : "(none)");
}

static void wig_downloads_paintable_scale_changed(WigDownloadsPaintable *self)
{
  wig_downloads_paintable_load_icons(self);
  gdk_paintable_invalidate_size(GDK_PAINTABLE(self));
}

static void wig_downloads_paintable_dispose(GObject *object)
{
  WigDownloadsPaintable *self = WIG_DOWNLOADS_PAINTABLE(object);

  g_clear_handle_id(&self->check_hold_id, g_source_remove);
  g_clear_object(&self->check_animation);
  g_clear_object(&self->arrow);
  g_clear_object(&self->check);
  g_clear_object(&self->widget);

  G_OBJECT_CLASS(wig_downloads_paintable_parent_class)->dispose(object);
}

static void wig_downloads_paintable_class_init(WigDownloadsPaintableClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = wig_downloads_paintable_dispose;
}

static void wig_downloads_paintable_init(WigDownloadsPaintable *self)
{
}

static int wig_downloads_paintable_get_intrinsic_width(GdkPaintable *paintable)
{
  WigDownloadsPaintable *self = WIG_DOWNLOADS_PAINTABLE(paintable);

  return ICON_SIZE * gtk_widget_get_scale_factor(self->widget);
}

static int wig_downloads_paintable_get_intrinsic_height(GdkPaintable *paintable)
{
  return wig_downloads_paintable_get_intrinsic_width(paintable);
}

static void wig_downloads_paintable_init_paintable(GdkPaintableInterface *iface)
{
  iface->get_intrinsic_width = wig_downloads_paintable_get_intrinsic_width;
  iface->get_intrinsic_height = wig_downloads_paintable_get_intrinsic_height;
}

/* Both glyphs are drawn about the centre so that one shrinks away as the other
 * grows in, rather than the icon appearing to jump. */
static void snapshot_glyph(GtkSymbolicPaintable *glyph, GdkSnapshot *gdk_snapshot, double width, double height,
                           double amount, const GdkRGBA *colors, gsize n_colors)
{
  GtkSnapshot *snapshot = GTK_SNAPSHOT(gdk_snapshot);
  double scale = 0.6 + 0.4 * amount;

  gtk_snapshot_push_opacity(snapshot, amount);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)width / 2.0f, (float)height / 2.0f));
  gtk_snapshot_scale(snapshot, (float)scale, (float)scale);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(-(float)width / 2.0f, -(float)height / 2.0f));
  gtk_symbolic_paintable_snapshot_symbolic(glyph, gdk_snapshot, width, height, colors, n_colors);
  gtk_snapshot_restore(snapshot);
  gtk_snapshot_pop(snapshot);
}

static void wig_downloads_paintable_snapshot_symbolic(GtkSymbolicPaintable *paintable, GdkSnapshot *gdk_snapshot,
                                                      double width, double height, const GdkRGBA *colors,
                                                      gsize n_colors)
{
  WigDownloadsPaintable *self = WIG_DOWNLOADS_PAINTABLE(paintable);

  if (self->check_progress < 1)
    snapshot_glyph(GTK_SYMBOLIC_PAINTABLE(self->arrow), gdk_snapshot, width, height, 1 - self->check_progress, colors,
                   n_colors);

  if (self->check_progress > 0)
    snapshot_glyph(GTK_SYMBOLIC_PAINTABLE(self->check), gdk_snapshot, width, height, self->check_progress, colors,
                   n_colors);

  if (!self->active)
    return;

  double radius = width / 2.0 + RING_GAP;
  double bleed = RING_GAP + RING_WIDTH;
  cairo_t *cr = gtk_snapshot_append_cairo(
      GTK_SNAPSHOT(gdk_snapshot),
      &GRAPHENE_RECT_INIT(-(float)bleed, -(float)bleed, (float)(width + bleed * 2), (float)(height + bleed * 2)));

  cairo_translate(cr, width / 2.0, height / 2.0);
  cairo_set_line_width(cr, RING_WIDTH);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  /* The full ring is the work still to do, the bright arc the part already done,
   * both starting at the top. */
  GdkRGBA track = colors[0];
  track.alpha *= 0.25f;
  gdk_cairo_set_source_rgba(cr, &track);
  cairo_arc(cr, 0, 0, radius, 0, 2 * G_PI);
  cairo_stroke(cr);

  if (self->progress > 0) {
    gdk_cairo_set_source_rgba(cr, &colors[0]);
    cairo_arc(cr, 0, 0, radius, -G_PI_2, -G_PI_2 + self->progress * 2 * G_PI);
    cairo_stroke(cr);
  }

  cairo_destroy(cr);
}

static void wig_downloads_paintable_init_symbolic(GtkSymbolicPaintableInterface *iface)
{
  iface->snapshot_symbolic = wig_downloads_paintable_snapshot_symbolic;
}

GdkPaintable *wig_downloads_paintable_new(GtkWidget *widget)
{
  WigDownloadsPaintable *self = g_object_new(WIG_TYPE_DOWNLOADS_PAINTABLE, NULL);

  self->widget = g_object_ref(widget);
  g_signal_connect_object(widget, "notify::scale-factor", G_CALLBACK(wig_downloads_paintable_scale_changed), self,
                          G_CONNECT_SWAPPED);
  wig_downloads_paintable_load_icons(self);

  return GDK_PAINTABLE(self);
}

void wig_downloads_paintable_set_progress(WigDownloadsPaintable *self, double progress)
{
  if (self->progress == progress)
    return;

  self->progress = progress;
  gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}

void wig_downloads_paintable_set_active(WigDownloadsPaintable *self, gboolean active)
{
  if (self->active == active)
    return;

  self->active = active;
  gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}

static void wig_downloads_paintable_animate_check(WigDownloadsPaintable *self, double to)
{
  adw_timed_animation_set_value_from(ADW_TIMED_ANIMATION(self->check_animation), self->check_progress);
  adw_timed_animation_set_value_to(ADW_TIMED_ANIMATION(self->check_animation), to);
  adw_animation_play(self->check_animation);
}

static void wig_downloads_paintable_hide_check(WigDownloadsPaintable *self)
{
  self->check_hold_id = 0;
  wig_downloads_paintable_animate_check(self, 0);
}

static void wig_downloads_paintable_check_animation_done(WigDownloadsPaintable *self)
{
  if (self->check_progress < 0.5)
    return;

  self->check_hold_id = g_timeout_add_once(CHECK_HOLD_MS, (GSourceOnceFunc)wig_downloads_paintable_hide_check, self);
}

static void wig_downloads_paintable_set_check_progress(double value, WigDownloadsPaintable *self)
{
  self->check_progress = value;
  gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}

void wig_downloads_paintable_flash_done(WigDownloadsPaintable *self)
{
  /* A second download landing while the check is up restarts the hold rather
   * than stacking another animation on top of it. */
  g_clear_handle_id(&self->check_hold_id, g_source_remove);

  if (!self->check_animation) {
    AdwAnimationTarget *target = adw_callback_animation_target_new(
        (AdwAnimationTargetFunc)wig_downloads_paintable_set_check_progress, self, NULL);
    self->check_animation = adw_timed_animation_new(self->widget, 0, 1, CHECK_REVEAL_MS, target);
    adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(self->check_animation), ADW_EASE_OUT_CUBIC);
    g_signal_connect_swapped(self->check_animation, "done", G_CALLBACK(wig_downloads_paintable_check_animation_done),
                             self);
  }

  wig_downloads_paintable_animate_check(self, 1);
}
