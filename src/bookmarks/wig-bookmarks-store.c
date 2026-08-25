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

#include "wig-bookmarks-store.h"

#include <errno.h>
#include <gio/gio.h>
#include <sqlite3.h>

#define WIG_BOOKMARKS_SEARCH_LIMIT 200

struct _WigBookmarksStore {
  GObject parent;

  sqlite3 *db;
};

G_DEFINE_FINAL_TYPE(WigBookmarksStore, wig_bookmarks_store, G_TYPE_OBJECT)

enum {
  CHANGED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void set_sqlite_error(GError **error, sqlite3 *db, const char *context)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", context, sqlite3_errmsg(db));
}

static gboolean exec_sql(WigBookmarksStore *self, const char *sql, GError **error)
{
  char *message = NULL;
  int rc = sqlite3_exec(self->db, sql, NULL, NULL, &message);
  if (rc != SQLITE_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", message ? message : sqlite3_errmsg(self->db));
    sqlite3_free(message);
    return FALSE;
  }
  return TRUE;
}

static sqlite3_stmt *prepare_stmt(WigBookmarksStore *self, const char *sql, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_sqlite_error(error, self->db, "bookmarks: prepare statement");
    return NULL;
  }
  return stmt;
}

static gboolean step_done(WigBookmarksStore *self, sqlite3_stmt *stmt, GError **error)
{
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    set_sqlite_error(error, self->db, "bookmarks: execute statement");
    return FALSE;
  }
  return TRUE;
}

static gboolean begin_transaction(WigBookmarksStore *self, GError **error)
{
  return exec_sql(self, "BEGIN IMMEDIATE", error);
}

static gboolean commit_transaction(WigBookmarksStore *self, GError **error)
{
  return exec_sql(self, "COMMIT", error);
}

static void rollback_transaction(WigBookmarksStore *self)
{
  sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
}

static int get_user_version(WigBookmarksStore *self, GError **error)
{
  sqlite3_stmt *stmt = prepare_stmt(self, "PRAGMA user_version", error);
  if (!stmt)
    return -1;

  int version = -1;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    version = sqlite3_column_int(stmt, 0);
  else
    set_sqlite_error(error, self->db, "bookmarks: read schema version");

  sqlite3_finalize(stmt);
  return version;
}

static gboolean exec_resource_sql(WigBookmarksStore *self, const char *resource_path, GError **error)
{
  g_autoptr(GBytes) bytes = g_resources_lookup_data(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, error);
  if (!bytes)
    return FALSE;

  gsize size;
  const char *data = g_bytes_get_data(bytes, &size);
  g_autofree char *sql = g_strndup(data, size);

  return exec_sql(self, sql, error);
}

static gboolean apply_migration(WigBookmarksStore *self, const char *resource_path, int version, GError **error)
{
  g_autofree char *pragma = g_strdup_printf("PRAGMA user_version = %d", version);

  if (!begin_transaction(self, error))
    return FALSE;

  if (!exec_resource_sql(self, resource_path, error))
    goto fail;

  if (!exec_sql(self, pragma, error))
    goto fail;

  if (!commit_transaction(self, error)) {
    rollback_transaction(self);
    return FALSE;
  }

  return TRUE;

fail:
  rollback_transaction(self);
  return FALSE;
}

static gboolean init_schema(WigBookmarksStore *self, GError **error)
{
  int version = get_user_version(self, error);
  if (version < 0)
    return FALSE;

  if (version < 1)
    return apply_migration(self, "/com/igalia/wig/bookmarks/bookmarks-schema.sql", 1, error);

  return TRUE;
}

static void wig_bookmarks_store_finalize(GObject *object)
{
  WigBookmarksStore *self = WIG_BOOKMARKS_STORE(object);

  if (self->db)
    sqlite3_close(self->db);

  G_OBJECT_CLASS(wig_bookmarks_store_parent_class)->finalize(object);
}

static void wig_bookmarks_store_class_init(WigBookmarksStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = wig_bookmarks_store_finalize;

  signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void wig_bookmarks_store_init(WigBookmarksStore *self)
{
}

WigBookmarksStore *wig_bookmarks_store_new(const char *state_dir, GError **error)
{
  if (g_mkdir_with_parents(state_dir, 0700) != 0) {
    g_set_error(error, G_IO_ERROR, (gint)g_io_error_from_errno(errno), "bookmarks: create state directory: %s",
                g_strerror(errno));
    return NULL;
  }

  g_autofree char *path = g_build_filename(state_dir, "bookmarks.sqlite", NULL);
  WigBookmarksStore *self = g_object_new(WIG_TYPE_BOOKMARKS_STORE, NULL);

  int rc = sqlite3_open_v2(path, &self->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
  if (rc != SQLITE_OK) {
    set_sqlite_error(error, self->db, "bookmarks: open database");
    g_object_unref(self);
    return NULL;
  }

  sqlite3_busy_timeout(self->db, 5000);

  sqlite3_stmt *wal_stmt = NULL;
  if (sqlite3_prepare_v2(self->db, "PRAGMA journal_mode = WAL", -1, &wal_stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(wal_stmt) == SQLITE_ROW) {
      const char *mode = (const char *)sqlite3_column_text(wal_stmt, 0);
      if (mode && !g_str_equal(mode, "wal"))
        g_warning("bookmarks: WAL mode unavailable, using '%s' journal mode", mode);
    }
    sqlite3_finalize(wal_stmt);
  }

  if (!exec_sql(self, "PRAGMA foreign_keys = ON", error) || !init_schema(self, error)) {
    g_object_unref(self);
    return NULL;
  }

  return self;
}

static void bind_text(sqlite3_stmt *stmt, int index, const char *value)
{
  sqlite3_bind_text(stmt, index, value ? value : "", -1, SQLITE_TRANSIENT);
}

static WigBookmark *bookmark_from_row(sqlite3_stmt *stmt)
{
  const char *id = (const char *)sqlite3_column_text(stmt, 0);
  const char *parent_id = (const char *)sqlite3_column_text(stmt, 1);
  gboolean is_folder = sqlite3_column_int(stmt, 2) != 0;
  const char *title = (const char *)sqlite3_column_text(stmt, 3);
  const char *url = (const char *)sqlite3_column_text(stmt, 4);
  int position = sqlite3_column_int(stmt, 5);
  gint64 date_added = sqlite3_column_int64(stmt, 6);
  gint64 last_modified = sqlite3_column_int64(stmt, 7);

  return wig_bookmark_new(id, parent_id, is_folder, title, url, position, date_added, last_modified);
}

static const char BOOKMARK_COLUMNS[] = "id, parent_id, is_folder, title, url, position, date_added, last_modified";

/* A bookmark that belongs to neither fixed folder sits at the top level, so an
 * absent parent is the top level rather than a missing value. */
static void bind_parent(sqlite3_stmt *stmt, int index, const char *parent_id)
{
  if (parent_id && *parent_id)
    bind_text(stmt, index, parent_id);
  else
    sqlite3_bind_null(stmt, index);
}

static int next_position(WigBookmarksStore *self, const char *parent_id)
{
  sqlite3_stmt *stmt = prepare_stmt(self, "SELECT COALESCE(MAX(position) + 1, 0) FROM bookmarks WHERE parent_id IS ?1",
                                    NULL);
  if (!stmt)
    return 0;

  bind_parent(stmt, 1, parent_id);

  int position = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    position = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return position;
}

static WigBookmark *bookmarks_store_insert(WigBookmarksStore *self, const char *parent_id, gboolean is_folder,
                                           const char *title, const char *url, GError **error)
{
  g_autofree char *id = g_uuid_string_random();
  gint64 now = g_get_real_time() / 1000;
  int position = next_position(self, parent_id);

  sqlite3_stmt *stmt = prepare_stmt(self,
                                    "INSERT INTO bookmarks (id, parent_id, is_folder, title, url, position, "
                                    "date_added, last_modified) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?7)",
                                    error);
  if (!stmt)
    return NULL;

  bind_text(stmt, 1, id);
  bind_parent(stmt, 2, parent_id);
  sqlite3_bind_int(stmt, 3, is_folder ? 1 : 0);
  bind_text(stmt, 4, title);
  bind_text(stmt, 5, url);
  sqlite3_bind_int(stmt, 6, position);
  sqlite3_bind_int64(stmt, 7, now);

  gboolean ok = step_done(self, stmt, error);
  sqlite3_finalize(stmt);
  if (!ok)
    return NULL;

  g_signal_emit(self, signals[CHANGED], 0);

  return wig_bookmark_new(id, parent_id, is_folder, title, url, position, now, now);
}

WigBookmark *wig_bookmarks_store_add(WigBookmarksStore *self, const char *parent_id, const char *title, const char *url,
                                     GError **error)
{
  return bookmarks_store_insert(self, parent_id, FALSE, title, url, error);
}

WigBookmark *wig_bookmarks_store_add_folder(WigBookmarksStore *self, const char *parent_id, const char *title,
                                            GError **error)
{
  return bookmarks_store_insert(self, parent_id, TRUE, title, "", error);
}

gboolean wig_bookmarks_store_update(WigBookmarksStore *self, const char *id, const char *title, const char *url,
                                    GError **error)
{
  sqlite3_stmt *stmt = prepare_stmt(self, "UPDATE bookmarks SET title = ?2, url = ?3, last_modified = ?4 WHERE id = ?1",
                                    error);
  if (!stmt)
    return FALSE;

  bind_text(stmt, 1, id);
  bind_text(stmt, 2, title);
  bind_text(stmt, 3, url);
  sqlite3_bind_int64(stmt, 4, g_get_real_time() / 1000);

  gboolean ok = step_done(self, stmt, error);
  sqlite3_finalize(stmt);

  if (ok)
    g_signal_emit(self, signals[CHANGED], 0);

  return ok;
}

static gboolean id_is_descendant_of(WigBookmarksStore *self, const char *id, const char *ancestor_id)
{
  g_autoptr(GPtrArray) ancestors = wig_bookmarks_store_get_ancestors(self, id, NULL);
  if (!ancestors)
    return FALSE;

  for (guint i = 0; i < ancestors->len; i++) {
    if (g_strcmp0(wig_bookmark_get_id(g_ptr_array_index(ancestors, i)), ancestor_id) == 0)
      return TRUE;
  }

  return FALSE;
}

gboolean wig_bookmarks_store_move(WigBookmarksStore *self, const char *id, const char *parent_id, int position,
                                  GError **error)
{
  if (wig_bookmark_id_is_root(id)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "bookmarks: '%s' cannot be moved", id);
    return FALSE;
  }

  if (g_strcmp0(id, parent_id) == 0 || id_is_descendant_of(self, parent_id, id)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "bookmarks: cannot move '%s' inside itself", id);
    return FALSE;
  }

  if (!begin_transaction(self, error))
    return FALSE;

  sqlite3_stmt *stmt = prepare_stmt(
      self, "UPDATE bookmarks SET parent_id = ?2, position = ?3, last_modified = ?4 WHERE id = ?1", error);
  if (!stmt) {
    rollback_transaction(self);
    return FALSE;
  }

  bind_text(stmt, 1, id);
  bind_parent(stmt, 2, parent_id);
  sqlite3_bind_int(stmt, 3, position < 0 ? next_position(self, parent_id) : position);
  sqlite3_bind_int64(stmt, 4, g_get_real_time() / 1000);

  gboolean ok = step_done(self, stmt, error);
  int changed = sqlite3_changes(self->db);
  sqlite3_finalize(stmt);

  if (!ok) {
    rollback_transaction(self);
    return FALSE;
  }

  if (changed == 0) {
    rollback_transaction(self);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "bookmarks: no bookmark with id '%s'", id);
    return FALSE;
  }

  if (!commit_transaction(self, error)) {
    rollback_transaction(self);
    return FALSE;
  }

  g_signal_emit(self, signals[CHANGED], 0);
  return TRUE;
}

gboolean wig_bookmarks_store_remove(WigBookmarksStore *self, const char *id, GError **error)
{
  if (wig_bookmark_id_is_root(id)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "bookmarks: '%s' cannot be removed", id);
    return FALSE;
  }

  sqlite3_stmt *stmt = prepare_stmt(self, "DELETE FROM bookmarks WHERE id = ?1", error);
  if (!stmt)
    return FALSE;

  bind_text(stmt, 1, id);

  gboolean ok = step_done(self, stmt, error);
  int changed = sqlite3_changes(self->db);
  sqlite3_finalize(stmt);

  if (!ok)
    return FALSE;

  if (changed == 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "bookmarks: no bookmark with id '%s'", id);
    return FALSE;
  }

  g_signal_emit(self, signals[CHANGED], 0);
  return TRUE;
}

static WigBookmark *bookmarks_store_query_one(WigBookmarksStore *self, const char *sql, const char *value,
                                              GError **error)
{
  sqlite3_stmt *stmt = prepare_stmt(self, sql, error);
  if (!stmt)
    return NULL;

  bind_text(stmt, 1, value);

  WigBookmark *bookmark = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    bookmark = bookmark_from_row(stmt);

  sqlite3_finalize(stmt);
  return bookmark;
}

WigBookmark *wig_bookmarks_store_get(WigBookmarksStore *self, const char *id, GError **error)
{
  g_autofree char *sql = g_strdup_printf("SELECT %s FROM bookmarks WHERE id = ?1", BOOKMARK_COLUMNS);

  return bookmarks_store_query_one(self, sql, id, error);
}

WigBookmark *wig_bookmarks_store_find_by_url(WigBookmarksStore *self, const char *url, GError **error)
{
  if (!url || !*url)
    return NULL;

  g_autofree char *sql = g_strdup_printf("SELECT %s FROM bookmarks WHERE is_folder = 0 AND url = ?1 LIMIT 1",
                                         BOOKMARK_COLUMNS);

  return bookmarks_store_query_one(self, sql, url, error);
}

static GPtrArray *bookmarks_store_collect(sqlite3_stmt *stmt)
{
  GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);

  while (sqlite3_step(stmt) == SQLITE_ROW)
    g_ptr_array_add(items, bookmark_from_row(stmt));

  return items;
}

GPtrArray *wig_bookmarks_store_get_children(WigBookmarksStore *self, const char *parent_id, GError **error)
{
  g_autofree char *sql = g_strdup_printf(
      "SELECT %s FROM bookmarks WHERE parent_id IS ?1 ORDER BY is_folder DESC, position, title", BOOKMARK_COLUMNS);

  sqlite3_stmt *stmt = prepare_stmt(self, sql, error);
  if (!stmt)
    return NULL;

  bind_parent(stmt, 1, parent_id);

  GPtrArray *items = bookmarks_store_collect(stmt);
  sqlite3_finalize(stmt);
  return items;
}

GPtrArray *wig_bookmarks_store_get_ancestors(WigBookmarksStore *self, const char *id, GError **error)
{
  static const char sql[]
      = "WITH RECURSIVE chain(id, parent_id, is_folder, title, url, position, date_added, last_modified, depth) AS ("
        "  SELECT id, parent_id, is_folder, title, url, position, date_added, last_modified, 0"
        "    FROM bookmarks WHERE id = ?1"
        "  UNION ALL"
        "  SELECT b.id, b.parent_id, b.is_folder, b.title, b.url, b.position, b.date_added, b.last_modified,"
        "         chain.depth + 1"
        "    FROM bookmarks b JOIN chain ON b.id = chain.parent_id"
        "   WHERE chain.depth < 64"
        ") SELECT id, parent_id, is_folder, title, url, position, date_added, last_modified"
        "    FROM chain WHERE depth > 0 ORDER BY depth DESC";

  sqlite3_stmt *stmt = prepare_stmt(self, sql, error);
  if (!stmt)
    return NULL;

  bind_text(stmt, 1, id);

  GPtrArray *items = bookmarks_store_collect(stmt);
  sqlite3_finalize(stmt);
  return items;
}

GPtrArray *wig_bookmarks_store_get_folders(WigBookmarksStore *self, GError **error)
{
  g_autofree char *sql = g_strdup_printf("SELECT %s FROM bookmarks WHERE is_folder = 1 ORDER BY parent_id IS NOT NULL, "
                                         "position, title",
                                         BOOKMARK_COLUMNS);

  sqlite3_stmt *stmt = prepare_stmt(self, sql, error);
  if (!stmt)
    return NULL;

  GPtrArray *items = bookmarks_store_collect(stmt);
  sqlite3_finalize(stmt);
  return items;
}

static char *search_to_like_pattern(const char *search)
{
  g_autoptr(GString) pattern = g_string_new("%");

  for (const char *c = search; *c; c++) {
    if (*c == '\\' || *c == '%' || *c == '_')
      g_string_append_c(pattern, '\\');
    g_string_append_c(pattern, *c);
  }

  g_string_append_c(pattern, '%');
  return g_string_free(g_steal_pointer(&pattern), FALSE);
}

GPtrArray *wig_bookmarks_store_search(WigBookmarksStore *self, const char *text, guint limit, GError **error)
{
  g_autofree char *sql = g_strdup_printf(
      "SELECT %s FROM bookmarks WHERE is_folder = 0 AND (title LIKE ?1 ESCAPE '\\' OR url LIKE ?1 "
      "ESCAPE '\\') ORDER BY title, url LIMIT ?2",
      BOOKMARK_COLUMNS);

  sqlite3_stmt *stmt = prepare_stmt(self, sql, error);
  if (!stmt)
    return NULL;

  g_autofree char *pattern = search_to_like_pattern(text ? text : "");
  bind_text(stmt, 1, pattern);
  sqlite3_bind_int(stmt, 2, (int)CLAMP(limit, 1, WIG_BOOKMARKS_SEARCH_LIMIT));

  GPtrArray *items = bookmarks_store_collect(stmt);
  sqlite3_finalize(stmt);
  return items;
}
