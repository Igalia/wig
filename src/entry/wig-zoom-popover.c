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

#include "wig-zoom-popover.h"

#include <math.h>

struct _WigZoomPopover {
  GtkPopover parent;

  GtkWidget *scale;
};

G_DEFINE_FINAL_TYPE(WigZoomPopover, wig_zoom_popover, GTK_TYPE_POPOVER)

enum {
  SIGNAL_PERCENT_SELECTED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static gboolean scale_change_value(GtkRange *range, GtkScrollType scroll, double value, WigZoomPopover *self)
{
  int percent = CLAMP((int)round(value / WIG_ZOOM_STEP_PERCENT) * WIG_ZOOM_STEP_PERCENT, WIG_ZOOM_MIN_PERCENT,
                      WIG_ZOOM_MAX_PERCENT);
  if (percent != (int)gtk_range_get_value(range))
    g_signal_emit(self, signals[SIGNAL_PERCENT_SELECTED], 0, percent);
  return TRUE;
}

static char *format_percent(GtkScale *scale, double value, gpointer user_data)
{
  return g_strdup_printf("%.0f%%", value);
}

static void wig_zoom_popover_class_init(WigZoomPopoverClass *klass)
{
  signals[SIGNAL_PERCENT_SELECTED] = g_signal_new("percent-selected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                                  NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void wig_zoom_popover_init(WigZoomPopover *self)
{
  GtkAdjustment *adjustment = gtk_adjustment_new(100, WIG_ZOOM_MIN_PERCENT, WIG_ZOOM_MAX_PERCENT, WIG_ZOOM_STEP_PERCENT,
                                                 WIG_ZOOM_STEP_PERCENT * 5, 0);
  self->scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adjustment);
  gtk_scale_set_draw_value(GTK_SCALE(self->scale), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(self->scale), GTK_POS_RIGHT);
  gtk_scale_set_format_value_func(GTK_SCALE(self->scale), format_percent, NULL, NULL);
  gtk_scale_add_mark(GTK_SCALE(self->scale), 100, GTK_POS_BOTTOM, NULL);
  gtk_widget_set_size_request(self->scale, 240, -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->scale), GTK_ACCESSIBLE_PROPERTY_LABEL, "Zoom", -1);
  g_signal_connect(self->scale, "change-value", G_CALLBACK(scale_change_value), self);

  gtk_popover_set_child(GTK_POPOVER(self), self->scale);
}

GtkWidget *wig_zoom_popover_new(void)
{
  return g_object_new(WIG_TYPE_ZOOM_POPOVER, NULL);
}

void wig_zoom_popover_set_percent(WigZoomPopover *self, int percent)
{
  gtk_range_set_value(GTK_RANGE(self->scale), percent);
}
