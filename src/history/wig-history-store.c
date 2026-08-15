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

#include "wig-history-store.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

struct _WigHistoryStore {
  GObject parent;

  sqlite3 *db;
};

G_DEFINE_FINAL_TYPE(WigHistoryStore, wig_history_store, G_TYPE_OBJECT)

static void set_sqlite_error(GError **error, sqlite3 *db, const char *context)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", context, sqlite3_errmsg(db));
}

static gboolean exec_sql(WigHistoryStore *self, const char *sql, GError **error)
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

static sqlite3_stmt *prepare_stmt(WigHistoryStore *self, const char *sql, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_sqlite_error(error, self->db, "history: prepare statement");
    return NULL;
  }
  return stmt;
}

static gboolean step_done(WigHistoryStore *self, sqlite3_stmt *stmt, GError **error)
{
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    set_sqlite_error(error, self->db, "history: execute statement");
    return FALSE;
  }
  return TRUE;
}

static gboolean begin_transaction(WigHistoryStore *self, GError **error)
{
  return exec_sql(self, "BEGIN IMMEDIATE", error);
}

static gboolean commit_transaction(WigHistoryStore *self, GError **error)
{
  return exec_sql(self, "COMMIT", error);
}

static void rollback_transaction(WigHistoryStore *self)
{
  sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
}

static int get_user_version(WigHistoryStore *self, GError **error)
{
  sqlite3_stmt *stmt = prepare_stmt(self, "PRAGMA user_version", error);
  if (!stmt)
    return -1;

  int version = -1;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    version = sqlite3_column_int(stmt, 0);
  else
    set_sqlite_error(error, self->db, "history: read schema version");

  sqlite3_finalize(stmt);
  return version;
}

static gboolean exec_resource_sql(WigHistoryStore *self, const char *resource_path, GError **error)
{
  g_autoptr(GBytes) bytes = g_resources_lookup_data(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, error);
  if (!bytes)
    return FALSE;

  gsize size;
  const char *data = g_bytes_get_data(bytes, &size);
  g_autofree char *sql = g_strndup(data, size);

  return exec_sql(self, sql, error);
}

static gboolean apply_migration(WigHistoryStore *self, const char *resource_path, int version, GError **error)
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

static gboolean init_schema(WigHistoryStore *self, GError **error)
{
  int version = get_user_version(self, error);
  if (version < 0)
    return FALSE;

  if (version < 1)
    return apply_migration(self, "/com/igalia/wig/history/history-schema.sql", 1, error);

  return TRUE;
}

static void wig_history_store_finalize(GObject *object)
{
  WigHistoryStore *self = WIG_HISTORY_STORE(object);

  if (self->db)
    sqlite3_close(self->db);

  G_OBJECT_CLASS(wig_history_store_parent_class)->finalize(object);
}

static void wig_history_store_class_init(WigHistoryStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = wig_history_store_finalize;
}

static void wig_history_store_init(WigHistoryStore *self)
{
}

WigHistoryStore *wig_history_store_new(const char *state_dir, GError **error)
{
  g_return_val_if_fail(state_dir != NULL, NULL);

  if (g_mkdir_with_parents(state_dir, 0700) != 0) {
    g_set_error(error, G_IO_ERROR, (gint)g_io_error_from_errno(errno), "history: create state directory: %s",
                g_strerror(errno));
    return NULL;
  }

  g_autofree char *path = g_build_filename(state_dir, "history.sqlite", NULL);
  WigHistoryStore *self = g_object_new(WIG_TYPE_HISTORY_STORE, NULL);

  int rc = sqlite3_open_v2(path, &self->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
  if (rc != SQLITE_OK) {
    set_sqlite_error(error, self->db, "history: open database");
    g_object_unref(self);
    return NULL;
  }

  sqlite3_busy_timeout(self->db, 5000);

  sqlite3_stmt *wal_stmt = NULL;
  if (sqlite3_prepare_v2(self->db, "PRAGMA journal_mode = WAL", -1, &wal_stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(wal_stmt) == SQLITE_ROW) {
      const char *mode = (const char *)sqlite3_column_text(wal_stmt, 0);
      if (mode && !g_str_equal(mode, "wal"))
        g_warning("history: WAL mode unavailable, using '%s' journal mode", mode);
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

static char *lookup_page_id(WigHistoryStore *self, const char *url, GError **error)
{
  sqlite3_stmt *stmt = prepare_stmt(self, "SELECT id FROM history_pages WHERE url = ?", error);
  if (!stmt)
    return NULL;

  bind_text(stmt, 1, url);
  int rc = sqlite3_step(stmt);
  char *id = NULL;
  if (rc == SQLITE_ROW)
    id = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  else if (rc == SQLITE_DONE)
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "history: page not found after upsert");
  else
    set_sqlite_error(error, self->db, "history: lookup page");

  sqlite3_finalize(stmt);
  return id;
}

void wig_history_store_record_visit(WigHistoryStore *self, const char *url, const char *title, gboolean typed,
                                    gint64 visit_time, GError **error)
{
  g_return_if_fail(WIG_IS_HISTORY_STORE(self));
  g_return_if_fail(url != NULL);

  g_autofree char *page_id = g_uuid_string_random();
  g_autofree char *stored_page_id = NULL;
  g_autofree char *visit_id = NULL;

  if (!begin_transaction(self, error))
    return;

  sqlite3_stmt *stmt = prepare_stmt(
      self,
      "INSERT INTO history_pages(id, url, title, last_visit_time, visit_count, typed_count) "
      "VALUES (?, ?, ?, ?, 1, ?) "
      "ON CONFLICT(url) DO UPDATE SET "
      "  title = CASE WHEN excluded.title != '' THEN excluded.title ELSE history_pages.title END, "
      "  last_visit_time = excluded.last_visit_time, "
      "  visit_count = history_pages.visit_count + 1, "
      "  typed_count = history_pages.typed_count + excluded.typed_count",
      error);
  if (!stmt)
    goto fail;

  bind_text(stmt, 1, page_id);
  bind_text(stmt, 2, url);
  bind_text(stmt, 3, title);
  sqlite3_bind_int64(stmt, 4, visit_time);
  sqlite3_bind_int(stmt, 5, typed ? 1 : 0);
  if (!step_done(self, stmt, error)) {
    sqlite3_finalize(stmt);
    goto fail;
  }
  sqlite3_finalize(stmt);

  stored_page_id = lookup_page_id(self, url, error);
  if (!stored_page_id)
    goto fail;

  visit_id = g_uuid_string_random();
  stmt = prepare_stmt(self, "INSERT INTO history_visits(id, page_id, visit_time, typed) VALUES (?, ?, ?, ?)", error);
  if (!stmt)
    goto fail;

  bind_text(stmt, 1, visit_id);
  bind_text(stmt, 2, stored_page_id);
  sqlite3_bind_int64(stmt, 3, visit_time);
  sqlite3_bind_int(stmt, 4, typed ? 1 : 0);
  if (!step_done(self, stmt, error)) {
    sqlite3_finalize(stmt);
    goto fail;
  }
  sqlite3_finalize(stmt);

  if (!commit_transaction(self, error))
    rollback_transaction(self);
  return;

fail:
  rollback_transaction(self);
}

void wig_history_store_update_title(WigHistoryStore *self, const char *url, const char *title, GError **error)
{
  g_return_if_fail(WIG_IS_HISTORY_STORE(self));
  g_return_if_fail(url != NULL);

  if (!title || !*title)
    return;

  sqlite3_stmt *stmt = prepare_stmt(self, "UPDATE history_pages SET title = ? WHERE url = ?", error);
  if (!stmt)
    return;

  bind_text(stmt, 1, title);
  bind_text(stmt, 2, url);
  step_done(self, stmt, error);
  sqlite3_finalize(stmt);
}

static char *search_to_fts_query(const char *search)
{
  if (!search || !*search)
    return NULL;

  g_autoptr(GString) query = g_string_new(NULL);
  const char *p = search;
  while (*p) {
    gunichar ch = g_utf8_get_char(p);
    if (g_unichar_isalnum(ch)) {
      if (query->len > 0)
        g_string_append_c(query, ' ');
      while (*p) {
        ch = g_utf8_get_char(p);
        if (!g_unichar_isalnum(ch))
          break;
        g_string_append_unichar(query, g_unichar_tolower(ch));
        p = g_utf8_next_char(p);
      }
      g_string_append_c(query, '*');
      continue;
    }
    p = g_utf8_next_char(p);
  }

  if (query->len == 0)
    return NULL;

  return g_string_free_and_steal(g_steal_pointer(&query));
}

static char *search_to_like_pattern(const char *search)
{
  if (!search || !*search)
    return NULL;

  g_autoptr(GString) pattern = g_string_new("%");
  const char *p = search;
  while (*p) {
    gunichar ch = g_utf8_get_char(p);
    if (ch == '\\' || ch == '%' || ch == '_')
      g_string_append_c(pattern, '\\');
    g_string_append_unichar(pattern, ch);
    p = g_utf8_next_char(p);
  }
  g_string_append_c(pattern, '%');

  return g_string_free_and_steal(g_steal_pointer(&pattern));
}

static WigHistoryItem *item_from_stmt(sqlite3_stmt *stmt)
{
  const char *id = (const char *)sqlite3_column_text(stmt, 0);
  const char *url = (const char *)sqlite3_column_text(stmt, 1);
  const char *title = (const char *)sqlite3_column_text(stmt, 2);
  gint64 last_visit_time = sqlite3_column_int64(stmt, 3);
  guint visit_count = (guint)sqlite3_column_int(stmt, 4);
  guint typed_count = (guint)sqlite3_column_int(stmt, 5);

  return wig_history_item_new(id, url, title, last_visit_time, visit_count, typed_count);
}

GPtrArray *wig_history_store_query(WigHistoryStore *self, const char *search, gint64 before_time, guint limit,
                                   gboolean *has_more, GError **error)
{
  g_return_val_if_fail(WIG_IS_HISTORY_STORE(self), NULL);

  if (has_more)
    *has_more = FALSE;

  g_autofree char *fts_query = search_to_fts_query(search);
  g_autofree char *like_pattern = search_to_like_pattern(search);
  const char *sql = fts_query ? "SELECT h.id, h.url, h.title, h.last_visit_time, h.visit_count, h.typed_count "
                                "FROM ("
                                "  SELECT rowid, MIN(source) AS source, MIN(rank) AS rank FROM ("
                                "    SELECT h.rowid, 0 AS source, bm25(history_pages_fts) AS rank "
                                "    FROM history_pages_fts "
                                "    JOIN history_pages h ON h.rowid = history_pages_fts.rowid "
                                "    WHERE history_pages_fts MATCH ? AND (? = 0 OR h.last_visit_time < ?) "
                                "    UNION ALL "
                                "    SELECT rowid, 1 AS source, 0.0 AS rank FROM history_pages "
                                "    WHERE (url LIKE ? ESCAPE '\\' OR title LIKE ? ESCAPE '\\') "
                                "      AND (? = 0 OR last_visit_time < ?) "
                                "  ) GROUP BY rowid"
                                ") AS matches "
                                "JOIN history_pages h ON h.rowid = matches.rowid "
                                "ORDER BY matches.source, matches.rank, h.typed_count DESC, h.visit_count DESC, "
                                "h.last_visit_time DESC LIMIT ?"
                              : "SELECT id, url, title, last_visit_time, visit_count, typed_count "
                                "FROM history_pages WHERE (? = 0 OR last_visit_time < ?) "
                                "ORDER BY last_visit_time DESC LIMIT ?";

  sqlite3_stmt *stmt = prepare_stmt(self, sql, error);
  if (!stmt)
    return NULL;

  guint bounded_limit = CLAMP(limit, 1, 500);
  guint fetch_limit = bounded_limit + 1;
  if (fts_query) {
    bind_text(stmt, 1, fts_query);
    sqlite3_bind_int64(stmt, 2, before_time);
    sqlite3_bind_int64(stmt, 3, before_time);
    bind_text(stmt, 4, like_pattern);
    bind_text(stmt, 5, like_pattern);
    sqlite3_bind_int64(stmt, 6, before_time);
    sqlite3_bind_int64(stmt, 7, before_time);
    sqlite3_bind_int(stmt, 8, (int)fetch_limit);
  } else {
    sqlite3_bind_int64(stmt, 1, before_time);
    sqlite3_bind_int64(stmt, 2, before_time);
    sqlite3_bind_int(stmt, 3, (int)fetch_limit);
  }

  GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (items->len == bounded_limit) {
      if (has_more)
        *has_more = TRUE;
      break;
    }
    g_ptr_array_add(items, item_from_stmt(stmt));
  }

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    set_sqlite_error(error, self->db, "history: query");
    g_clear_pointer(&items, g_ptr_array_unref);
  }

  sqlite3_finalize(stmt);
  return items;
}