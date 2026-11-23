// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetpaths.h
 * @brief The asset module's ground rules, each written ONCE: how a path is spelled and compared,
 *        which entries a directory listing takes, and which files are pictures. QtCore only,
 *        header only.
 *
 * Both halves of the module read these: the scans the library browsers are built from
 * (assetscan.h) and the store that keeps a saved asset's files together (assetstore.h). They
 * are in a header of their own so that the store needs nothing of the scans' implementation.
 *
 * - A path is compared in ONE spelling: cleanPath(fromNativeSeparators(path)) — normalizePath.
 * - Paths compare CASE-INSENSITIVELY everywhere in the browsers (kPathCase). Stored library
 *   paths and scanned paths come from the same QDir/QFileDialog sources, so the only way they
 *   differ by case is the user retyping one; a case-sensitive compare then silently broke
 *   breadcrumbs and "Find In Library" on Windows, where the filesystem itself is
 *   case-insensitive.
 * - Symlinked/junctioned directories are NEVER followed (the filters carry NoSymLinks). The
 *   recursive walkers have no cycle protection (QDirIterator doesn't either), and before there
 *   was one filter the tree included such folders while the grid/search/hit walk excluded them,
 *   so a junction showed in the tree but never as a grid folder and painted its parent "empty".
 * - What the operating system marks hidden is left out by the filters themselves, which carry
 *   no QDir::Hidden; a walk that must see everything (the store's: assetstore_detail.h) adds it.
 */

#ifndef ASSETPATHS_H
#define ASSETPATHS_H

#include <QDir>
#include <QSet>
#include <QString>

namespace AssetScan {

/// The subdirectory filter EVERY scan uses: real directories only, never symlinks/junctions.
inline constexpr QDir::Filters kSubdirFilter = QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks;
/// The file filter every scan uses.
inline constexpr QDir::Filters kFileFilter = QDir::Files | QDir::NoSymLinks;
/// Files and subdirectories in ONE listing (a folder that is classified is listed once).
inline constexpr QDir::Filters kEntryFilter = kSubdirFilter | kFileFilter;

/// The module-wide case policy for comparing paths (see the file banner).
inline constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;

/// cleanPath(fromNativeSeparators(path)): the canonical spelling every comparison works on.
inline QString normalizePath(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

/// True when the two paths name the same location under the module's case policy.
inline bool samePath(const QString& a, const QString& b) {
    return normalizePath(a).compare(normalizePath(b), kPathCase) == 0;
}

/// True when `folderPath` IS `libRoot` or lies under it. Both are normalized first: a library
/// registered at a drive root keeps its trailing slash ("D:/"), where a naive `lib + "/"`
/// prefix test builds "D://" and never matches, silently breaking breadcrumbs and
/// "Find In Library" for every subfolder of that library.
inline bool folderWithinLibrary(const QString& folderPath, const QString& libRoot) {
    const QString root = normalizePath(libRoot);
    const QString folder = normalizePath(folderPath);
    if (folder.compare(root, kPathCase) == 0) return true;
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return folder.startsWith(prefix, kPathCase);
}

/// True for the extension of a picture — `suffix` already lower-cased, without its dot. A
/// picture is a thumbnail candidate and an asset's icon, never an asset: an asset is any OTHER
/// file that has a picture named after it beside it.
inline bool isImageSuffix(const QString& suffix) {
    static const QSet<QString> kImageExtensions = {QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                                   QStringLiteral("bmp"),  QStringLiteral("webp"), QStringLiteral("gif"),
                                                   QStringLiteral("tif"),  QStringLiteral("tiff")};
    return kImageExtensions.contains(suffix);
}

} // namespace AssetScan

#endif // ASSETPATHS_H
