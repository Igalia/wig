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

#include "wig-search-bar.h"

#include <wpe/webkit.h>

#define MAX_MATCH_COUNT 1000
#define FIND_OPTIONS (WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND)

struct _WigSearchBar {
  GtkWidget parent;

  GtkWidget *box;
  GtkWidget *entry;
  GtkWidget *match_label;
  GtkWidget *highlight_button;

  WigTab *tab;
  gulong found_text_id;
  gulong failed_to_find_text_id;
  gulong counted_matches_id;
};

G_DEFINE_FINAL_TYPE(WigSearchBar, wig_search_bar, GTK_TYPE_WIDGET)

enum {
  CLOSED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* The tab drops its view in dispose but the weak pointer above only clears at
 * finalize, so the view can already be gone while @tab still reads as valid. */
static WebKitFindController *wig_search_bar_get_controller(WigSearchBar *self)
{
  WebKitWebView *web_view = self->tab ? wig_tab_get_web_view(self->tab) : NULL;
  return web_view ? webkit_web_view_get_find_controller(web_view) : NULL;
}

static gboolean wig_search_bar_highlights_all(WigSearchBar *self)
{
  return gtk_check_button_get_active(GTK_CHECK_BUTTON(self->highlight_button));
}

static gboolean wig_search_bar_entry_matches_controller(WigSearchBar *self)
{
  WebKitFindController *controller = wig_search_bar_get_controller(self);
  if (!controller)
    return FALSE;

  const char *searched = webkit_find_controller_get_search_text(controller);
  return searched && g_str_equal(searched, gtk_editable_get_text(GTK_EDITABLE(self->entry)));
}

static void wig_search_bar_set_match_count(WigSearchBar *self, guint match_count)
{
  g_autofree char *text = NULL;

  if (match_count == 0)
    text = g_strdup("No matches");
  else if (match_count >= MAX_MATCH_COUNT)
    text = g_strdup_printf("%u+ matches", MAX_MATCH_COUNT);
  else if (match_count == 1)
    text = g_strdup("1 match");
  else
    text = g_strdup_printf("%u matches", match_count);

  gtk_label_set_text(GTK_LABEL(self->match_label), text);
}

/* The count outlives the search session it came from: it has to be shown again
 * when the tab comes back, and by then it can no longer be asked for. */
static void wig_search_bar_record_match_count(WigSearchBar *self, guint match_count)
{
  if (self->tab)
    wig_tab_set_search_match_count(self->tab, match_count);

  wig_search_bar_set_match_count(self, match_count);
}

static void wig_search_bar_search(WigSearchBar *self)
{
  WebKitFindController *controller = wig_search_bar_get_controller(self);
  if (!controller)
    return;

  const char *text = gtk_editable_get_text(GTK_EDITABLE(self->entry));
  if (!*text) {
    webkit_find_controller_search_finish(controller);
    wig_tab_set_search_active(self->tab, FALSE);
    gtk_widget_remove_css_class(self->entry, "error");
    gtk_label_set_text(GTK_LABEL(self->match_label), "");
    return;
  }

  wig_tab_set_search_active(self->tab, TRUE);

  /* Only counted-matches reports a usable total, so ask for it explicitly rather
   * than relying on whatever count the find operation below reports. */
  webkit_find_controller_count_matches(controller, text, FIND_OPTIONS, MAX_MATCH_COUNT);

  /* webkit_find_controller_search() always marks every match, and there is no
   * option to suppress that, so stepping onto a match is the only way to search
   * without highlighting. */
  if (wig_search_bar_highlights_all(self))
    webkit_find_controller_search(controller, text, FIND_OPTIONS, MAX_MATCH_COUNT);
  else
    webkit_find_controller_search_next(controller);
}

/* @match_count is deliberately ignored: stepping to the next or previous match
 * emits this again reporting only the match landed on, not the total. */
static void wig_search_bar_on_found_text(WebKitFindController *controller, guint match_count, WigSearchBar *self)
{
  gtk_widget_remove_css_class(self->entry, "error");
}

static void wig_search_bar_on_failed_to_find_text(WebKitFindController *controller, WigSearchBar *self)
{
  gtk_widget_add_css_class(self->entry, "error");
  wig_search_bar_record_match_count(self, 0);
}

static void wig_search_bar_on_counted_matches(WebKitFindController *controller, guint match_count, WigSearchBar *self)
{
  wig_search_bar_record_match_count(self, match_count);
}

static void wig_search_bar_on_search_changed(GtkSearchEntry *entry, WigSearchBar *self)
{
  if (self->tab && wig_tab_get_search_active(self->tab) && wig_search_bar_entry_matches_controller(self))
    return;

  wig_search_bar_search(self);
}

static void wig_search_bar_on_next_clicked(GtkButton *button, WigSearchBar *self)
{
  wig_search_bar_find_next(self);
}

static void wig_search_bar_on_previous_clicked(GtkButton *button, WigSearchBar *self)
{
  wig_search_bar_find_previous(self);
}

static void wig_search_bar_on_close_clicked(GtkButton *button, WigSearchBar *self)
{
  wig_search_bar_close(self);
}

static void wig_search_bar_on_stop_search(GtkSearchEntry *entry, WigSearchBar *self)
{
  wig_search_bar_close(self);
}

static void wig_search_bar_on_entry_next(GtkSearchEntry *entry, WigSearchBar *self)
{
  wig_search_bar_find_next(self);
}

static void wig_search_bar_on_entry_previous(GtkSearchEntry *entry, WigSearchBar *self)
{
  wig_search_bar_find_previous(self);
}

static void wig_search_bar_on_highlight_toggled(GtkCheckButton *button, WigSearchBar *self)
{
  WebKitFindController *controller = wig_search_bar_get_controller(self);
  if (controller)
    webkit_find_controller_search_finish(controller);

  wig_search_bar_search(self);
}

static gboolean wig_search_bar_escape_binding(GtkWidget *widget, GVariant *args, gpointer user_data)
{
  wig_search_bar_close(WIG_SEARCH_BAR(widget));
  return TRUE;
}

static void wig_search_bar_dispose(GObject *object)
{
  WigSearchBar *self = WIG_SEARCH_BAR(object);

  wig_search_bar_set_tab(self, NULL);
  g_clear_pointer(&self->box, gtk_widget_unparent);

  G_OBJECT_CLASS(wig_search_bar_parent_class)->dispose(object);
}

static void wig_search_bar_class_init(WigSearchBarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_search_bar_dispose;

  gtk_widget_class_set_css_name(widget_class, "wig-search-bar");
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_add_binding(widget_class, GDK_KEY_Escape, 0, wig_search_bar_escape_binding, NULL);

  signals[CLOSED] = g_signal_new("closed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
}

static void wig_search_bar_init(WigSearchBar *self)
{
  self->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(self->box, "search-bar-box");
  gtk_widget_set_parent(self->box, GTK_WIDGET(self));

  self->entry = gtk_search_entry_new();
  gtk_editable_set_width_chars(GTK_EDITABLE(self->entry), 30);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->entry), "Find in page");
  g_signal_connect(self->entry, "search-changed", G_CALLBACK(wig_search_bar_on_search_changed), self);
  g_signal_connect(self->entry, "next-match", G_CALLBACK(wig_search_bar_on_entry_next), self);
  g_signal_connect(self->entry, "previous-match", G_CALLBACK(wig_search_bar_on_entry_previous), self);
  g_signal_connect(self->entry, "stop-search", G_CALLBACK(wig_search_bar_on_stop_search), self);
  g_signal_connect(self->entry, "activate", G_CALLBACK(wig_search_bar_on_entry_next), self);
  gtk_box_append(GTK_BOX(self->box), self->entry);

  GtkWidget *previous_button = gtk_button_new_from_icon_name("go-up-symbolic");
  gtk_widget_add_css_class(previous_button, "flat");
  gtk_widget_set_tooltip_text(previous_button, "Find Previous");
  g_signal_connect(previous_button, "clicked", G_CALLBACK(wig_search_bar_on_previous_clicked), self);
  gtk_box_append(GTK_BOX(self->box), previous_button);

  GtkWidget *next_button = gtk_button_new_from_icon_name("go-down-symbolic");
  gtk_widget_add_css_class(next_button, "flat");
  gtk_widget_set_tooltip_text(next_button, "Find Next");
  g_signal_connect(next_button, "clicked", G_CALLBACK(wig_search_bar_on_next_clicked), self);
  gtk_box_append(GTK_BOX(self->box), next_button);

  self->highlight_button = gtk_check_button_new_with_label("Highlight All");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(self->highlight_button), TRUE);
  g_signal_connect(self->highlight_button, "toggled", G_CALLBACK(wig_search_bar_on_highlight_toggled), self);
  gtk_box_append(GTK_BOX(self->box), self->highlight_button);

  self->match_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->match_label, "dim-label");
  gtk_widget_set_halign(self->match_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(self->match_label, TRUE);
  gtk_box_append(GTK_BOX(self->box), self->match_label);

  GtkWidget *close_button = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(close_button, "flat");
  gtk_widget_set_tooltip_text(close_button, "Close");
  g_signal_connect(close_button, "clicked", G_CALLBACK(wig_search_bar_on_close_clicked), self);
  gtk_box_append(GTK_BOX(self->box), close_button);

  gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

GtkWidget *wig_search_bar_new(void)
{
  return g_object_new(WIG_TYPE_SEARCH_BAR, NULL);
}

void wig_search_bar_set_tab(WigSearchBar *self, WigTab *tab)
{
  if (self->tab == tab)
    return;

  if (self->tab) {
    /* Deliberately not finishing the search, so the outgoing tab keeps its find
     * session and its marks. */
    WebKitFindController *controller = wig_search_bar_get_controller(self);
    if (controller) {
      g_clear_signal_handler(&self->found_text_id, controller);
      g_clear_signal_handler(&self->failed_to_find_text_id, controller);
      g_clear_signal_handler(&self->counted_matches_id, controller);
    } else {
      /* The view went away and took the controller, and the handlers with it. */
      self->found_text_id = 0;
      self->failed_to_find_text_id = 0;
      self->counted_matches_id = 0;
    }
    g_object_remove_weak_pointer(G_OBJECT(self->tab), (gpointer *)&self->tab);
  }

  /* The tab list owns the tab and can drop it while the bar still points at it. */
  self->tab = tab;

  if (self->tab) {
    g_object_add_weak_pointer(G_OBJECT(self->tab), (gpointer *)&self->tab);

    WebKitFindController *controller = wig_search_bar_get_controller(self);
    self->found_text_id = g_signal_connect_object(controller, "found-text", G_CALLBACK(wig_search_bar_on_found_text),
                                                  self, G_CONNECT_DEFAULT);
    self->failed_to_find_text_id = g_signal_connect_object(
        controller, "failed-to-find-text", G_CALLBACK(wig_search_bar_on_failed_to_find_text), self, G_CONNECT_DEFAULT);
    self->counted_matches_id = g_signal_connect_object(
        controller, "counted-matches", G_CALLBACK(wig_search_bar_on_counted_matches), self, G_CONNECT_DEFAULT);
  }

  gboolean active = self->tab && wig_tab_get_search_active(self->tab);
  const char *searched = active ? webkit_find_controller_get_search_text(wig_search_bar_get_controller(self)) : NULL;

  gtk_editable_set_text(GTK_EDITABLE(self->entry), searched ? searched : "");
  gtk_widget_set_visible(GTK_WIDGET(self), active);

  if (!active) {
    gtk_label_set_text(GTK_LABEL(self->match_label), "");
    gtk_widget_remove_css_class(self->entry, "error");
    return;
  }

  /* Redisplaying the stored count rather than counting again: counting would drop
   * the marks the search left in the page, and searching again would move the
   * selection. */
  guint match_count = wig_tab_get_search_match_count(self->tab);
  wig_search_bar_set_match_count(self, match_count);

  if (match_count == 0)
    gtk_widget_add_css_class(self->entry, "error");
  else
    gtk_widget_remove_css_class(self->entry, "error");
}

void wig_search_bar_open(WigSearchBar *self)
{
  g_return_if_fail(WIG_IS_SEARCH_BAR(self));

  gtk_widget_set_visible(GTK_WIDGET(self), TRUE);
  gtk_widget_grab_focus(self->entry);
  gtk_editable_select_region(GTK_EDITABLE(self->entry), 0, -1);
}

void wig_search_bar_close(WigSearchBar *self)
{
  g_return_if_fail(WIG_IS_SEARCH_BAR(self));

  if (!gtk_widget_get_visible(GTK_WIDGET(self)))
    return;

  WebKitFindController *controller = wig_search_bar_get_controller(self);
  if (controller)
    webkit_find_controller_search_finish(controller);
  if (self->tab)
    wig_tab_set_search_active(self->tab, FALSE);

  gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
  g_signal_emit(self, signals[CLOSED], 0);
}

gboolean wig_search_bar_is_open(WigSearchBar *self)
{
  g_return_val_if_fail(WIG_IS_SEARCH_BAR(self), FALSE);

  return gtk_widget_get_visible(GTK_WIDGET(self));
}

void wig_search_bar_find_next(WigSearchBar *self)
{
  g_return_if_fail(WIG_IS_SEARCH_BAR(self));

  /* Stepping before a search has been started is a WebKit programming error. */
  WebKitFindController *controller = wig_search_bar_get_controller(self);
  if (controller && webkit_find_controller_get_search_text(controller))
    webkit_find_controller_search_next(controller);
}

void wig_search_bar_find_previous(WigSearchBar *self)
{
  g_return_if_fail(WIG_IS_SEARCH_BAR(self));

  WebKitFindController *controller = wig_search_bar_get_controller(self);
  if (controller && webkit_find_controller_get_search_text(controller))
    webkit_find_controller_search_previous(controller);
}
