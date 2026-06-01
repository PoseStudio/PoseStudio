# Changelog

All notable changes to the PoseStudio project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
