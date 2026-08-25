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

#include "wig-bookmark-context-menu.h"

/* GtkPopoverMenu reads an underscore as a mnemonic, so a title carrying one has
 * to say so twice to be drawn once. */
static char *menu_label_for_title(const char *title)
{
  g_autoptr(GString) label = g_string_new(title && *title ? title : "Untitled");

  g_string_replace(label, "_", "__", 0);
  return g_string_free(g_steal_pointer(&label), FALSE);
}

static gboolean folder_is_below(WigBookmarksStore *store, WigBookmark *folder, const char *id)
{
  g_autoptr(GPtrArray) ancestors = wig_bookmarks_store_get_ancestors(store, wig_bookmark_get_id(folder), NULL);

  for (guint i = 0; ancestors && i < ancestors->len; i++) {
    if (g_strcmp0(wig_bookmark_get_id(g_ptr_array_index(ancestors, i)), id) == 0)
      return TRUE;
  }

  return FALSE;
}

static void append_move_section(GMenu *menu, WigBookmarksStore *store, WigBookmark *bookmark)
{
  g_autoptr(GPtrArray) folders = wig_bookmarks_store_get_folders(store, NULL);
  g_autoptr(GMenu) targets = g_menu_new();
  const char *id = wig_bookmark_get_id(bookmark);
  const char *parent_id = wig_bookmark_get_parent_id(bookmark);

  if (parent_id) {
    g_autoptr(GMenuItem) top_level = g_menu_item_new("Bookmarks", NULL);
    g_menu_item_set_action_and_target(top_level, "bookmark.move", "(ss)", id, "");
    g_menu_append_item(targets, top_level);
  }

  for (guint i = 0; folders && i < folders->len; i++) {
    WigBookmark *folder = g_ptr_array_index(folders, i);
    const char *folder_id = wig_bookmark_get_id(folder);

    if (g_strcmp0(folder_id, parent_id) == 0 || g_strcmp0(folder_id, id) == 0)
      continue;

    if (wig_bookmark_get_is_folder(bookmark) && folder_is_below(store, folder, id))
      continue;

    g_autofree char *label = menu_label_for_title(wig_bookmark_get_title(folder));
    g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target(item, "bookmark.move", "(ss)", id, folder_id);
    g_menu_append_item(targets, item);
  }

  if (g_menu_model_get_n_items(G_MENU_MODEL(targets)) == 0)
    return;

  g_autoptr(GMenu) section = g_menu_new();
  g_menu_append_submenu(section, "Move to Folder", G_MENU_MODEL(targets));
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
}

static void append_delete_section(GMenu *menu, WigBookmarksStore *store, WigBookmark *bookmark)
{
  const char *id = wig_bookmark_get_id(bookmark);
  g_autoptr(GMenu) section = g_menu_new();
  g_autofree char *label = NULL;

  if (wig_bookmark_get_is_folder(bookmark)) {
    g_autoptr(GPtrArray) children = wig_bookmarks_store_get_children(store, id, NULL);
    guint n = children ? children->len : 0;

    /* Saying how much goes with the folder is the whole confirmation: there is
     * nowhere to put a dialog that would not become a window of its own. */
    label = n > 0 ? g_strdup_printf("Delete Folder and %u Item%s", n, n == 1 ? "" : "s") : g_strdup("Delete Folder");
  } else {
    label = g_strdup("Delete");
  }

  g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
  g_menu_item_set_action_and_target(item, "bookmark.remove", "s", id);
  g_menu_append_item(section, item);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
}

GtkWidget *wig_bookmark_context_menu_popup(WigBookmarksStore *store, WigBookmark *bookmark)
{
  if (!store || !bookmark || wig_bookmark_id_is_root(wig_bookmark_get_id(bookmark)))
    return NULL;

  const char *id = wig_bookmark_get_id(bookmark);
  g_autoptr(GMenu) menu = g_menu_new();

  g_autoptr(GMenu) edit_section = g_menu_new();
  g_autoptr(GMenuItem) edit_item = g_menu_item_new(wig_bookmark_get_is_folder(bookmark) ? "Rename…" : "Edit…", NULL);
  g_menu_item_set_action_and_target(edit_item, "bookmark.edit", "s", id);
  g_menu_append_item(edit_section, edit_item);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(edit_section));

  append_move_section(menu, store, bookmark);
  append_delete_section(menu, store, bookmark);

  GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_widget_set_halign(popover, GTK_ALIGN_START);

  return popover;
}
