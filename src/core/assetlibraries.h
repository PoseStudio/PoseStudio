// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetlibraries.h
 * @brief The one place that reads and writes the AssetLibraries table.
 *
 * An asset library is a physical folder the Asset Manager browses. The table holds each
 * registered folder plus two flags: AssetLibraryIsBuiltIn (the single "Maquettes" library that
 * ships with the app — always sorted first, never user-removable) and AssetLibraryEnabled
 * (reserved for a future disable toggle; every reader filters on it so a disabled row would
 * simply vanish from the UI).
 *
 * Before this header existed the same SELECT/INSERT/DELETE statements were typed out in the
 * Asset Manager, the Preferences → Assets page, LibraryPaths and the database bootstrap, each
 * with its own ordering and error handling. Route every library query through here so the
 * display order (built-in first, then user libraries by folder NAME, case-insensitive — not by
 * full path, which misorders libraries living under different parents/drives) and the
 * duplicate-add semantics are defined exactly once.
 *
 * Requires the application database to be open (main.cpp opens it before any widget exists).
 * GUI thread only, like the rest of the data layer.
 */

#ifndef ASSETLIBRARIES_H
#define ASSETLIBRARIES_H

#include <QList>
#include <QString>

namespace AssetLibraries {

/// One registered library folder.
struct Library {
    int     id = 0;          ///< AssetLibraryID (the stable key for removal)
    QString path;            ///< Absolute folder path as stored (cleanPath form)
    bool    builtIn = false; ///< True for the shipped "Maquettes" library
};

/// Every ENABLED library in the Asset Manager's display order: built-in libraries first, then
/// user libraries sorted by their folder name (case-insensitive). An empty list on a query
/// failure (logged).
QList<Library> enabled();

/// Only the user-manageable (non-built-in) libraries, ordered by full path (case-insensitive) —
/// the Preferences → Assets list. Enabled or not; that page is where a disable toggle would live.
QList<Library> userManaged();

/// Just the paths of enabled(), in the same order — for callers that only need to walk folders.
QStringList enabledPaths();

/// Outcome of add(): the table has a UNIQUE constraint on the path, and INSERT OR IGNORE
/// swallows the conflict, so "already registered" must be detected and reported separately or a
/// re-add of an existing folder looks like a silent failure to the user.
enum class AddResult { Added, AlreadyRegistered, Failed };

/// Registers `path` as a new user library (never built-in). Does not refresh any UI.
AddResult add(const QString& path);

/// Removes the library with the given AssetLibraryID. Returns false (logged) on a query error.
bool remove(int id);

} // namespace AssetLibraries

#endif // ASSETLIBRARIES_H
