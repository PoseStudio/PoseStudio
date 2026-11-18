-- PoseStudio SQLite schema (posestudio.db). Executed on EVERY launch by initializeDatabase() in
-- src/core/database.cpp, so every statement must be CREATE ... IF NOT EXISTS: a new table or
-- index added here reaches existing databases by itself. A new COLUMN of an existing table does
-- not (the table is there already): add it to ensureSchemaCurrent() in the same file as well.
--
-- Statements are split on a semicolon by a naive splitter, so keep semicolons out of string
-- literals and trigger bodies.

CREATE TABLE IF NOT EXISTS Preferences(PreferenceID INTEGER PRIMARY KEY AUTOINCREMENT, PreferenceName TEXT UNIQUE NOT NULL, PreferenceValue TEXT NOT NULL DEFAULT '',PreferenceStamp DATETIME DEFAULT CURRENT_TIMESTAMP);
CREATE INDEX IF NOT EXISTS idx_PreferenceName ON Preferences(PreferenceName);

CREATE TABLE IF NOT EXISTS AssetLibraries(AssetLibraryID INTEGER PRIMARY KEY AUTOINCREMENT, AssetLibraryPath TEXT NOT NULL UNIQUE, AssetLibraryEnabled INTEGER DEFAULT 1, AssetLibraryIsBuiltIn INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS Favorites(FavoriteID INTEGER PRIMARY KEY AUTOINCREMENT, FavoritePath TEXT NOT NULL UNIQUE, FavoriteSortOrder INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS AssetCollections(AssetCollectionID INTEGER PRIMARY KEY AUTOINCREMENT, AssetCollectionName TEXT NOT NULL, AssetCollectionParentID INTEGER NOT NULL DEFAULT 0, AssetCollectionScope TEXT NOT NULL DEFAULT '');

CREATE TABLE IF NOT EXISTS AssetCollectionItems(AssetCollectionItemID INTEGER PRIMARY KEY AUTOINCREMENT, AssetCollectionItemPath TEXT NOT NULL, AssetCollectionItemCol INTEGER NOT NULL DEFAULT 0, AssetCollectionItemSortOrder INTEGER NOT NULL DEFAULT 0, UNIQUE(AssetCollectionItemPath, AssetCollectionItemCol) ON CONFLICT IGNORE);
CREATE INDEX IF NOT EXISTS idx_AssetCollectionItemPath ON AssetCollectionItems(AssetCollectionItemPath);
CREATE INDEX IF NOT EXISTS idx_AssetCollectionItemCol ON AssetCollectionItems(AssetCollectionItemCol);
