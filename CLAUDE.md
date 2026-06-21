# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

PoseStudio is an early-stage, open-source Qt6/C++ desktop application for 3D character creation, posing, and scene building (think a free alternative to DAZ Studio). The codebase is currently a small skeleton: a main window shell, an asset manager side panel backed by SQLite, and a menu bar. The 3D viewport, rigging, animation, and rendering systems described in README.md are aspirational/roadmap — they don't exist in code yet.

## Build

Requires Qt 6 (developed against 6.11.1, MSVC 2022 64-bit) and CMake 3.16+.

```
cmake -B build -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/msvc2022_64"
cmake --build build
```

If `CMAKE_PREFIX_PATH` isn't passed, `CMakeLists.txt` falls back to the hardcoded path `C:/Qt/6.11.1/msvc2022_64` — never commit a change to that fallback to point at your personal Qt install.

The build links `Qt6::Widgets` and `Qt6::Sql` only (no Qt Network/3D/Quick modules yet). `CMAKE_AUTOMOC`/`AUTOUIC`/`AUTORCC` are all on, so new `Q_OBJECT` classes, `.ui` files, and resources just need to be added to the `add_executable` source list / `resources.qrc` — no manual moc/rcc invocation.

There is no test suite, linter, or CI configuration in this repo yet.

## Architecture

**Startup sequence matters** (`src/main.cpp`): the SQLite database is opened and `PreferencesManager` is loaded *before* any widget is constructed, because widgets (e.g. `AssetManagerWidget`) read preferences/DB state during construction. If you add a new manager/cache that UI depends on, initialize it in this same early block, not lazily inside a widget.

**Database** (`src/database.{h,cpp}`): single named QSqlDatabase connection (`"db_conn"`) shared process-wide via `QSqlDatabase::database("db_conn")` rather than opened per-call. The SQLite file lives next to the executable (`posestudio.db`), not in the working directory or a user-profile path. Schema lives in `resources/database/initialize.sql`, embedded into the binary via `resources.qrc` and only fully executed on `DbInitMode::FactoryReset` (which also deletes the existing file first, and is reachable from the app via Help → Factory Reset Database). There is still no formal migration system, but additive schema changes for existing databases (new columns/tables) are applied unconditionally at the end of `initializeDatabase()`, each guarded to be a no-op once already present (e.g. a `PRAGMA table_info` check before `ALTER TABLE ADD COLUMN`, or `CREATE TABLE/INDEX IF NOT EXISTS`) — add new ones here rather than only in `initialize.sql`, or pre-existing installs will silently fail at the first query touching the new column/table.

**Preferences** (`src/preferencesmanager.{h,cpp}`): a process-wide singleton (`PreferencesManager::instance()`) that loads the entire `Preferences` SQLite table into a `QHash` once at startup. Reads never touch the DB; writes update the cache and persist immediately. It is explicitly not thread-safe — only call it from the Qt GUI thread.

**Asset manager** (`src/assetmanagerwidget.{h,cpp}`, the largest file in the project): the side-panel widget that browses physical asset library folders (paths stored in `AssetLibraries`) alongside virtual, user-defined "Collections" (`AssetCollections`/`AssetCollectionItems`/`AssetCollectionFolders` tables). Collections can nest arbitrarily deep via `AssetCollections.AssetCollectionParentID` (`0` = top-level) — any collection, at any depth, can hold its own flat asset items and folder shortcuts alongside further sub-collections, mirroring filesystem folder semantics. `contextForTreeItem()` walks a tree item's ancestor chain to classify it as `Library`/`Collection`/`SearchResults` regardless of nesting depth; prefer it over checking a node's immediate parent when context-menu or grid logic needs to know which "world" an item belongs to. Key pieces:
- `AssetFolderProxyModel` wraps the filesystem `QStandardItemModel`-backed tree to mark folders as direct/indirect "hits" (i.e. contain or descend into a folder containing matching assets) for search/filtering, with results debounced via a pending-index queue rather than recomputed synchronously.
- `AssetTreeDelegate` / `AssetGridDelegate` fully override Qt's default painting for the directory tree and the thumbnail grid respectively — don't expect standard `QStyledItemDelegate` behavior; geometry constants they rely on (icon size, margins, gaps) are centralized in `src/constants.h` so the two delegates' size hints can't drift apart.
- Breadcrumb rendering is cached per `displayFolder()` call (`m_breadcrumb*` members) and only re-laid-out, not recomputed, on label resize.
- Physical folders and DB-backed virtual Collections are both rendered into the same `QTreeView`/`dirModel`, distinguished by `searchResultsRootItem`/`collectionsRootItem` root nodes — when adding tree behavior, check which subtree (filesystem vs. virtual) an index belongs to before assuming filesystem semantics (e.g. rename-on-disk) apply.

**Styling**: dark theme enforced via `QApplication::setStyle("Fusion")` plus hand-written QSS, never the native OS style. Stylesheets are modular (`global.qss`, `_assetmanager.qss`, `_menumanager.qss`) and concatenated in `loadStylesheets()` (`src/main.cpp`) in a fixed cascade order — later files override earlier ones, so module-specific overrides belong in the `_*.qss` files, not `global.qss`. Some custom widgets (e.g. `AssetTreeView`) expose `Q_PROPERTY` values (like `separatorColor`) specifically so QSS can drive C++ painting logic. `QDialog`/`QMessageBox` are dark-themed globally too — but note Qt's QSS engine doesn't apply selectors (e.g. `a { color: ... }`) to rich-text `<a>` links inside a `QLabel`; those need the color set inline in the HTML string itself (see the "Add Asset Folder" hint label in `assetmanagerwidget.cpp` for the pattern).

**Destructive operations** (e.g. Help → Factory Reset Database, in `menumanager.cpp`): rather than trying to reconcile every widget's in-memory state (the asset tree, `PreferencesManager`'s cache, etc.) after wiping the database, the app just relaunches itself via `QProcess::startDetached(QApplication::applicationFilePath(), ...)` followed by `QApplication::quit()`. Follow this pattern for any future action that invalidates broad in-memory state rather than writing ad-hoc refresh logic for every affected widget.

**Constants** (`src/constants.h`): shared UI dimensions, colors, and timing values live here rather than being duplicated as magic numbers across widgets/delegates — add new shared layout/color/timing constants here instead of inlining them.
