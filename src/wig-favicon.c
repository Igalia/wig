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

#include "wig-favicon.h"

#if HAVE_FAVICON_SUPPORT
#include "wig-utils.h"

typedef struct {
  char *page_uri;
  int size;
} FaviconRequest;

static void favicon_request_free(FaviconRequest *request)
{
  g_clear_pointer(&request->page_uri, g_free);
  g_free(request);
}

static void favicon_loaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = G_TASK(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(WebKitImageList) images = webkit_favicon_database_get_page_icons_finish(WEBKIT_FAVICON_DATABASE(source),
                                                                                    result, &error);
  if (!images) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  FaviconRequest *request = g_task_get_task_data(task);
  GIcon *icon = wig_util_best_page_icon(images, request->size);
  if (!icon) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No favicon is stored for %s", request->page_uri);
    return;
  }

  g_task_return_pointer(task, g_object_ref(icon), g_object_unref);
}

void wig_favicon_get_async(WebKitFaviconDatabase *database, const char *page_uri, int size, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(WEBKIT_IS_FAVICON_DATABASE(database));
  g_return_if_fail(page_uri && *page_uri);
  g_return_if_fail(size > 0);

  g_autoptr(GTask) task = g_task_new(database, cancellable, callback, user_data);
  g_task_set_source_tag(task, wig_favicon_get_async);
  FaviconRequest *request = g_new(FaviconRequest, 1);
  request->page_uri = g_strdup(page_uri);
  request->size = size;
  g_task_set_task_data(task, request, (GDestroyNotify)favicon_request_free);
  webkit_favicon_database_get_page_icons(database, page_uri, cancellable, favicon_loaded, g_steal_pointer(&task));
}

GIcon *wig_favicon_get_finish(WebKitFaviconDatabase *database, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, database), NULL);
  g_return_val_if_fail(g_async_result_is_tagged(result, wig_favicon_get_async), NULL);

  return g_task_propagate_pointer(G_TASK(result), error);
}
#endif
