// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetscan.h
 * @brief Filesystem scanning for the Asset Manager: what counts as an asset, how folders are
 *        classified, how paths are compared. QtCore only, no widgets, no database.
 *
 * The one rule everything here implements: an ASSET is a non-image file (sidecar extensions
 * such as .mtl excluded) with a picture beside it that shares its name UP TO THE FIRST DOT.
 * WHICH picture is its thumbnail is the narrower question: the largest one NAMED AFTER THE
 * ASSET — for `Name.duf` that is `Name.png`, `Name.duf.png` (the whole file name), or either
 * with `.tip` before the picture's extension (`Name.tip.png`, the larger preview some content
 * ships beside the small one; never an asset of its own, being a picture) — and only an asset
 * with no picture named after it wears the largest picture of the first-dot group. So
 * `Material.001.psm` and `Material.002.psm` are two assets with an icon each, and a file that
 * was an asset when the first dot alone decided is one still. An asset is LABELLED by its
 * whole name without the extension (`Material.001`).
 *
 * A folder that holds such a pair is a DirectHit, one that only has a DirectHit somewhere below
 * it is an IndirectHit, and anything else is NoHit (greyed in the tree). Every scan site in the
 * module (tree lazy-load, grid subfolders, search, hit classification) goes through the filters
 * and helpers of assetpaths.h — the symlink, case and picture rules, each decided exactly once —
 * and through the functions below, so that one more rule holds everywhere:
 *
 * - HIDDEN NAMES are never listed (core/hiddennames.h): a file or a folder whose name begins
 *   with a dot is not an asset, not a thumbnail, not a folder of the tree or the grid, and a
 *   hidden folder is never walked into, so nothing in it is found either. That is the whole
 *   mechanism for keeping something out of a browser; no folder NAME is special. (What the
 *   operating system marks hidden is left out by the QDir filters themselves, which carry no
 *   QDir::Hidden.)
 *
 * The classification (classifyFolder) is a pure function of the filesystem so the proxy model
 * can run it on a worker thread; it never touches Qt GUI types.
 */

#ifndef ASSETSCAN_H
#define ASSETSCAN_H

#include "assetpaths.h"
#include "hiddennames.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfoList>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace AssetScan {

/**
 * @struct AssetHit
 * @brief One discovered asset: the file, its folder, its best thumbnail, and the stat data the
 *        grid's tooltip shows, all captured from the ONE directory listing that found it (so
 *        displaying a folder never re-stats each asset).
 */
struct AssetHit {
    QString folderPath;     ///< Absolute path to the directory containing the asset
    QString assetFileName;  ///< Filename of the asset (e.g. model.obj, figure.duf)
    QString bestImage;      ///< Filename of its thumbnail (the file banner's rule), or empty
    qint64 sizeBytes = 0;   ///< Asset file size
    QDateTime lastModified; ///< Asset file mtime
};

/// How a folder relates to assets (see the file banner). The numeric order matters to nobody;
/// the names do: only DirectHit folders become search results and list assets in the grid.
enum class HitState { NoHit = 0, IndirectHit = 1, DirectHit = 2 };

/// The members of `assetPaths` that lie in or under one of `roots`, in input order: what a
/// browser that looks at part of the disk shows of a list kept for the whole application
/// (Favorites, in the Materials Library).
QStringList pathsWithin(const QStringList& assetPaths, const QStringList& roots);

/// Display name for a folder: its dirName(), or the path itself for a drive root ("D:/"), whose
/// dirName() is empty. A drive-root library would otherwise render as a nameless tree row.
QString folderDisplayName(const QString& path);

/// True when `path` has at least one real (non-symlink) subdirectory that is listed (a hidden
/// one is not): decides whether a tree row gets the "..." lazy-load placeholder that makes it
/// expandable.
bool hasSubdirectories(const QString& path);

/// The real subdirectories of `path`, sorted by name, case-insensitive: the order the tree
/// and the grid list them in. Hidden folders (a name that begins with a dot) are left out.
QFileInfoList listSubdirectories(const QString& path);

/// True if any ancestor directory of `path` is present in `set`. Walks up with
/// QFileInfo::path() so a drive-root ancestor ("D:/") is reached: truncating at the last '/'
/// turned "D:/x" into "D:", which never matched a root library stored as "D:/". The set holds
/// paths produced by the same scan as `path`, so membership is exact-case by design.
bool hasAncestorIn(const QString& path, const QSet<QString>& set);

/// Caps a search result's displayed path to its last `maxSegments` segments, prefixing "..."
/// when truncated, joined with " / " (the separator AssetTreeDelegate greys the prefix of).
QString truncateSearchPath(const QString& relPath, int maxSegments = 4);

/**
 * @brief Classifies `folderPath` (DirectHit / IndirectHit / NoHit), walking into subfolders
 *        only as far as needed: a direct hit ends the walk, and the first subtree with a hit
 *        makes the folder an IndirectHit without visiting its siblings.
 * @param known States already established (the caller's cache); any folder found here is
 *        taken as-is instead of re-walked. Read only, so a copy of an implicitly-shared
 *        QHash can be handed to a worker thread safely.
 * @return The state of every folder the walk visited, `folderPath` included, for the caller
 *         to merge into its cache. Each folder is listed ONCE (files and dirs together).
 */
QHash<QString, HitState> classifyFolder(const QString& folderPath,
                                        const QHash<QString, HitState>& known);

/// The assets directly inside `folderPath` (non-recursive), in no particular order: each
/// non-image file with a picture beside it, paired with its thumbnail (the file banner's rule).
/// Files without a picture are not assets, and neither is a hidden file (nor is a hidden picture
/// a thumbnail).
QList<AssetHit> scanFolderAssets(const QString& folderPath);

/**
 * @brief Resolves a flat list of asset file paths (Favorites/Collection items) into AssetHits.
 *        Each folder is listed once (paths grouped by folder internally), and the result is
 *        returned in the caller's INPUT order: Favorites and Collections rely on this to
 *        preserve the user's manual drag order. Paths whose file no longer exists are dropped;
 *        an asset whose thumbnail has gone comes back with an empty bestImage.
 */
QList<AssetHit> resolveAssetHits(const QStringList& assetPaths);

} // namespace AssetScan

#endif // ASSETSCAN_H
