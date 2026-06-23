# Changelog

All notable changes to the PoseStudio project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.3] - 2026-06-23

### Added
- **Manual sort order for Collections:** Assets inside a Collection can now be drag-reordered, the same way Favorites already worked. Added an `AssetCollectionItemSortOrder` column to `AssetCollectionItems` (with a migration for existing databases, seeded from insertion order), and collections now load in that manual order instead of always alphabetically. New items added to a collection append to the end of its order; the info bar's "Sortable" label now also appears while browsing a Collection.

### Changed
- **Generalized the drag-reorder mechanism:** The hand-rolled grid drag-reorder code (ghost thumbnail, drop-line indicator, edge auto-scroll) was Favorites-only; it's now shared between Favorites and Collections behind a single `isSortableView()` check, with the relevant methods/members renamed from Favorites-specific names (`beginFavoritesDrag`, etc.) to generic ones (`beginGridDrag`, etc.).

## [0.1.2] - 2026-06-21

### Added
- **Find In Library / Browse Folder for Collections & Search Results:** Folders and assets shown anywhere under Collections or Search Results (at any nesting depth) now surface "Find In Library" and "Browse Folder" at the top of their context menus, jumping to the item's real location in the physical library tree. Added a new `tree.png` icon for the action.
- **Nested Sub-Collections:** Collections can now contain other Collections, arbitrarily deep (e.g. `Characters > Female`, `Clothing > Shirts`). Any collection — top-level or nested — can hold its own asset items and folder shortcuts alongside further sub-collections. Added a "New Sub-Collection" action to each collection's context menu, and a "New Collection" action to the Collections root's context menu (`add-col.png`).
- **Full-Path Collection Picker:** "Add/Move/Copy To Collection" menus now list every collection by its full path (e.g. "Clothing / Shirts") instead of a bare name, so any nested collection is selectable directly from a flat list.
- **Factory Reset Database menu action:** Added Help → "Factory Reset Database..." with a confirmation dialog; on confirm it wipes and rebuilds `posestudio.db` from `initialize.sql` and relaunches the app.
- **"Add Asset Folder" empty-state link:** When no asset library is configured, a clickable "Add Asset Folder" link appears in the directory tree panel, opening a native folder picker and registering the chosen folder as a new library.
- **`sub-collection.png` icon:** All Collection nodes (top-level and nested) now render with a dedicated icon instead of sharing the physical-folder hit-state icons.

### Changed
- **Collection name uniqueness is now per-parent, not global:** "New Collection"/"New Sub-Collection" actions auto-generate a unique sibling name ("New Collection (2)", etc.) instead of silently reusing an existing collection of the same name under the same parent.
- **Dialog styling:** `QMessageBox`/`QDialog` now follow the app's dark theme (background, text color, button styling) instead of the default light Fusion appearance.
- **All icons replaced with Lucide Icons:** Every icon in `resources/icons/` has been re-sourced from the [Lucide](https://lucide.dev/) icon library for a more consistent visual language. Lucide will be the default icon source for new icons going forward.

### Fixed
- **Sub-collection renames not persisting:** The rename handler only wrote to the database for top-level collections; renaming a nested sub-collection updated the tree but reverted on restart. It now keys off the collection's own ID regardless of nesting depth.
- **Adding a folder to a Collection not persisting:** Root cause was that the `AssetCollectionFolders` table itself didn't exist in databases created before that table was added to the schema, so every insert was silently failing at the SQL `prepare()` step. Added a migration that creates the table (and its indexes) if missing.
- **Missing `AssetCollectionParentID` migration for existing databases:** Added an idempotent `ALTER TABLE` so existing installs gain the column needed for nested collections without requiring a factory reset.

### Removed
- **Unused icons:** Removed `favorites.png` and `favorite-item.png` (leftover from the removed Favorites feature) from `resources.qrc` and disk.

### Internal
- Added `CLAUDE.md`, project guidance for future Claude Code sessions covering build commands and architecture.

## [0.1.1] - 2026-06-19

### Added
- **Clickable Breadcrumb Navigation:** The asset panel's folder header is a real breadcrumb again — ancestor segments are clickable links (with hover underline) that jump straight to that folder. Segments are fit dynamically to the available header width, fitting as many trailing folders as possible and collapsing whatever doesn't fit (including the library name) into a single leading "...".
- **Folder Context Menu on Breadcrumb:** Right-clicking a breadcrumb segment (or the current folder) now offers "Open" and "Browse Folder", targeting whichever link is under the cursor.
- **Search Result De-duplication:** A search result that is itself a subfolder of another result already in the list is dropped, since it's already reachable by expanding that ancestor in the tree.
- **Search Result Path Truncation:** Long search result paths are capped to their last 4 segments, prefixed with "..." instead of showing the full path.
- **Folder Context Menu in Asset Grid:** Right-clicking a subfolder shown in the asset grid now offers Open, Add To Collection, Browse Folder, and Refresh — matching the functionality already available for individual assets.

### Changed
- **Breadcrumb Header Never Resizes the Window:** The folder header label no longer grows to fit long paths; it elides instead, so dragging the splitter or resizing the window is no longer fought by the label's content.
- **Tree Selection Reflects Current Folder:** Navigating away from a folder via a breadcrumb link, or by opening a subfolder from the asset grid, now clears the tree's selection/highlight instead of leaving the previous folder looking selected.
- **Two-Line Asset Labels:** Asset and folder names in the grid now word-wrap to two lines (eliding the second line with "..." if still too long) instead of being cut off after one line, with a deliberate, consistently-sized gap between the thumbnail and its label.
- **Asset Grid Row Spacing:** Added breathing room between grid rows via `QListWidget::setSpacing`.

### Removed
- **Combined View:** Removed the virtual "Combined View" tree node that aggregated subdirectories across all enabled asset libraries (`COMBINED_ROOT` / `COMBINED_DIR_*`). Folder navigation, search, and "Go to Folder" now operate solely against each library's own physical directory tree. Collections folder shortcuts pointing at virtual Combined View paths are no longer supported.

### Fixed
- **Grid Cell Sizing Drift:** `AssetGridDelegate`'s cell-size calculation and its actual paint logic had drifted out of sync (1.5 vs. 2 lines of text height), intermittently compressing the gap between a thumbnail and its label. The two are now derived from the same formula.

### Internal
- Project-wide cleanup pass: removed dead code left over from the now-deleted Favorites feature and other unused constants/functions, deduplicated repeated context-menu-building and icon-loading logic into shared helpers, moved a fully-inlined tree delegate implementation out of the header to restore the project's intended fast-compile structure, and rewrote stale or inaccurate comments throughout.

## [0.1.0] - 2026-06-16

### Added
- **Asset Search:** Added a persistent search bar (QLineEdit + clear button + search button) above the Collections node in the tree panel. Searching scans all enabled asset library directories recursively and populates a dedicated "Search Results" root node.
- **Search Results Tree:** Results are displayed as a proper navigable hierarchy — shared ancestor folders are merged so the full path context is preserved, not compacted into a single flat string.
- **Full Subtree Browsing from Search:** Matched folders in the search results are fully expandable via the existing lazy-load mechanism, allowing the user to browse the entire subtree beneath a hit without leaving the search results panel.
- **NoHit Filtering in Search:** Folders that contain no assets anywhere in their subtree (greyed "empty" folders) are automatically excluded from search results.
- **Search Visual Feedback:** While a search is running, a "Searching…" placeholder appears in the tree, the search controls are disabled, and the cursor changes to the system wait cursor. Results replace the placeholder atomically when complete.
- **Search Root Iconography:** The Search Results root node displays `search.png` when results are populated and `search-g.png` when empty, matching the grey/bright state of its text color.
- **Clear Search Button:** A dedicated clear button (`clear.png`) sits between the input and search button. Clicking it clears the input and collapses the Search Results node instantly.
- **Folder Shortcuts in Collections:** Users can now add any folder (physical library folder or Combined View virtual folder) directly to a Collection via the right-click context menu. The folder appears as a navigable child node under the collection, functioning identically to a folder shortcut.
- **"New Collection" Quick-Add:** All "Add to Collection" flyout menus (grid items and folder nodes) now have a "New Collection" entry pinned at the top with a separator below it. Clicking it creates a collection named "New Collection" (or reuses it if it already exists) and immediately adds the item.
- **Delete Collection:** Added a "Delete Collection" context menu item directly below "Rename Collection". The action is only enabled when the collection is completely empty (no asset items and no folder shortcuts), preventing accidental data loss.
- **Tri-State Folder Icons:** Folder nodes in the tree now use three distinct icons to communicate asset presence at a glance: `folder-full.png` (assets exist directly in this folder), `folder-hit.png` (assets exist in a subfolder), and `folder-empty.png` (no assets anywhere in the subtree).
- **Asynchronous Hit Detection:** Folder hit-state scanning is now fully deferred. On first paint, nodes display a bright default icon immediately. A `QPersistentModelIndex` queue processes one folder per event-loop tick via `QTimer::singleShot(0)`, emitting targeted `dataChanged` signals so icons and colors fill in progressively without blocking the UI.

### Changed
- **Collections Replace Shortcuts/Favorites:** The Shortcuts and Favorites systems have been removed entirely. Their functionality (grouping folders independently of physical location) is now unified under Collections, which supports both individual asset items and folder shortcuts in the same collection.
- **Alphabetical Collection Ordering:** Collection nodes are automatically sorted alphabetically on initial load, after a rename, and after a new collection is created.
- **Faster Tree Expansion:** Child nodes are now inserted via a single `item->appendRows(QList<QStandardItem*>)` call wrapped in `setUpdatesEnabled(false/true)`, replacing per-item `appendRow` calls to eliminate redundant `rowsInserted` signal emissions during expansion.
- **Database Cleanup:** Removed the unused `AssetFavorites` table and its associated index from `initialize.sql`.

### Fixed
- **Collection Staying Grey After Folder Add:** Adding a folder shortcut to a collection now correctly calls `invalidateAndRefresh` on the collection's cache entry, so its icon and color update immediately without requiring a manual refresh.
- **Combined View Folders in Collections (Not Found):** Virtual `COMBINED_DIR_` paths no longer trigger a filesystem existence check (which would always fail). The system now detects the `COMBINED_DIR_` prefix and uses library database queries to validate and display the folder correctly.
- **Search Results Root Greyed:** The Search Results node is now correctly bright when results are present. The proxy model evaluates `rowCount()` on the source item at paint time rather than attempting a `folderHitState` call on a non-filesystem path.

## [0.0.9] - 2026-06-06
### Added
- **Interactive Tooltips:** Implemented `CustomToolTip` class (inherited from `QWidget` with a `QLabel` child) to replace standard native tooltips. This allows for hoverable, interactive rich-text panels that remain open when the user moves their mouse over them.
- **Tooltip Grace Period:** Added custom `eventFilter` and `QTimer` logic to the thumbnail grid, providing a 400ms "grace period" for the user to move the mouse into the tooltip before it closes.
- **Context Menu Expansion:** Integrated dynamic "Add to Collection" sub-menu into the grid right-click context menu, featuring live database polling.
- **Smart "Browse Folder" Routing:** Added a highly efficient `QDirIterator` peek algorithm to virtual "Combined View" nodes, conditionally displaying a "Browse Folder" context action only if exactly one mapped physical folder actually contains files.
- **Double-Click Action:** Wired `QListWidget::itemDoubleClicked` signal to the open-asset logic, synchronizing double-click behavior with the context menu's "Open" action.

### Changed
- **UI Architecture:** Migrated all color definitions from hardcoded RGB values to centralized `Constants` in `constants.h` (e.g., `COLOR_ACCENT_BLUE`, `COLOR_THUMB_BG_START`, etc.), ensuring theme consistency across C++ and QSS.
- **Tooltip UX:** - Disabled OS-level tooltip animations (fade/slide) via `QApplication::setEffectEnabled` for a snappier, professional feel.
    - Implemented a custom `QProxyStyle` to override native `SH_ToolTip_WakeUpDelay` and `SH_ToolTip_FallAsleepDelay`, allowing for immediate resetting of delays when moving between items.
- **Context Menu UX:** Overrode native `PM_SubMenuOverlap` via custom `QProxyStyle` to introduce a 4-pixel visual gap between parent and child menus, overriding default native OS overlap rules.
- **QSS Modularity:** Transitioned widget-specific C++ stylesheet injections to a dedicated `_assetmanager.qss` file to prevent CSS specificity wars and isolate component styles.
- **Refactoring:** - Decoupled `AssetManagerWidget` logic from native tooltips to support custom, interactive floating widgets.
    - Updated `QListWidgetItem` data handling to securely store metadata (`Qt::UserRole` for paths and `Qt::UserRole + 1` for HTML content).

### Fixed
- **Rendering Artifacts:** Applied `Qt::WA_TranslucentBackground` to the custom tooltip and dynamically spawned sub-menus to eliminate rigid rectangular background artifacts around rounded CSS borders.
- **Selection Highlight Conflicts:** Prevented Qt from auto-tinting `QIcon` images upon selection, and resolved CSS specificity overrides to ensure a crisp, dark gray background with a blue accent border on selected items.
- **Memory/Build Issues:** - Resolved `QListWidgetItem` syntax errors by adding proper forward declarations in `assetmanagerwidget.h`.
    - Fixed CMake `AutoMoc` errors by correctly including `"assetmanagerwidget.moc"` in the C++ file for nested classes.
    - Fixed logic leak in mouse-move event tracking by introducing `activeToolTipItem` state tracking to prevent stuck tooltips.

## [0.0.8] - 2026-06-04
### Added
- **Virtual Collections System:** Implemented a database-backed "Collections" architecture, allowing users to group and organize 3D assets entirely independently of their physical hard-drive locations.
- **Dynamic QSS Properties:** Subclassed standard Qt UI components (e.g., `AssetTreeView`) to expose custom C++ `Q_PROPERTY` variables, successfully bridging pure CSS stylesheets with highly customized C++ `QPainter` drawing logic.
- **Context-Aware Right-Click Menus:** Built highly specific context menus that dynamically adapt based on the clicked node type (e.g., Root Nodes, Virtual Collections, Physical Folders). 
- **Hierarchy Protection:** Implemented intelligent tree-traversal logic to detect if a folder is a nested child of an existing Favorite, preventing redundant database entries.
- **Global UI Terminology Framework:** Abstracted core UI nomenclature (like "Favorites" and "Collections") into global constants (`constants.h`), establishing a robust foundation for future internationalization (i18n) and user-customization.
- **Contextual Iconography:** Integrated a full suite of custom icons (Expand, Collapse, Rename, Browse, Refresh) into the new context menus to improve navigation speed and visual hierarchy.

### Changed
- **Proxy Model Abstraction:** Upgraded the `AssetFolderProxyModel` to seamlessly route, cache, and serve both physical disk directory scans and SQLite database queries without the frontend grid renderer knowing the difference.
- **Inline Editor Geometry:** Overrode `updateEditorGeometry` in the item delegate to perfectly align Qt's native `QLineEdit` text box with custom-painted C++ text margins during renaming.
- **Root Node Protections:** Stripped native OS edit privileges (F2/slow double-click) from structural tree headers ("Favorites", "Collections") to prevent accidental renaming.
- **Open-Source Code Sweep:** Performed a massive cleanup of `assetmanagerwidget.cpp` and `main.cpp`, grouping internal logic and applying professional Doxygen-style blocks to assist new open-source contributors.

### Fixed
- **Fatal Startup Crash:** Resolved a silent Windows access violation (segfault) where the database was attempting to populate the UI grid before the widget pointers had been fully instantiated in memory.
- **Menu Separator Layout:** Fixed native Qt Fusion style quirks by explicitly targeting and padding `QMenu::icon` and `QMenu::separator` in the global `.qss` file.

## [0.0.7] - 2026-06-01
### Added
- **Asset Manager Grid UI:** Implemented a responsive `QListWidget` (Icon Mode) to visually display 3D assets and paired thumbnails.
- **Smart Recursive Scanning:** Added a UI toggle allowing users to choose between scanning only the root directory or recursively scanning all subfolders.
- **Non-Blocking Progress Dialog:** Integrated `QProgressDialog` into the scanning engine to prevent UI lockups during massive directory traversals. Includes a safe "Cancel" feature and dynamic text truncation for deep folder paths.
- **Procedural Thumbnail Rendering:** Engineered a custom `QPainter` canvas to mathematically bake a dark-theme gradient and layout padding directly into the image buffer, bypassing native CSS layout limitations.
- **High-Quality Image Scaling:** Thumbnails are now dynamically loaded into RAM and scaled using bilinear filtering (`Qt::SmoothTransformation`) to preserve aspect ratios without pixelation.
- **Alphabetical Sorting:** The Asset Library now automatically sorts discovered assets from A to Z prior to rendering.

### Changed
- **Main Boot Sequence:** Overhauled `main.cpp` to establish a clear, modular bootstrap sequence (Database -> Preferences -> Theming -> Layout). 
- **Safe Database Defaults:** Changed the `initDb` factory reset flag default to `0` to prevent accidental database wiping on standard launches.
- **CMake OS Agnosticism:** Removed hardcoded local Qt paths from `CMakeLists.txt` and added command-line instructions, ensuring the project builds cleanly across different contributor operating systems.
- **Grid Styling:** Replaced default Windows/macOS scrollbars with a custom dark-theme CSS implementation and added premium hover/selection states to the asset grid.

### Fixed
- **Text Padding Jitter:** Solved a known Qt layout bug where CSS `padding-top` shifted the entire thumbnail bounding box rather than the internal text.
- **Sorting Performance:** Relocated the `sortItems()` call outside the main scanning loop to prevent redundant UI recalculations and massively improve load times.
- **Application Exit Codes:** Corrected a bug in `main.cpp` where a failed database connection returned `false` (0/Success) instead of `-1` (Failure) to the operating system.

### Documentation
- **Open-Source Code Sweep:** Applied professional, Doxygen-style header comments to all core C++ files to explain architectural intent to future contributors.
- **File Architecture:** Documented the exact purpose and grouping of the Qt Resource (`.qrc`), Stylesheet (`.qss`), and SQL initialization files.

## [0.0.6] - 2026-05-31
### Added
- **PreferencesManager Singleton:** Implemented a fast, thread-safe memory cache for instant application-wide access to user settings without repetitive database querying.
- **AssetManagerWidget:** Created a standalone, modular C++ UI class for the upcoming 3D asset library, establishing a blueprint for future decoupled UI components.
- **Qt Resource Bundling:** Integrated `resources.qrc` to securely embed global stylesheets and database initialization scripts directly into the executable for guaranteed portability.

### Changed
- **Workspace UI:** Re-architected the main `QSplitter` layout to anchor the Properties and Assets tabs to the right side of the screen, adopting an industry-standard layout for 3D software.
- **Database Initialization:** Overhauled the SQLite startup sequence to dynamically target the active executable path, allowing for robust "factory reset" database regeneration.
- **Preferences Schema:** Upgraded the database schema to utilize a `UNIQUE` constraint on preference names, supporting safe, non-destructive `UPSERT` saves.
- **Global Styling:** Migrated all inline C++ styles into a global `.qss` file, utilizing targeted Object IDs for clean, scalable theme management.

### Fixed
- Resolved OS file-locking crashes during database regeneration by ensuring active connections are explicitly closed before file deletion.
- Corrected raw SQL insertion syntax in `initialize.sql` to properly escape strings and target correct tables instead of indexes.

## [0.0.5] - 2026-05-31
### Changed
- Refactored project structure to cleanly isolate source code within a dedicated `src/` directory.
- Modularized core functions to separate UI logic from initialization routines.

## [0.0.4] - 2026-05-30
### Added
- Created the main application splash screen overlay.

## [0.0.3] - 2026-05-30
### Added
- Built the foundational main application window instance.
- Implemented the initial top-level file menu architecture.

## [0.0.2] - 2026-05-29
### Added
- Created initial "Hello World" C++ Qt application package.
- Integrated SQLite core library into the application.
- Created initial database table `.sql` file and initialization function.

## [0.0.1] - 2026-05-24
### Added
- Initial project creation and repository initialization.
- Official PoseStudio project logo.
- Foundational `README.md` establishing the project vision as an open-source 3D character platform.
- Community funding configurations for GitHub Sponsors and Patreon.
- Core open-source community files including `LICENSE`, `.gitignore`, and `CODE_OF_CONDUCT.md`.
- Associated domain linkages for posestudio.org.
- Updated Code of Conduct.

## [Unreleased]
### Added
- Foundational architectural planning for 3D character posing and rigging features.
- Initial scaffolding for the core application framework.