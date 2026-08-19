-- PoseStudio SQLite schema (posestudio.db). Executed on a first launch and on Factory Reset —
-- see initializeDatabase() in src/core/database.cpp, which also applies additive migrations for
-- pre-existing databases (add new columns/tables THERE as well, not only here).
--
-- Statements are split on ';' by a naive splitter, so no semicolons inside string literals or
-- trigger bodies.

CREATE TABLE Preferences(PreferenceID INTEGER PRIMARY KEY AUTOINCREMENT, PreferenceName TEXT UNIQUE NOT NULL, PreferenceValue TEXT NOT NULL DEFAULT '',PreferenceStamp DATETIME DEFAULT CURRENT_TIMESTAMP);
CREATE INDEX idx_PreferenceName ON Preferences(PreferenceName);

CREATE TABLE AssetLibraries(AssetLibraryID INTEGER PRIMARY KEY AUTOINCREMENT, AssetLibraryPath TEXT NOT NULL UNIQUE, AssetLibraryEnabled INTEGER DEFAULT 1, AssetLibraryIsBuiltIn INTEGER NOT NULL DEFAULT 0);

CREATE TABLE Favorites(FavoriteID INTEGER PRIMARY KEY AUTOINCREMENT, FavoritePath TEXT NOT NULL UNIQUE, FavoriteSortOrder INTEGER NOT NULL DEFAULT 0);

CREATE TABLE AssetCollections(AssetCollectionID INTEGER PRIMARY KEY AUTOINCREMENT, AssetCollectionName TEXT NOT NULL, AssetCollectionParentID INTEGER NOT NULL DEFAULT 0);

CREATE TABLE AssetCollectionItems(AssetCollectionItemID INTEGER PRIMARY KEY AUTOINCREMENT, AssetCollectionItemPath TEXT NOT NULL, AssetCollectionItemCol INTEGER NOT NULL DEFAULT 0, AssetCollectionItemSortOrder INTEGER NOT NULL DEFAULT 0, UNIQUE(AssetCollectionItemPath, AssetCollectionItemCol) ON CONFLICT IGNORE);
CREATE INDEX idx_AssetCollectionItemPath ON AssetCollectionItems(AssetCollectionItemPath);
CREATE INDEX idx_AssetCollectionItemCol ON AssetCollectionItems(AssetCollectionItemCol);
