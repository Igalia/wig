CREATE TABLE IF NOT EXISTS history_pages (
  id TEXT PRIMARY KEY,
  url TEXT NOT NULL UNIQUE,
  title TEXT NOT NULL DEFAULT '',
  last_visit_time INTEGER NOT NULL,
  visit_count INTEGER NOT NULL DEFAULT 0,
  typed_count INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS history_visits (
  id TEXT PRIMARY KEY,
  page_id TEXT NOT NULL REFERENCES history_pages(id) ON DELETE CASCADE,
  visit_time INTEGER NOT NULL,
  typed INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS history_visits_page_time ON history_visits(page_id, visit_time DESC);
CREATE INDEX IF NOT EXISTS history_pages_last_visit_time ON history_pages(last_visit_time DESC);

CREATE VIRTUAL TABLE IF NOT EXISTS history_pages_fts USING fts5(
  url,
  title,
  content='history_pages',
  content_rowid='rowid',
  tokenize='unicode61'
);

CREATE TRIGGER IF NOT EXISTS history_pages_ai AFTER INSERT ON history_pages BEGIN
  INSERT INTO history_pages_fts(rowid, url, title) VALUES (new.rowid, new.url, new.title);
END;

CREATE TRIGGER IF NOT EXISTS history_pages_ad AFTER DELETE ON history_pages BEGIN
  INSERT INTO history_pages_fts(history_pages_fts, rowid, url, title)
  VALUES ('delete', old.rowid, old.url, old.title);
END;

CREATE TRIGGER IF NOT EXISTS history_pages_au AFTER UPDATE OF url, title ON history_pages BEGIN
  INSERT INTO history_pages_fts(history_pages_fts, rowid, url, title)
  VALUES ('delete', old.rowid, old.url, old.title);
  INSERT INTO history_pages_fts(rowid, url, title) VALUES (new.rowid, new.url, new.title);
END;
