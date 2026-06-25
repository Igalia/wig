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

#include "wig-tab-context-menu.h"

GtkWidget *wig_tab_context_menu_popup(WigTabList *list, WigTab *tab)
{
  g_return_val_if_fail(WIG_IS_TAB_LIST(list), NULL);
  g_return_val_if_fail(WIG_IS_TAB(tab), NULL);

  guint tab_id = wig_tab_get_id(tab);
  guint pos = wig_tab_list_index_of(list, tab);
  guint n_tabs = wig_tab_list_get_n_tabs(list);
  gboolean has_left = pos > 0;
  gboolean has_right = pos < n_tabs - 1;
  gboolean has_others = n_tabs > 1;

  GSimpleActionGroup *group = wig_tab_list_get_action_group(list);
  g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(group), "close-to-left")),
                              has_left);
  g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(group), "close-to-right")),
                              has_right);
  g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(group), "close-others")),
                              has_others);

#define APPEND_TAB_ITEM(menu_, label_, action_)                                                                        \
  G_STMT_START                                                                                                         \
  {                                                                                                                    \
    GMenuItem *_item = g_menu_item_new((label_), NULL);                                                                \
    g_menu_item_set_action_and_target(_item, "tabs." action_, "u", tab_id);                                            \
    g_menu_append_item((menu_), _item);                                                                                \
    g_object_unref(_item);                                                                                             \
  }                                                                                                                    \
  G_STMT_END

  g_autoptr(GMenu) menu = g_menu_new();

  g_autoptr(GMenu) section1 = g_menu_new();
  APPEND_TAB_ITEM(section1, "Reload Tab", "reload");
  APPEND_TAB_ITEM(section1, "Mute Tab", "mute");
  APPEND_TAB_ITEM(section1, "Duplicate Tab", "duplicate");
  APPEND_TAB_ITEM(section1, "Copy Link", "copy-link");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section1));

  g_autoptr(GMenu) section2 = g_menu_new();
  APPEND_TAB_ITEM(section2, "Close Tab", "close");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section2));

  g_autoptr(GMenu) close_multiple = g_menu_new();
  APPEND_TAB_ITEM(close_multiple, "Close Tabs to Left", "close-to-left");
  APPEND_TAB_ITEM(close_multiple, "Close Other Tabs", "close-others");
  APPEND_TAB_ITEM(close_multiple, "Close Tabs to Right", "close-to-right");

  g_autoptr(GMenu) section3 = g_menu_new();
  g_autoptr(GMenuItem) submenu_item = g_menu_item_new_submenu("Close Multiple Tabs", G_MENU_MODEL(close_multiple));

  /* Attach a dedicated action so the submenu header itself can be insensitive. */
  g_autoptr(GSimpleAction) open_multiple = g_simple_action_new("open-close-multiple", NULL);
  g_simple_action_set_enabled(open_multiple, has_others);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(open_multiple));
  g_menu_item_set_attribute(submenu_item, "action", "s", "tab.open-close-multiple");

  g_menu_append_item(section3, submenu_item);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section3));

  GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

  return popover;
}
