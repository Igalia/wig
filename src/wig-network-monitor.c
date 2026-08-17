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

#include "wig-network-monitor.h"

/* A network is reported the moment an interface comes up, while the address, the
 * route and the resolver land over the following moment: a load started in
 * between fails exactly as it did when there was no network at all. Each of
 * those steps is a change of its own, so waiting for them to stop arriving
 * serves both settling and coalescing. */
#define WIG_NETWORK_SETTLE_MS 750

struct _WigNetworkMonitor {
  GObject parent;

  GNetworkMonitor *monitor;
  GNetworkConnectivity connectivity;
  gboolean available;
  guint settle_id;
};

G_DEFINE_FINAL_TYPE(WigNetworkMonitor, wig_network_monitor, G_TYPE_OBJECT)

enum {
  CAME_ONLINE,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static gboolean wig_network_monitor_settled(gpointer user_data)
{
  WigNetworkMonitor *self = user_data;

  self->settle_id = 0;

  g_debug("network: settled with connectivity %d, the network is usable again", self->connectivity);
  g_signal_emit(self, signals[CAME_ONLINE], 0);

  return G_SOURCE_REMOVE;
}

static void wig_network_monitor_changed(WigNetworkMonitor *self, gboolean available, GNetworkMonitor *monitor)
{
  GNetworkConnectivity connectivity = g_network_monitor_get_connectivity(monitor);
  gboolean was_available = self->available;
  GNetworkConnectivity was_connectivity = self->connectivity;

  g_debug("network: changed, available %d (was %d), connectivity %d (was %d)", available, was_available, connectivity,
          was_connectivity);

  self->available = available;
  self->connectivity = connectivity;

  if (!available) {
    g_clear_handle_id(&self->settle_id, g_source_remove);
    return;
  }

  /* Connectivity only reaches FULL once something past the local network
   * answers, so paying a captive portal arrives here as connectivity changing
   * rather than as a network appearing. Backends with no way to tell (anything
   * but NetworkManager) report FULL as soon as the network is up, where the two
   * are the same event anyway. */
  if (was_available && !(connectivity == G_NETWORK_CONNECTIVITY_FULL && was_connectivity != connectivity))
    return;

  g_clear_handle_id(&self->settle_id, g_source_remove);
  self->settle_id = g_timeout_add(WIG_NETWORK_SETTLE_MS, wig_network_monitor_settled, self);
}

static void wig_network_monitor_dispose(GObject *object)
{
  WigNetworkMonitor *self = WIG_NETWORK_MONITOR(object);

  g_clear_handle_id(&self->settle_id, g_source_remove);

  G_OBJECT_CLASS(wig_network_monitor_parent_class)->dispose(object);
}

static void wig_network_monitor_class_init(WigNetworkMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = wig_network_monitor_dispose;

  signals[CAME_ONLINE] = g_signal_new("came-online", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                      G_TYPE_NONE, 0);
}

static void wig_network_monitor_init(WigNetworkMonitor *self)
{
  self->monitor = g_network_monitor_get_default();
  self->available = g_network_monitor_get_network_available(self->monitor);
  self->connectivity = g_network_monitor_get_connectivity(self->monitor);

  g_debug("network: watching %s, available %d, connectivity %d", G_OBJECT_TYPE_NAME(self->monitor), self->available,
          self->connectivity);

  g_signal_connect_object(self->monitor, "network-changed", G_CALLBACK(wig_network_monitor_changed), self,
                          G_CONNECT_SWAPPED);
}

WigNetworkMonitor *wig_network_monitor_new(void)
{
  return g_object_new(WIG_TYPE_NETWORK_MONITOR, NULL);
}

gboolean wig_network_monitor_get_available(WigNetworkMonitor *self)
{
  g_assert(WIG_IS_NETWORK_MONITOR(self));

  return self->available;
}
