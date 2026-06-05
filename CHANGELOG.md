# Changelog

All notable changes to the PoseStudio project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.0.9] - 2026-06-06
### Added
- **Interactive Tooltips:** Implemented `CustomToolTip` class (inherited from `QWidget` with a `QLabel` child) to replace standard native tooltips. This allows for hoverable, interactive rich-text panels that remain open when the user moves their mouse over them.
- **Tooltip Grace Period:** Added custom `eventFilter` and `QTimer` logic to the thumbnail grid, providing a 400ms "grace period" for the user to move the mouse into the tooltip before it closes.
- **Context Menu Expansion:** Integrated dynamic "Add to Collection" sub-menu into the grid right-click context menu, featuring live database polling.
- **Double-Click Action:** Wired `QListWidget::itemDoubleClicked` signal to the open-asset logic, synchronizing double-click behavior with the context menu's "Open" action.

### Changed
- **UI Architecture:** Migrated all color definitions from hardcoded RGB values to centralized `Constants` in `constants.h` (e.g., `COLOR_ACCENT_BLUE`, `COLOR_THUMB_BG_START`, etc.), ensuring theme consistency across C++ and QSS.
- **Tooltip UX:** - Disabled OS-level tooltip animations (fade/slide) via `QApplication::setEffectEnabled` for a snappier, professional feel.
    - Implemented a custom `QProxyStyle` to override native `SH_ToolTip_WakeUpDelay` and `SH_ToolTip_FallAsleepDelay`, allowing for immediate resetting of delays when moving between items.
- **Refactoring:** - Decoupled `AssetManagerWidget` logic from native tooltips to support custom, interactive floating widgets.
    - Updated `QListWidgetItem` data handling to securely store metadata (`Qt::UserRole` for paths and `Qt::UserRole + 1` for HTML content).

### Fixed
- **Rendering Artifacts:** Applied `Qt::WA_TranslucentBackground` to the custom tooltip window to eliminate rigid rectangular background artifacts around rounded CSS borders.
- **Memory/Build Issues:** - Resolved `QListWidgetItem` syntax errors by adding proper forward declarations in `assetmanagerwidget.h`.
    - Fixed CMake `AutoMoc` errors by correctly including `"assetmanagerwidget.moc"` in the C++ file for nested classes.
    - Fixed logic leak in mouse-move event tracking by introducing `activeToolTipItem` state tracking.



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
- Associated domain linkages for PoseStudio.org.
- Updated Code of Conduct

## [Unreleased]
### Added
- Foundational architectural planning for 3D character posing and rigging features.
- Initial scaffolding for the core application framework.