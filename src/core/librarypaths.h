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
 * subfolder — the folder the Environment panel lists and where users drop their own .hdr files —
 * and materials in its Materials/ subfolder, the folder the Materials Library tab browses.
 *
 * Cost model: the library root is looked up in the database ONCE and cached (see
 * invalidateCache); every other function is pure path arithmetic or a directory scan, and the
 * one filesystem write (creating hdri/) is explicit — ensureHdriDirectory() — rather than a side
 * effect of every lookup. The panorama scan returns each file's category and display name
 * precomputed, so callers never re-derive them.
 */

#ifndef LIBRARYPATHS_H
#define LIBRARYPATHS_H

#include <QList>
#include <QString>
#include <QStringList>

namespace LibraryPaths {

/// The user's "My PoseStudio Library" root: the first *enabled, non-built-in* asset library whose
/// folder name matches Constants::USER_LIBRARY_DIRNAME; else the default Documents location
/// (whether or not it exists on disk yet). Never returns an empty string. Requires the application
/// database to be open (main.cpp opens it before any widget is constructed). Cached after the
/// first call — see invalidateCache().
QString userLibraryRoot();

/// Forgets the cached library root so the next userLibraryRoot() re-reads the AssetLibraries
/// table. Call it after any library is added or removed (Preferences → Assets does; the Asset
/// Manager's own add/remove-library actions must too) — otherwise a "My PoseStudio Library"
/// registered mid-session isn't seen until the next launch.
void invalidateCache();

/// The HDRI environments folder inside the user library (<root>/hdri) — a pure path, which may
/// not exist yet; ensureHdriDirectory() creates it. A panorama (.hdr or .exr) in this folder
/// appears in the Environment panel's HDRI menu; a same-basename image file (any common format,
/// e.g. .jpg — the stock previews) becomes its menu thumbnail. Users may categorize panoramas into
/// subfolders — each subfolder becomes a heading in the HDRI menu.
QString hdriDirectory();

/// hdriDirectory(), created on disk if it is missing, so "drop files here" always has a target.
/// Called from the startup path (the HDRI selector's constructor) and the "Open HDRI Folder…"
/// link — deliberately not from every lookup, which used to cost a filesystem write per call.
QString ensureHdriDirectory();

/// The materials folder inside the user library (<root>/Materials): the ONE folder the Materials
/// Library tab browses (assets/assetmanagerscope.h) — a pure path, which may not exist yet;
/// ensureMaterialsDirectory() creates it. Subfolders are the library's categories. A developer
/// lever, POSESTUDIO_MATERIALS_DIR=<folder>, names another folder for a run (how the tab is
/// looked at with test content, without touching the user's own library).
QString materialsDirectory();

/// materialsDirectory(), created on disk if it is missing, so the tab always has its root and
/// "put materials here" always has a target (an install that predates the folder gets it the
/// first time the tab is built).
QString ensureMaterialsDirectory();

/// The library a folder belongs to: the outermost registered asset library (or the materials
/// folder, where the developer lever points it elsewhere) that @p folder is or lies under; ""
/// for a folder outside every library. What a saved asset's textures are searched under when
/// they are not where the asset says (assets/assetstore.h).
QString libraryRootOf(const QString& folder);

/// One panorama in the HDRI menu, with everything the menu shows precomputed.
struct HdriEntry {
    QString path;     ///< Absolute file path (.hdr or .exr)
    QString category; ///< Its folder relative to the hdri root ("" = uncategorized root file;
                      ///< a nested subfolder reads "Outdoor/Sunset")
    QString name;     ///< Basename without extension — the menu text and the thumbnail-pairing key
};

/// Every panorama (.hdr/.exr) under hdriDirectory(), searched recursively so subfolder-categorized
/// collections are found, in display order: uncategorized root files first, then each subfolder
/// (= category) alphabetically, files alphabetical within — the exact order the Environment
/// panel's HDRI menu renders, with a heading per subfolder.
QList<HdriEntry> hdriEntries();

/// Just the paths of hdriEntries(), in the same order — for callers that only walk files.
QStringList hdriFiles();

/// The startup/default panorama. An explicit override next to the executable wins — <appDir>/
/// environment.hdr, then environment.exr (a developer/packaging lever for forcing one specific
/// environment, checked before the library) — else the stock panorama by name wherever the user
/// filed it, else the first of hdriEntries(); empty when there are no panoramas at all (the
/// viewport then keeps its procedural studio environment). Shared by the viewport's startup
/// environment load and the Environment panel's initial selection, so the button text can't
/// disagree with what the viewport loaded.
QString defaultHdri();

} // namespace LibraryPaths

#endif // LIBRARYPATHS_H
