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

#include "wig-settings-memory.h"

#include "wig-application.h"

#include <wpe/webkit.h>

typedef enum {
  MEMORY_LIMIT,
  CONSERVATIVE_THRESHOLD,
  STRICT_THRESHOLD,
  KILL_THRESHOLD,
  POLL_INTERVAL,
  N_MEMORY_VALUES,
} MemoryValue;

/* The thresholds are fractions of the limit, so they share a range and only
 * differ in what crossing them costs. */
static const struct {
  const char *key;
  const char *title;
  const char *description;
  double min;
  double max;
  double step;
  guint digits;
} memory_values[N_MEMORY_VALUES] = {
  [MEMORY_LIMIT] = { "memory-limit", "Memory Limit",
                     "How much memory the process may use, in MiB. Zero leaves it to the system.", 0, 1024 * 1024, 16,
                     0 },
  [CONSERVATIVE_THRESHOLD] = { "memory-conservative-threshold", "Conservative Threshold",
                               "Fraction of the limit at which careful collection begins.", 0, 1, 0.01, 2 },
  [STRICT_THRESHOLD] = { "memory-strict-threshold", "Strict Threshold",
                         "Fraction at which collection turns aggressive and caches are dropped.", 0, 1, 0.01, 2 },
  [KILL_THRESHOLD] = { "memory-kill-threshold", "Kill Threshold",
                       "Fraction at which the process is ended outright. Zero never ends it.", 0, 1, 0.01, 2 },
  [POLL_INTERVAL] = { "memory-poll-interval", "Poll Interval", "How often memory use is looked at, in seconds.", 0.1,
                      600, 0.1, 1 },
};

struct _WigSettingsMemory {
  GtkWidget parent;

  GtkWidget *page;
  AdwBanner *banner;
};

G_DEFINE_FINAL_TYPE(WigSettingsMemory, wig_settings_memory, GTK_TYPE_WIDGET)

static double memory_value_get(WebKitMemoryPressureSettings *settings, MemoryValue value)
{
  switch (value) {
  case MEMORY_LIMIT:
    return webkit_memory_pressure_settings_get_memory_limit(settings);
  case CONSERVATIVE_THRESHOLD:
    return webkit_memory_pressure_settings_get_conservative_threshold(settings);
  case STRICT_THRESHOLD:
    return webkit_memory_pressure_settings_get_strict_threshold(settings);
  case KILL_THRESHOLD:
    return webkit_memory_pressure_settings_get_kill_threshold(settings);
  case POLL_INTERVAL:
    return webkit_memory_pressure_settings_get_poll_interval(settings);
  default:
    g_assert_not_reached();
  }
}

static double memory_stored_get(GSettings *settings, MemoryValue value)
{
  if (value == MEMORY_LIMIT)
    return g_settings_get_uint(settings, memory_values[value].key);
  return g_settings_get_double(settings, memory_values[value].key);
}

static void memory_stored_set(GSettings *settings, MemoryValue value, double amount)
{
  if (value == MEMORY_LIMIT)
    g_settings_set_uint(settings, memory_values[value].key, (guint)amount);
  else
    g_settings_set_double(settings, memory_values[value].key, amount);
}

static double memory_value_effective(GSettings *settings, WebKitMemoryPressureSettings *pressure, MemoryValue value)
{
  double stored = memory_stored_get(settings, value);

  return stored > 0 ? stored : memory_value_get(pressure, value);
}

void wig_settings_memory_apply(GSettings *settings, WebKitMemoryPressureSettings *pressure)
{
  double limit = memory_stored_get(settings, MEMORY_LIMIT);
  if (limit > 0)
    webkit_memory_pressure_settings_set_memory_limit(pressure, (guint)limit);

  double interval = memory_stored_get(settings, POLL_INTERVAL);
  if (interval > 0)
    webkit_memory_pressure_settings_set_poll_interval(pressure, interval);

  double conservative = memory_value_effective(settings, pressure, CONSERVATIVE_THRESHOLD);
  double strict = memory_value_effective(settings, pressure, STRICT_THRESHOLD);
  double kill = memory_stored_get(settings, KILL_THRESHOLD);

  if (conservative <= 0 || conservative >= strict || strict >= 1 || (kill != 0 && kill <= strict)) {
    g_warning("memory: thresholds %g/%g/%g do not rise in that order, keeping the ones in effect", conservative, strict,
              kill);
    return;
  }

  webkit_memory_pressure_settings_set_kill_threshold(pressure, 0);
  if (strict > webkit_memory_pressure_settings_get_conservative_threshold(pressure)) {
    webkit_memory_pressure_settings_set_strict_threshold(pressure, strict);
    webkit_memory_pressure_settings_set_conservative_threshold(pressure, conservative);
  } else {
    webkit_memory_pressure_settings_set_conservative_threshold(pressure, conservative);
    webkit_memory_pressure_settings_set_strict_threshold(pressure, strict);
  }
  webkit_memory_pressure_settings_set_kill_threshold(pressure, kill);

  g_debug("memory: limit %u MiB, thresholds %g/%g/%g, poll every %g s",
          webkit_memory_pressure_settings_get_memory_limit(pressure), conservative, strict, kill,
          webkit_memory_pressure_settings_get_poll_interval(pressure));
}

/* Which value a row stands for is small enough to travel as the closure data
 * itself, and the settings it acts on belong to the application. */
static void memory_row_changed(AdwSpinRow *row, GParamSpec *pspec, gpointer data)
{
  MemoryValue value = GPOINTER_TO_UINT(data);
  WigSettingsMemory *self = WIG_SETTINGS_MEMORY(gtk_widget_get_ancestor(GTK_WIDGET(row), WIG_TYPE_SETTINGS_MEMORY));
  WigApplication *app = wig_application_get();
  GSettings *settings = wig_application_get_settings(app);
  WebKitMemoryPressureSettings *pressure = wig_application_get_memory_pressure_settings(app);

  g_debug("memory: %s is now %g", memory_values[value].title, adw_spin_row_get_value(row));
  memory_stored_set(settings, value, adw_spin_row_get_value(row));
  wig_settings_memory_apply(settings, pressure);

  /* This only stores the configuration WebKit hands to the next network process
   * it starts; the one already running keeps what it was given, so the way to
   * see any of this is to start again. */
  webkit_network_session_set_memory_pressure_settings(pressure);
  adw_banner_set_revealed(self->banner, TRUE);
}

static GtkWidget *memory_row_new(GSettings *settings, WebKitMemoryPressureSettings *pressure, MemoryValue value)
{
  GtkWidget *row = adw_spin_row_new_with_range(memory_values[value].min, memory_values[value].max,
                                               memory_values[value].step);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), memory_values[value].title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), memory_values[value].description);
  adw_spin_row_set_digits(ADW_SPIN_ROW(row), memory_values[value].digits);
  adw_spin_row_set_value(ADW_SPIN_ROW(row), memory_value_effective(settings, pressure, value));
  /* Connected once the row holds what the setting already is. */
  g_signal_connect(row, "notify::value", G_CALLBACK(memory_row_changed), GUINT_TO_POINTER(value));

  return row;
}

void wig_settings_memory_index(WigSettingsSearch *search, const char *pane, const char *pane_title)
{
  for (guint i = 0; i < N_MEMORY_VALUES; i++)
    wig_settings_search_add(search, memory_values[i].title, memory_values[i].description, pane, pane_title);
}

static void wig_settings_memory_dispose(GObject *object)
{
  WigSettingsMemory *self = WIG_SETTINGS_MEMORY(object);

  g_clear_pointer(&self->page, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_settings_memory_parent_class)->dispose(object);
}

static void wig_settings_memory_class_init(WigSettingsMemoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_settings_memory_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "wig-settings-memory");
}

static void wig_settings_memory_init(WigSettingsMemory *self)
{
  WigApplication *app = wig_application_get();
  GSettings *settings = wig_application_get_settings(app);
  WebKitMemoryPressureSettings *pressure = wig_application_get_memory_pressure_settings(app);

  self->page = adw_preferences_page_new();
  gtk_widget_set_parent(self->page, GTK_WIDGET(self));

  self->banner = ADW_BANNER(adw_banner_new("Changes take effect after restarting the browser."));
  adw_banner_set_button_label(self->banner, "Restart Browser");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(self->banner), "app.restart");
  adw_preferences_page_set_banner(ADW_PREFERENCES_PAGE(self->page), self->banner);

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, "Memory Limits");
  adw_preferences_group_set_description(
      group,
      "How much memory the network process may use before WebKit collects, drops caches, or ends it. These "
      "require restarting the browser.");

  for (guint i = 0; i < N_MEMORY_VALUES; i++)
    adw_preferences_group_add(group, memory_row_new(settings, pressure, i));

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self->page), group);
}

GtkWidget *wig_settings_memory_new(void)
{
  return g_object_new(WIG_TYPE_SETTINGS_MEMORY, NULL);
}
