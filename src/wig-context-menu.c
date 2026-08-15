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

#include "wig-context-menu.h"

/* A misspelled word brings a guess per item plus the two spelling actions, which
 * is most of the menu by the time the usual editing entries follow. */
static gboolean is_spelling_action(WebKitContextMenuAction action)
{
  switch (action) {
  case WEBKIT_CONTEXT_MENU_ACTION_SPELLING_GUESS:
  case WEBKIT_CONTEXT_MENU_ACTION_NO_GUESSES_FOUND:
  case WEBKIT_CONTEXT_MENU_ACTION_IGNORE_SPELLING:
  case WEBKIT_CONTEXT_MENU_ACTION_LEARN_SPELLING:
  case WEBKIT_CONTEXT_MENU_ACTION_IGNORE_GRAMMAR:
    return TRUE;
  default:
    return FALSE;
  }
}

static GMenuItem *build_action_item(WebKitContextMenuItem *item, GAction *action)
{
  GMenuItem *menu_item = g_menu_item_new(webkit_context_menu_item_get_title(item), NULL);
  g_autofree char *action_name = g_strdup_printf("wpeContextMenu.%s", g_action_get_name(action));
  g_menu_item_set_action_and_target_value(menu_item, action_name, webkit_context_menu_item_get_gaction_target(item));
  return menu_item;
}

static GMenu *build_items(GList *items, GSimpleActionGroup *action_group, WebKitHitTestResult *hit_test_result,
                          const char *open_link_action, const char *open_link_label)
{
  g_autoptr(GMenu) menu = g_menu_new();
  g_autoptr(GMenu) spelling_guesses = NULL;
  g_autoptr(GMenu) spelling_actions = NULL;
  GMenu *section_menu = menu;
  for (GList *l = items; l; l = g_list_next(l)) {
    WebKitContextMenuItem *item = WEBKIT_CONTEXT_MENU_ITEM(l->data);
    if (webkit_context_menu_item_is_separator(item)) {
      g_autoptr(GMenu) section = g_menu_new();
      g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
      section_menu = section;
      continue;
    }

    if (open_link_action && webkit_context_menu_item_get_stock_action(item) == WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK
        && webkit_hit_test_result_context_is_link(hit_test_result)) {
      g_autoptr(GMenuItem) menu_item = g_menu_item_new(open_link_label, NULL);
      g_menu_item_set_action_and_target(menu_item, open_link_action, "s",
                                        webkit_hit_test_result_get_link_uri(hit_test_result));
      g_menu_append_item(section_menu, menu_item);
      continue;
    }

    GAction *action = webkit_context_menu_item_get_gaction(item);
    if (!action)
      continue;
    g_action_map_add_action(G_ACTION_MAP(action_group), action);

    WebKitContextMenuAction stock_action = webkit_context_menu_item_get_stock_action(item);
    if (is_spelling_action(stock_action)) {
      /* The submenu takes the place of the first guess, which is where WebKit
       * puts the whole block. */
      if (!spelling_guesses) {
        spelling_guesses = g_menu_new();
        spelling_actions = g_menu_new();

        g_autoptr(GMenu) spelling = g_menu_new();
        g_menu_append_section(spelling, NULL, G_MENU_MODEL(spelling_guesses));
        g_menu_append_section(spelling, NULL, G_MENU_MODEL(spelling_actions));
        g_autoptr(GMenuItem) spelling_item = g_menu_item_new_submenu("Spelling", G_MENU_MODEL(spelling));
        g_menu_append_item(section_menu, spelling_item);
      }

      gboolean is_guess = stock_action == WEBKIT_CONTEXT_MENU_ACTION_SPELLING_GUESS
          || stock_action == WEBKIT_CONTEXT_MENU_ACTION_NO_GUESSES_FOUND;
      g_autoptr(GMenuItem) guess_item = build_action_item(item, action);
      g_menu_append_item(is_guess ? spelling_guesses : spelling_actions, guess_item);
      continue;
    }

    g_autoptr(GMenuItem) menu_item = NULL;
    WebKitContextMenu *submenu = webkit_context_menu_item_get_submenu(item);
    if (submenu) {
      g_autoptr(GMenu) submenu_model = build_items(webkit_context_menu_get_items(submenu), action_group,
                                                   hit_test_result, open_link_action, open_link_label);
      menu_item = g_menu_item_new_submenu(webkit_context_menu_item_get_title(item), G_MENU_MODEL(submenu_model));
    } else {
      menu_item = build_action_item(item, action);
    }
    g_menu_append_item(section_menu, menu_item);
  }

  return g_steal_pointer(&menu);
}

GMenu *wig_context_menu_build(WebKitContextMenu *context_menu, GSimpleActionGroup *action_group,
                              WebKitHitTestResult *hit_test_result, const char *open_link_action,
                              const char *open_link_label)
{
  g_return_val_if_fail(WEBKIT_IS_CONTEXT_MENU(context_menu), NULL);
  g_return_val_if_fail(G_IS_SIMPLE_ACTION_GROUP(action_group), NULL);
  g_return_val_if_fail(WEBKIT_IS_HIT_TEST_RESULT(hit_test_result), NULL);
  g_return_val_if_fail((open_link_action == NULL) == (open_link_label == NULL), NULL);

  return build_items(webkit_context_menu_get_items(context_menu), action_group, hit_test_result, open_link_action,
                     open_link_label);
}
