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

#include "wig-bookmark-popover.h"

#include "wig-application.h"
#include "wig-bookmarks-store.h"

#include <adwaita.h>

struct _WigBookmarkPopover {
  GtkPopover parent;

  WigBookmarksStore *store;
  WigBookmark *bookmark;
  GListStore *folders;
  char *uri;
  char *title;
  gboolean applying;

  GtkWidget *name_row;
  GtkWidget *folder_row;
  GtkWidget *remove_button;
};

G_DEFINE_FINAL_TYPE(WigBookmarkPopover, wig_bookmark_popover, GTK_TYPE_POPOVER)

typedef enum {
  PROP_BOOKMARKED = 1,
  PROP_CAN_BOOKMARK,
} WigBookmarkPopoverProps;

static GParamSpec *props[PROP_CAN_BOOKMARK + 1];

/* The top level is where a bookmark sits when it belongs to neither fixed
 * folder, so it is offered as a destination of its own rather than as nothing. */
static void wig_bookmark_popover_fill_folders(WigBookmarkPopover *self)
{
  g_list_store_remove_all(self->folders);

  if (!self->store)
    return;

  g_autoptr(WigBookmark) top_level = wig_bookmark_new("", NULL, TRUE, "Bookmarks", "", 0, 0, 0);
  g_list_store_append(self->folders, top_level);

  g_autoptr(GPtrArray) folders = wig_bookmarks_store_get_folders(self->store, NULL);
  for (guint i = 0; folders && i < folders->len; i++)
    g_list_store_append(self->folders, g_ptr_array_index(folders, i));
}

static void wig_bookmark_popover_select_folder(WigBookmarkPopover *self, const char *parent_id)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->folders));

  for (guint i = 0; i < n; i++) {
    g_autoptr(WigBookmark) folder = g_list_model_get_item(G_LIST_MODEL(self->folders), i);
    const char *folder_id = wig_bookmark_get_id(folder);
    if (g_strcmp0(*folder_id ? folder_id : NULL, parent_id) == 0) {
      adw_combo_row_set_selected(ADW_COMBO_ROW(self->folder_row), i);
      return;
    }
  }
}

static void wig_bookmark_popover_refresh(WigBookmarkPopover *self)
{
  if (self->applying)
    return;

  g_clear_object(&self->bookmark);

  if (self->store && self->uri)
    self->bookmark = wig_bookmarks_store_find_by_url(self->store, self->uri, NULL);

  self->applying = TRUE;

  const char *name = self->bookmark ? wig_bookmark_get_title(self->bookmark) : self->title;
  gtk_editable_set_text(GTK_EDITABLE(self->name_row), name ? name : "");
  wig_bookmark_popover_select_folder(self, self->bookmark ? wig_bookmark_get_parent_id(self->bookmark) : NULL);
  gtk_widget_set_sensitive(self->remove_button, self->bookmark != NULL);

  self->applying = FALSE;

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_BOOKMARKED]);
}

static void wig_bookmark_popover_store_changed(WigBookmarkPopover *self)
{
  wig_bookmark_popover_fill_folders(self);
  wig_bookmark_popover_refresh(self);
}

static void wig_bookmark_popover_commit(WigBookmarkPopover *self)
{
  if (self->applying || !self->bookmark || !self->store)
    return;

  const char *name = gtk_editable_get_text(GTK_EDITABLE(self->name_row));
  gpointer selected_item = adw_combo_row_get_selected_item(ADW_COMBO_ROW(self->folder_row));
  g_autoptr(WigBookmark) folder = selected_item ? g_object_ref(selected_item) : NULL;
  const char *selected = folder ? wig_bookmark_get_id(folder) : NULL;
  const char *parent_id = selected && *selected ? selected : NULL;

  self->applying = TRUE;

  if (g_strcmp0(name, wig_bookmark_get_title(self->bookmark)) != 0)
    wig_bookmarks_store_update(self->store, wig_bookmark_get_id(self->bookmark), name, self->uri, NULL);

  if (folder && g_strcmp0(parent_id, wig_bookmark_get_parent_id(self->bookmark)) != 0)
    wig_bookmarks_store_move(self->store, wig_bookmark_get_id(self->bookmark), parent_id, -1, NULL);

  self->applying = FALSE;

  wig_bookmark_popover_refresh(self);
}

static void wig_bookmark_popover_remove_clicked(WigBookmarkPopover *self)
{
  if (!self->bookmark || !self->store)
    return;

  self->applying = TRUE;
  wig_bookmarks_store_remove(self->store, wig_bookmark_get_id(self->bookmark), NULL);
  self->applying = FALSE;

  wig_bookmark_popover_refresh(self);
  gtk_popover_popdown(GTK_POPOVER(self));
}

static void folder_row_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_list_item_set_child(list_item, label);
}

static void folder_row_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  WigBookmark *folder = gtk_list_item_get_item(list_item);

  gtk_label_set_label(GTK_LABEL(gtk_list_item_get_child(list_item)), wig_bookmark_get_title(folder));
}

static void wig_bookmark_popover_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  WigBookmarkPopover *self = WIG_BOOKMARK_POPOVER(object);

  switch ((WigBookmarkPopoverProps)prop_id) {
  case PROP_BOOKMARKED:
    g_value_set_boolean(value, self->bookmark != NULL);
    break;
  case PROP_CAN_BOOKMARK:
    g_value_set_boolean(value, self->store != NULL && self->uri != NULL);
    break;
  }
}

static void wig_bookmark_popover_dispose(GObject *object)
{
  WigBookmarkPopover *self = WIG_BOOKMARK_POPOVER(object);

  g_clear_object(&self->bookmark);
  g_clear_object(&self->folders);
  g_clear_pointer(&self->uri, g_free);
  g_clear_pointer(&self->title, g_free);

  G_OBJECT_CLASS(wig_bookmark_popover_parent_class)->dispose(object);
}

static void wig_bookmark_popover_class_init(WigBookmarkPopoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->get_property = wig_bookmark_popover_get_property;
  object_class->dispose = wig_bookmark_popover_dispose;

  props[PROP_BOOKMARKED] = g_param_spec_boolean("bookmarked", NULL, NULL, FALSE,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_CAN_BOOKMARK] = g_param_spec_boolean("can-bookmark", NULL, NULL, FALSE,
                                                  G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, G_N_ELEMENTS(props), props);
}

static void wig_bookmark_popover_init(WigBookmarkPopover *self)
{
  self->store = wig_application_get_bookmarks_store(wig_application_get());
  self->folders = g_list_store_new(WIG_TYPE_BOOKMARK);

  g_signal_connect(self, "closed", G_CALLBACK(wig_bookmark_popover_commit), NULL);

  if (self->store)
    g_signal_connect_object(self->store, "changed", G_CALLBACK(wig_bookmark_popover_store_changed), self,
                            G_CONNECT_SWAPPED);

  wig_bookmark_popover_fill_folders(self);

  self->name_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->name_row), "Name");

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(folder_row_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(folder_row_bind), NULL);

  self->folder_row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->folder_row), "Folder");
  adw_combo_row_set_model(ADW_COMBO_ROW(self->folder_row), G_LIST_MODEL(self->folders));
  adw_combo_row_set_factory(ADW_COMBO_ROW(self->folder_row), factory);

  GtkWidget *rows = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(rows), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(rows, "boxed-list");
  gtk_list_box_append(GTK_LIST_BOX(rows), self->name_row);
  gtk_list_box_append(GTK_LIST_BOX(rows), self->folder_row);

  self->remove_button = gtk_button_new_with_label("Remove");
  gtk_widget_add_css_class(self->remove_button, "destructive-action");
  g_signal_connect_object(self->remove_button, "clicked", G_CALLBACK(wig_bookmark_popover_remove_clicked), self,
                          G_CONNECT_SWAPPED);

  GtkWidget *done = gtk_button_new_with_label("Done");
  gtk_widget_add_css_class(done, "suggested-action");
  gtk_widget_set_hexpand(done, TRUE);
  gtk_widget_set_halign(done, GTK_ALIGN_END);
  g_signal_connect_object(done, "clicked", G_CALLBACK(gtk_popover_popdown), self, G_CONNECT_SWAPPED);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(buttons), self->remove_button);
  gtk_box_append(GTK_BOX(buttons), done);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_size_request(box, 320, -1);
  gtk_box_append(GTK_BOX(box), rows);
  gtk_box_append(GTK_BOX(box), buttons);

  gtk_popover_set_child(GTK_POPOVER(self), box);
}

GtkWidget *wig_bookmark_popover_new(void)
{
  return g_object_new(WIG_TYPE_BOOKMARK_POPOVER, NULL);
}

void wig_bookmark_popover_set_page(WigBookmarkPopover *self, const char *uri, const char *title)
{
  if (g_strcmp0(uri, self->uri) == 0 && g_strcmp0(title, self->title) == 0)
    return;

  g_set_str(&self->uri, uri);
  g_set_str(&self->title, title);

  wig_bookmark_popover_refresh(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_BOOKMARK]);
}

gboolean wig_bookmark_popover_get_bookmarked(WigBookmarkPopover *self)
{
  return self->bookmark != NULL;
}

gboolean wig_bookmark_popover_get_can_bookmark(WigBookmarkPopover *self)
{
  return self->store != NULL && self->uri != NULL;
}

void wig_bookmark_popover_ensure_bookmarked(WigBookmarkPopover *self)
{
  if (self->bookmark || !self->store || !self->uri)
    return;

  g_autoptr(GError) error = NULL;
  g_autoptr(WigBookmark) added = wig_bookmarks_store_add(self->store, NULL, self->title, self->uri, &error);

  if (error)
    g_warning("bookmarks: could not add %s: %s", self->uri, error->message);
}
