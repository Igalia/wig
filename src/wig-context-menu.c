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

#include "wig-utils.h"

/* Enough of the selection to recognise it without the item growing wider than
 * the rest of the menu. */
#define SELECTION_SNIPPET_MAX_CHARS 32

typedef struct {
  WebKitHitTestResult *hit_test_result;
  GMenu *image_section;
  GMenu *search_section;
  GMenuItem *inspect_item;
  gboolean has_copy_item;
} BuildContext;

static GMenu *append_new_section(GMenu *menu)
{
  GMenu *section = g_menu_new();
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  return section;
}

static void append_item_in_section(GMenu *menu, GMenuItem *item)
{
  g_autoptr(GMenu) section = append_new_section(menu);
  g_menu_append_item(section, item);
}

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

static GMenuItem *build_uri_item(const char *label, const char *action, const char *uri)
{
  GMenuItem *menu_item = g_menu_item_new(label, NULL);
  g_menu_item_set_action_and_target(menu_item, action, "s", uri);
  return menu_item;
}

/* WebKit offers a new window for anything it can open on its own; wig browses in
 * tabs, so the stock item is swapped for one aimed at the tab it would land in. */
static gboolean append_new_tab_items(const BuildContext *context, WebKitContextMenuAction stock_action, GMenu *section)
{
  WebKitHitTestResult *hit_test_result = context->hit_test_result;
  const char *subject;
  const char *uri;

  switch (stock_action) {
  case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK:
    if (!webkit_hit_test_result_context_is_link(hit_test_result))
      return FALSE;
    subject = "Link";
    uri = webkit_hit_test_result_get_link_uri(hit_test_result);
    break;
  case WEBKIT_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW:
    if (!webkit_hit_test_result_context_is_image(hit_test_result))
      return FALSE;
    subject = "Image";
    uri = webkit_hit_test_result_get_image_uri(hit_test_result);
    break;
  case WEBKIT_CONTEXT_MENU_ACTION_OPEN_VIDEO_IN_NEW_WINDOW:
  case WEBKIT_CONTEXT_MENU_ACTION_OPEN_AUDIO_IN_NEW_WINDOW:
    if (!webkit_hit_test_result_context_is_media(hit_test_result))
      return FALSE;
    subject = stock_action == WEBKIT_CONTEXT_MENU_ACTION_OPEN_VIDEO_IN_NEW_WINDOW ? "Video" : "Audio";
    uri = webkit_hit_test_result_get_media_uri(hit_test_result);
    break;
  default:
    return FALSE;
  }

  g_autofree char *new_tab_label = g_strdup_printf("Open %s in New Tab", subject);
  g_autoptr(GMenuItem) new_tab_item = build_uri_item(new_tab_label, "popup.open-in-new-tab", uri);
  g_menu_append_item(section, new_tab_item);

  g_autofree char *background_label = g_strdup_printf("Open %s in Background Tab", subject);
  g_autoptr(GMenuItem) background_item = build_uri_item(background_label, "popup.open-in-background-tab", uri);
  g_menu_append_item(section, background_item);

  return TRUE;
}

static gboolean is_image_action(WebKitContextMenuAction action)
{
  switch (action) {
  case WEBKIT_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW:
  case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK:
  case WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD:
    return TRUE;
  default:
    return FALSE;
  }
}

/* A selection is whatever the page had, newlines and runs of spaces included,
 * neither of which belongs in a menu item or in a search query. */
static char *collapse_whitespace(const char *text)
{
  g_autoptr(GString) collapsed = g_string_new(NULL);
  gboolean pending_space = FALSE;

  for (const char *p = text; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isspace(c)) {
      pending_space = collapsed->len > 0;
      continue;
    }

    if (pending_space) {
      g_string_append_c(collapsed, ' ');
      pending_space = FALSE;
    }
    g_string_append_unichar(collapsed, c);
  }

  return g_string_free(g_steal_pointer(&collapsed), FALSE);
}

static char *build_search_label(const char *engine_name, const char *terms)
{
  g_autoptr(GString) label = g_string_new(NULL);
  g_string_append_printf(label, "Search %s for “", engine_name ? engine_name : "the Web");

  if (g_utf8_strlen(terms, -1) > SELECTION_SNIPPET_MAX_CHARS) {
    const char *end = g_utf8_offset_to_pointer(terms, SELECTION_SNIPPET_MAX_CHARS);
    g_string_append_len(label, terms, end - terms);
    g_string_append(label, "…");
  } else {
    g_string_append(label, terms);
  }
  g_string_append(label, "”");

  /* GtkPopoverMenu reads the label for mnemonics. */
  g_string_replace(label, "_", "__", 0);
  return g_string_free(g_steal_pointer(&label), FALSE);
}

/* What wig adds belongs after the items WebKit built for the same thing, so it
 * waits for the section holding those to end: the image address joins the image
 * items, and the search gets the section below the one the copy item is in. */
static void flush_pending_items(BuildContext *context, GMenu *menu)
{
  if (context->image_section) {
    g_autoptr(GMenuItem) item = build_uri_item("Copy Image Address", "popup.copy-text",
                                               webkit_hit_test_result_get_image_uri(context->hit_test_result));
    g_menu_append_item(context->image_section, item);
    context->image_section = NULL;
  }

  if (context->has_copy_item && !context->search_section)
    context->search_section = append_new_section(menu);
}

static GMenu *build_items(GList *items, GSimpleActionGroup *action_group, BuildContext *context)
{
  g_autoptr(GMenu) menu = g_menu_new();
  g_autoptr(GMenu) spelling_guesses = NULL;
  g_autoptr(GMenu) spelling_actions = NULL;
  GMenu *section_menu = menu;
  for (GList *l = items; l; l = g_list_next(l)) {
    WebKitContextMenuItem *item = WEBKIT_CONTEXT_MENU_ITEM(l->data);
    if (webkit_context_menu_item_is_separator(item)) {
      flush_pending_items(context, menu);
      g_autoptr(GMenu) section = g_menu_new();
      g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
      section_menu = section;
      continue;
    }

    WebKitContextMenuAction stock_action = webkit_context_menu_item_get_stock_action(item);
    if (is_image_action(stock_action))
      context->image_section = section_menu;
    else if (stock_action == WEBKIT_CONTEXT_MENU_ACTION_COPY)
      context->has_copy_item = TRUE;

    if (append_new_tab_items(context, stock_action, section_menu))
      continue;

    GAction *action = webkit_context_menu_item_get_gaction(item);
    if (!action)
      continue;
    g_action_map_add_action(G_ACTION_MAP(action_group), action);

    if (stock_action == WEBKIT_CONTEXT_MENU_ACTION_INSPECT_ELEMENT) {
      g_clear_object(&context->inspect_item);
      context->inspect_item = build_action_item(item, action);
      continue;
    }

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
      g_autoptr(GMenu) submenu_model = build_items(webkit_context_menu_get_items(submenu), action_group, context);
      menu_item = g_menu_item_new_submenu(webkit_context_menu_item_get_title(item), G_MENU_MODEL(submenu_model));
    } else {
      menu_item = build_action_item(item, action);
    }
    g_menu_append_item(section_menu, menu_item);
  }

  return g_steal_pointer(&menu);
}

GMenu *wig_context_menu_build(WebKitContextMenu *context_menu, GSimpleActionGroup *action_group,
                              WebKitHitTestResult *hit_test_result, GMenu **search_section)
{
  BuildContext context = { .hit_test_result = hit_test_result };
  g_autoptr(GMenu) menu = build_items(webkit_context_menu_get_items(context_menu), action_group, &context);
  flush_pending_items(&context, menu);

  /* WebKit only offers a copy when the click landed on nothing else, so a
   * selection right-clicked over a link, an image, or media is left without
   * one. */
  if (!context.has_copy_item && webkit_hit_test_result_context_is_selection(hit_test_result)) {
    g_autoptr(GMenuItem) copy_item = g_menu_item_new("Copy Selected Text", "popup.copy-selection");
    append_item_in_section(menu, copy_item);
  }

  /* With no copy item to sit under, a search for the selection waits below
   * everything WebKit offered. */
  if (!context.search_section)
    context.search_section = append_new_section(menu);

  if (webkit_hit_test_result_context_is_editable(hit_test_result)) {
    g_autoptr(GMenuItem) emoji_item = g_menu_item_new("Insert Emoji…", "win.insert-emoji");
    append_item_in_section(menu, emoji_item);
  }

  /* The inspector was kept back so that it ends up below everything wig adds. */
  if (context.inspect_item) {
    g_autoptr(GMenuItem) inspect_item = g_steal_pointer(&context.inspect_item);
    append_item_in_section(menu, inspect_item);
  }

  *search_section = g_steal_pointer(&context.search_section);

  return g_steal_pointer(&menu);
}

void wig_context_menu_add_search_item(GMenu *search_section, const char *selected_text, const char *search_engine)
{
  g_autofree char *terms = collapse_whitespace(selected_text);
  if (!*terms)
    return;

  g_autofree char *engine_name = wig_util_search_engine_name(search_engine);
  g_autofree char *label = build_search_label(engine_name, terms);
  g_autofree char *uri = wig_util_search_uri(terms, search_engine);
  g_autoptr(GMenuItem) item = build_uri_item(label, "popup.open-in-new-tab", uri);

  g_menu_append_item(search_section, item);
}
