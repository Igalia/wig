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

#include "wig-bookmarks-dialog.h"

#include "wig-application.h"
#include "wig-bookmark-context-menu.h"
#include "wig-bookmark-row.h"
#include "wig-bookmarks-store.h"

#define BOOKMARKS_DIALOG_SEARCH_LIMIT 100
#define BOOKMARKS_DIALOG_CRUMB_WIDTH 20
#define BOOKMARKS_DIALOG_SHOWN_ANCESTORS 1

enum {
  OPEN_URI_SIGNAL,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

struct _WigBookmarksDialog {
  AdwDialog parent;

  WigBookmarksStore *store;
  GListStore *items;
  GListStore *footer;
  char *folder_id;
  char *query;
  gboolean creating;

  GtkWidget *breadcrumbs;
  GtkWidget *entry;
  GtkWidget *stack;
  GtkWidget *list;
  GtkWidget *empty;
  GtkWidget *edit_popover;
  GtkWidget *edit_name;
  GtkWidget *edit_url;
  char *editing_id;
  GdkRectangle menu_target;
};

G_DEFINE_FINAL_TYPE(WigBookmarksDialog, wig_bookmarks_dialog, ADW_TYPE_DIALOG)

static void wig_bookmarks_dialog_open_folder(WigBookmarksDialog *self, const char *folder_id);

static void wig_bookmarks_dialog_breadcrumb_clicked(WigBookmarksDialog *self, GtkButton *button)
{
  const char *folder_id = gtk_widget_get_name(GTK_WIDGET(button));

  wig_bookmarks_dialog_open_folder(self, *folder_id ? folder_id : NULL);
}

static void wig_bookmarks_dialog_append_crumb(WigBookmarksDialog *self, GtkWidget *crumb)
{
  if (gtk_widget_get_first_child(self->breadcrumbs)) {
    GtkWidget *separator = gtk_image_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(separator, "dim-label");
    gtk_box_append(GTK_BOX(self->breadcrumbs), separator);
  }

  gtk_box_append(GTK_BOX(self->breadcrumbs), crumb);
}

static void wig_bookmarks_dialog_add_breadcrumb(WigBookmarksDialog *self, const char *label, const char *folder_id,
                                                gboolean is_current)
{
  GtkWidget *name = gtk_label_new(label);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(name), BOOKMARKS_DIALOG_CRUMB_WIDTH);

  GtkWidget *button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(button), name);
  gtk_widget_set_tooltip_text(button, label);
  gtk_widget_set_name(button, folder_id ? folder_id : "");
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_sensitive(button, !is_current);
  g_signal_connect_object(button, "clicked", G_CALLBACK(wig_bookmarks_dialog_breadcrumb_clicked), self,
                          G_CONNECT_SWAPPED);
  wig_bookmarks_dialog_append_crumb(self, button);
}

static void wig_bookmarks_dialog_add_collapsed_breadcrumbs(WigBookmarksDialog *self, GPtrArray *ancestors, guint count)
{
  g_autoptr(GMenu) menu = g_menu_new();

  for (guint i = 0; i < count; i++) {
    WigBookmark *ancestor = g_ptr_array_index(ancestors, i);
    g_autoptr(GMenuItem) item = g_menu_item_new(wig_bookmark_get_title(ancestor), NULL);

    g_menu_item_set_action_and_target(item, "bookmark.open-folder", "s", wig_bookmark_get_id(ancestor));
    g_menu_append_item(menu, item);
  }

  GtkWidget *button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "view-more-horizontal-symbolic");
  gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(button), FALSE);
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
  gtk_widget_set_tooltip_text(button, "Enclosing Folders");
  wig_bookmarks_dialog_append_crumb(self, button);
}

static void wig_bookmarks_dialog_build_breadcrumbs(WigBookmarksDialog *self)
{
  GtkWidget *child = gtk_widget_get_first_child(self->breadcrumbs);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(self->breadcrumbs), child);
    child = next;
  }

  wig_bookmarks_dialog_add_breadcrumb(self, "Bookmarks", NULL, self->folder_id == NULL);

  if (!self->folder_id || !self->store)
    return;

  g_autoptr(GPtrArray) ancestors = wig_bookmarks_store_get_ancestors(self->store, self->folder_id, NULL);
  guint first_shown = 0;

  if (ancestors && ancestors->len > BOOKMARKS_DIALOG_SHOWN_ANCESTORS + 1) {
    first_shown = ancestors->len - BOOKMARKS_DIALOG_SHOWN_ANCESTORS;
    wig_bookmarks_dialog_add_collapsed_breadcrumbs(self, ancestors, first_shown);
  }

  for (guint i = first_shown; ancestors && i < ancestors->len; i++) {
    WigBookmark *ancestor = g_ptr_array_index(ancestors, i);
    wig_bookmarks_dialog_add_breadcrumb(self, wig_bookmark_get_title(ancestor), wig_bookmark_get_id(ancestor), FALSE);
  }

  g_autoptr(WigBookmark) current = wig_bookmarks_store_get(self->store, self->folder_id, NULL);
  if (current)
    wig_bookmarks_dialog_add_breadcrumb(self, wig_bookmark_get_title(current), self->folder_id, TRUE);
}

static void wig_bookmarks_dialog_load(WigBookmarksDialog *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = NULL;

  g_list_store_remove_all(self->items);

  if (self->store && self->query)
    items = wig_bookmarks_store_search(self->store, self->query, BOOKMARKS_DIALOG_SEARCH_LIMIT, &error);
  else if (self->store)
    items = wig_bookmarks_store_get_children(self->store, self->folder_id, &error);

  if (error)
    g_warning("bookmarks: query failed: %s", error->message);

  for (guint i = 0; items && i < items->len; i++)
    g_list_store_append(self->items, g_ptr_array_index(items, i));

  /* Making a folder is an act on the folder being looked at, so it is offered at
   * the end of that folder rather than over a set of search results. */
  g_list_store_remove_all(self->footer);
  if (!self->query) {
    g_autoptr(GObject) marker = g_object_new(G_TYPE_OBJECT, NULL);
    g_list_store_append(self->footer, marker);
  }

  wig_bookmarks_dialog_build_breadcrumbs(self);
  gtk_widget_set_visible(self->breadcrumbs, self->query == NULL);

  g_debug("bookmarks: showing %u item(s) in %s", g_list_model_get_n_items(G_LIST_MODEL(self->items)),
          self->query           ? "search results"
              : self->folder_id ? self->folder_id
                                : "the root");

  adw_status_page_set_title(ADW_STATUS_PAGE(self->empty), self->query ? "No Results" : "Nothing Here");
  adw_status_page_set_description(ADW_STATUS_PAGE(self->empty),
                                  self->query ? "No bookmark matches that." : "Bookmarks you keep are listed here.");
  gboolean has_rows = g_list_model_get_n_items(G_LIST_MODEL(self->items)) > 0 || !self->query;
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack), has_rows ? "list" : "empty");
}

static void wig_bookmarks_dialog_open_folder(WigBookmarksDialog *self, const char *folder_id)
{
  g_set_str(&self->folder_id, folder_id);
  wig_bookmarks_dialog_load(self);
}

static void wig_bookmarks_dialog_store_changed(WigBookmarksDialog *self)
{
  wig_bookmarks_dialog_load(self);
}

static void wig_bookmarks_dialog_edit_applied(WigBookmarksDialog *self)
{
  if (!self->store)
    return;

  g_autoptr(GError) error = NULL;
  const char *name = gtk_editable_get_text(GTK_EDITABLE(self->edit_name));
  const char *url = gtk_widget_get_visible(self->edit_url) ? gtk_editable_get_text(GTK_EDITABLE(self->edit_url)) : "";

  if (gtk_widget_get_visible(self->edit_url) && !*url) {
    gtk_widget_add_css_class(self->edit_url, "error");
    gtk_widget_grab_focus(self->edit_url);
    return;
  }

  gtk_widget_remove_css_class(self->edit_url, "error");

  if (self->creating) {
    g_autoptr(WigBookmark) added = wig_bookmarks_store_add_folder(self->store, self->folder_id,
                                                                  *name ? name : "New Folder", &error);
  } else if (self->editing_id) {
    wig_bookmarks_store_update(self->store, self->editing_id, name, url, &error);
  }

  if (error)
    g_warning("bookmarks: could not save: %s", error->message);

  gtk_popover_popdown(GTK_POPOVER(self->edit_popover));
}

static void wig_bookmarks_dialog_new_folder_clicked(WigBookmarksDialog *self, GtkButton *button)
{
  graphene_rect_t bounds;
  if (gtk_widget_compute_bounds(GTK_WIDGET(button), GTK_WIDGET(self), &bounds))
    self->menu_target = (GdkRectangle) { (int)bounds.origin.x, (int)bounds.origin.y, (int)bounds.size.width,
                                         (int)bounds.size.height };

  self->creating = TRUE;
  g_clear_pointer(&self->editing_id, g_free);

  gtk_editable_set_text(GTK_EDITABLE(self->edit_name), "");
  gtk_widget_set_visible(self->edit_url, FALSE);

  gtk_popover_set_pointing_to(GTK_POPOVER(self->edit_popover), &self->menu_target);
  gtk_popover_popup(GTK_POPOVER(self->edit_popover));
  gtk_widget_grab_focus(self->edit_name);
}

static void wig_bookmarks_dialog_edit(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigBookmarksDialog *self = user_data;
  const char *id = g_variant_get_string(parameter, NULL);

  g_autoptr(WigBookmark) bookmark = self->store ? wig_bookmarks_store_get(self->store, id, NULL) : NULL;
  if (!bookmark)
    return;

  self->creating = FALSE;
  g_set_str(&self->editing_id, id);

  gboolean is_folder = wig_bookmark_get_is_folder(bookmark);
  gtk_editable_set_text(GTK_EDITABLE(self->edit_name), wig_bookmark_get_title(bookmark));
  gtk_editable_set_text(GTK_EDITABLE(self->edit_url), wig_bookmark_get_url(bookmark));
  gtk_widget_set_visible(self->edit_url, !is_folder);
  gtk_widget_remove_css_class(self->edit_url, "error");

  gtk_popover_set_pointing_to(GTK_POPOVER(self->edit_popover), &self->menu_target);
  gtk_popover_popup(GTK_POPOVER(self->edit_popover));
}

static void wig_bookmarks_dialog_move(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigBookmarksDialog *self = user_data;
  const char *id = NULL;
  const char *folder_id = NULL;

  g_variant_get(parameter, "(&s&s)", &id, &folder_id);

  g_autoptr(GError) error = NULL;
  wig_bookmarks_store_move(self->store, id, folder_id, -1, &error);
  if (error)
    g_warning("bookmarks: could not move %s: %s", id, error->message);
}

static void wig_bookmarks_dialog_remove(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  WigBookmarksDialog *self = user_data;
  const char *id = g_variant_get_string(parameter, NULL);

  g_autoptr(GError) error = NULL;
  wig_bookmarks_store_remove(self->store, id, &error);
  if (error)
    g_warning("bookmarks: could not remove %s: %s", id, error->message);
}

static void wig_bookmarks_dialog_open_folder_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  wig_bookmarks_dialog_open_folder(user_data, g_variant_get_string(parameter, NULL));
}

static const GActionEntry bookmark_actions[] = {
  { "edit", wig_bookmarks_dialog_edit, "s" },
  { "move", wig_bookmarks_dialog_move, "(ss)" },
  { "open-folder", wig_bookmarks_dialog_open_folder_activated, "s" },
  { "remove", wig_bookmarks_dialog_remove, "s" },
};

static void wig_bookmarks_dialog_search_changed(WigBookmarksDialog *self, GtkSearchEntry *entry)
{
  const char *terms = gtk_editable_get_text(GTK_EDITABLE(entry));
  const char *query = *terms ? terms : NULL;

  if (g_strcmp0(query, self->query) == 0)
    return;

  g_set_str(&self->query, query);
  wig_bookmarks_dialog_load(self);
}

static void wig_bookmarks_dialog_row_activated(WigBookmarksDialog *self, guint position)
{
  if (position >= g_list_model_get_n_items(G_LIST_MODEL(self->items)))
    return;

  g_autoptr(WigBookmark) bookmark = g_list_model_get_item(G_LIST_MODEL(self->items), position);
  if (!bookmark)
    return;

  if (wig_bookmark_get_is_folder(bookmark))
    wig_bookmarks_dialog_open_folder(self, wig_bookmark_get_id(bookmark));
  else
    g_signal_emit(self, signals[OPEN_URI_SIGNAL], 0, wig_bookmark_get_url(bookmark), FALSE);
}

static void bookmark_row_middle_clicked(GtkGestureClick *gesture, int n_press, double x, double y,
                                        WigBookmarksDialog *self)
{
  GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  WigBookmark *bookmark = wig_bookmark_row_get_bookmark(WIG_BOOKMARK_ROW(row));

  if (!bookmark || wig_bookmark_get_is_folder(bookmark))
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_signal_emit(self, signals[OPEN_URI_SIGNAL], 0, wig_bookmark_get_url(bookmark), TRUE);
}

static void bookmark_row_right_clicked(GtkGestureClick *gesture, int n_press, double x, double y,
                                       WigBookmarksDialog *self)
{
  GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  WigBookmark *bookmark = wig_bookmark_row_get_bookmark(WIG_BOOKMARK_ROW(row));

  GtkWidget *menu = wig_bookmark_context_menu_popup(self->store, bookmark);
  if (!menu)
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  /* The edit popover outlives the row it was asked for, and rows are recycled as
   * the list scrolls, so it hangs off the dialog and is given the row's place in
   * the dialog's own coordinates to point at. */
  graphene_rect_t bounds;
  if (gtk_widget_compute_bounds(row, GTK_WIDGET(self), &bounds))
    self->menu_target = (GdkRectangle) { (int)bounds.origin.x, (int)bounds.origin.y, (int)bounds.size.width,
                                         (int)bounds.size.height };

  wig_bookmark_row_show_context_menu(WIG_BOOKMARK_ROW(row), menu, x, y);
}

static void bookmark_row_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item, WigBookmarksDialog *self)
{
  GtkWidget *row = wig_bookmark_row_new();

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_MIDDLE);
  g_signal_connect_object(click, "pressed", G_CALLBACK(bookmark_row_middle_clicked), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

  GtkGesture *secondary = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
  g_signal_connect_object(secondary, "pressed", G_CALLBACK(bookmark_row_right_clicked), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(secondary));

  GtkWidget *new_folder_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append(GTK_BOX(new_folder_content), gtk_image_new_from_icon_name("folder-new-symbolic"));
  gtk_box_append(GTK_BOX(new_folder_content), gtk_label_new("New Folder"));

  GtkWidget *new_folder = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(new_folder), new_folder_content);
  gtk_widget_add_css_class(new_folder, "flat");
  g_signal_connect_object(new_folder, "clicked", G_CALLBACK(wig_bookmarks_dialog_new_folder_clicked), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(stack), row, "bookmark");
  gtk_stack_add_named(GTK_STACK(stack), new_folder, "new-folder");
  gtk_list_item_set_child(list_item, stack);
}

static void bookmark_row_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item, WigBookmarksDialog *self)
{
  GtkStack *stack = GTK_STACK(gtk_list_item_get_child(list_item));
  gpointer item = gtk_list_item_get_item(list_item);
  gboolean is_bookmark = WIG_IS_BOOKMARK(item);

  gtk_stack_set_visible_child_name(stack, is_bookmark ? "bookmark" : "new-folder");
  gtk_list_item_set_activatable(list_item, is_bookmark);

  if (is_bookmark)
    wig_bookmark_row_set_bookmark(WIG_BOOKMARK_ROW(gtk_stack_get_child_by_name(stack, "bookmark")), item);
}

static gboolean wig_bookmarks_dialog_start_search(GtkWidget *widget, GVariant *args, gpointer user_data)
{
  WigBookmarksDialog *self = WIG_BOOKMARKS_DIALOG(widget);

  gtk_widget_grab_focus(self->entry);
  return TRUE;
}

static void wig_bookmarks_dialog_dispose(GObject *object)
{
  WigBookmarksDialog *self = WIG_BOOKMARKS_DIALOG(object);

  g_clear_pointer(&self->edit_popover, gtk_widget_unparent);
  g_clear_object(&self->items);
  g_clear_object(&self->footer);
  g_clear_pointer(&self->folder_id, g_free);
  g_clear_pointer(&self->editing_id, g_free);
  g_clear_pointer(&self->query, g_free);

  G_OBJECT_CLASS(wig_bookmarks_dialog_parent_class)->dispose(object);
}

static void wig_bookmarks_dialog_class_init(WigBookmarksDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = wig_bookmarks_dialog_dispose;

  signals[OPEN_URI_SIGNAL] = g_signal_new("open-uri", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  gtk_widget_class_add_binding(widget_class, GDK_KEY_f, GDK_CONTROL_MASK, wig_bookmarks_dialog_start_search, NULL);
}

static void wig_bookmarks_dialog_init(WigBookmarksDialog *self)
{
  self->store = wig_application_get_bookmarks_store(wig_application_get());
  self->items = g_list_store_new(WIG_TYPE_BOOKMARK);
  self->footer = g_list_store_new(G_TYPE_OBJECT);

  if (self->store)
    g_signal_connect_object(self->store, "changed", G_CALLBACK(wig_bookmarks_dialog_store_changed), self,
                            G_CONNECT_SWAPPED);

  self->breadcrumbs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  self->entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->entry, TRUE);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->entry), "Search Bookmarks");
  g_signal_connect_object(self->entry, "search-changed", G_CALLBACK(wig_bookmarks_dialog_search_changed), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(bar), self->breadcrumbs);
  gtk_box_append(GTK_BOX(bar), self->entry);
  gtk_widget_set_margin_top(bar, 6);
  gtk_widget_set_margin_bottom(bar, 6);
  gtk_widget_set_margin_start(bar, 12);
  gtk_widget_set_margin_end(bar, 12);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect_object(factory, "setup", G_CALLBACK(bookmark_row_setup), self, G_CONNECT_DEFAULT);
  g_signal_connect_object(factory, "bind", G_CALLBACK(bookmark_row_bind), self, G_CONNECT_DEFAULT);

  g_autoptr(GListStore) parts = g_list_store_new(G_TYPE_LIST_MODEL);
  g_list_store_append(parts, self->items);
  g_list_store_append(parts, self->footer);
  GtkFlattenListModel *rows = gtk_flatten_list_model_new(G_LIST_MODEL(g_object_ref(parts)));

  self->list = gtk_list_view_new(GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(rows))), factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(self->list), TRUE);
  gtk_widget_add_css_class(self->list, "rich-list");
  gtk_widget_add_css_class(self->list, "separators");
  g_signal_connect_object(self->list, "activate", G_CALLBACK(wig_bookmarks_dialog_row_activated), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), self->list);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroller, TRUE);

  self->empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty), "user-bookmarks-symbolic");

  self->stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->stack), scroller, "list");
  gtk_stack_add_named(GTK_STACK(self->stack), self->empty, "empty");

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), bar);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->stack);

  self->edit_name = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->edit_name), "Name");
  g_signal_connect_object(self->edit_name, "entry-activated", G_CALLBACK(wig_bookmarks_dialog_edit_applied), self,
                          G_CONNECT_SWAPPED);

  self->edit_url = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->edit_url), "Address");
  g_signal_connect_object(self->edit_url, "entry-activated", G_CALLBACK(wig_bookmarks_dialog_edit_applied), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *edit_rows = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(edit_rows), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(edit_rows, "boxed-list");
  gtk_list_box_append(GTK_LIST_BOX(edit_rows), self->edit_name);
  gtk_list_box_append(GTK_LIST_BOX(edit_rows), self->edit_url);

  GtkWidget *save = gtk_button_new_with_label("Save");
  gtk_widget_add_css_class(save, "suggested-action");
  gtk_widget_set_halign(save, GTK_ALIGN_END);
  g_signal_connect_object(save, "clicked", G_CALLBACK(wig_bookmarks_dialog_edit_applied), self, G_CONNECT_SWAPPED);

  GtkWidget *edit_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_size_request(edit_box, 340, -1);
  gtk_box_append(GTK_BOX(edit_box), edit_rows);
  gtk_box_append(GTK_BOX(edit_box), save);

  self->edit_popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(self->edit_popover), edit_box);
  gtk_widget_set_parent(self->edit_popover, GTK_WIDGET(self));

  g_autoptr(GSimpleActionGroup) actions = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(actions), bookmark_actions, G_N_ELEMENTS(bookmark_actions), self);
  gtk_widget_insert_action_group(GTK_WIDGET(self), "bookmark", G_ACTION_GROUP(actions));

  adw_dialog_set_title(ADW_DIALOG(self), "Bookmarks");
  adw_dialog_set_content_width(ADW_DIALOG(self), 720);
  adw_dialog_set_content_height(ADW_DIALOG(self), 560);
  adw_dialog_set_child(ADW_DIALOG(self), toolbar);
  wig_bookmarks_dialog_load(self);
}

AdwDialog *wig_bookmarks_dialog_new(void)
{
  return g_object_new(WIG_TYPE_BOOKMARKS_DIALOG, NULL);
}
