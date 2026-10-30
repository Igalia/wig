CREATE TABLE IF NOT EXISTS history_web_apps (
  id INTEGER PRIMARY KEY,
  app_id TEXT NOT NULL UNIQUE
);

CREATE TABLE history_pages_new (
  id TEXT PRIMARY KEY,
  url TEXT NOT NULL,
  web_app INTEGER REFERENCES history_web_apps(id),
  title TEXT NOT NULL DEFAULT '',
  last_visit_time INTEGER NOT NULL,
  visit_count INTEGER NOT NULL DEFAULT 0,
  typed_count INTEGER NOT NULL DEFAULT 0
);

INSERT INTO history_pages_new(rowid, id, url, title, last_visit_time, visit_count, typed_count)
  SELECT rowid, id, url, title, last_visit_time, visit_count, typed_count FROM history_pages;

DROP TABLE history_pages;
ALTER TABLE history_pages_new RENAME TO history_pages;

CREATE UNIQUE INDEX history_pages_url_web_app ON history_pages(url, IFNULL(web_app, 0));
CREATE INDEX history_pages_web_app_last_visit_time ON history_pages(web_app, last_visit_time DESC);

CREATE TRIGGER history_pages_ai AFTER INSERT ON history_pages BEGIN
  INSERT INTO history_pages_fts(rowid, url, title) VALUES (new.rowid, new.url, new.title);
END;

CREATE TRIGGER history_pages_ad AFTER DELETE ON history_pages BEGIN
  INSERT INTO history_pages_fts(history_pages_fts, rowid, url, title)
  VALUES ('delete', old.rowid, old.url, old.title);
END;

CREATE TRIGGER history_pages_au AFTER UPDATE OF url, title ON history_pages BEGIN
  INSERT INTO history_pages_fts(history_pages_fts, rowid, url, title)
  VALUES ('delete', old.rowid, old.url, old.title);
  INSERT INTO history_pages_fts(rowid, url, title) VALUES (new.rowid, new.url, new.title);
END;

INSERT INTO history_pages_fts(history_pages_fts) VALUES ('rebuild');
