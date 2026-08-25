CREATE TABLE IF NOT EXISTS bookmarks (
  id TEXT PRIMARY KEY,
  parent_id TEXT REFERENCES bookmarks(id) ON DELETE CASCADE,
  is_folder INTEGER NOT NULL DEFAULT 0,
  title TEXT NOT NULL DEFAULT '',
  url TEXT NOT NULL DEFAULT '',
  position INTEGER NOT NULL DEFAULT 0,
  date_added INTEGER NOT NULL DEFAULT 0,
  last_modified INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS bookmarks_parent_position ON bookmarks(parent_id, position);
CREATE INDEX IF NOT EXISTS bookmarks_url ON bookmarks(url) WHERE is_folder = 0;

INSERT OR IGNORE INTO bookmarks (id, parent_id, is_folder, title, position) VALUES
  ('favorites', NULL, 1, 'Favorites', 0);
