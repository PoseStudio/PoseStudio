/**
 * @file librarypaths.h
 * @brief Resolves per-user content locations rooted in the user's own asset library.
 *
 * The "My PoseStudio Library" folder is the writable, user-visible home for content the user is
 * expected to browse and extend by hand — unlike QStandardPaths::AppDataLocation, which hides
 * internal state (the database). It is an ordinary asset library: registered in the AssetLibraries
 * table and browsable in the Asset Manager. initializeDatabase() creates it in Documents once on a
 * fresh install; a user who already has a library folder by that name (registered from anywhere on
 * disk) keeps theirs, and that one is resolved here. HDRI environment panoramas live in its hdri/
 * subfolder — the folder the Environment panel lists and where users drop their own .hdr files.
 */

#ifndef LIBRARYPATHS_H
#define LIBRARYPATHS_H

#include <QString>

namespace LibraryPaths {

/// The user's "My PoseStudio Library" root: the first *enabled, non-built-in* asset library whose
/// folder name matches Constants::USER_LIBRARY_DIRNAME; else the default Documents location
/// (whether or not it exists on disk yet). Never returns an empty string. Requires the "db_conn"
/// database to be open (main.cpp opens it before any widget is constructed).
QString userLibraryRoot();

/// The HDRI environments folder inside the user library, created on demand so "drop files here"
/// always has a target. An .hdr in this folder appears in the Environment panel's HDRI menu; a
/// same-basename image file (any common format, e.g. .webp) becomes its menu thumbnail.
QString hdriDirectory();

} // namespace LibraryPaths

#endif // LIBRARYPATHS_H
