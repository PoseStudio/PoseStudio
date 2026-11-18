/**
 * @file assetpackage.h
 * @brief PACKAGES: assets with their icons and textures as one plain .zip, laid out as they lie
 *        in their folder. QtCore only (the implementation adds miniz). The layout is
 *        assetstore.h's standard, so installing a package is unzipping it into a library folder —
 *        installPackage does it with the checks an archive from elsewhere needs: no entry may
 *        leave the destination.
 */

#ifndef ASSETPACKAGE_H
#define ASSETPACKAGE_H

#include "assetstore.h"

#include <QString>
#include <QStringList>

namespace AssetStore {


/// How much a package of @p items would hold.
struct PackageSize {
    int    files = 0;
    qint64 bytes = 0; ///< As they lie on disk (the package compresses what is not compressed already).
};

/// What exportPackage would pack for @p items, counted from the same listing and without
/// reading a file: for asking before a very large folder is packed — packing reads every file
/// and cannot be interrupted.
PackageSize packageSize(const QStringList& items);

/// A package of @p items — asset files and/or whole folders — written to @p zipPath: each asset
/// with its icons and its own texture files, each folder with everything in it, under paths
/// relative to the items' common folder.
Result exportPackage(const QStringList& items, const QString& zipPath);

/// How many files of the package @p zipPath are already in @p destinationFolder with OTHER
/// content (what installing with @p replaceExisting off would leave as they are). -1 = the
/// package cannot be read.
int packageConflicts(const QString& zipPath, const QString& destinationFolder);

/// Unpacks @p zipPath into @p destinationFolder. A file that is there with the same content is
/// left; one with other content is replaced only with @p replaceExisting, else left and listed
/// in Result::kept. An entry that would land outside the destination rejects the whole package
/// before anything is written.
Result installPackage(const QString& zipPath, const QString& destinationFolder, bool replaceExisting);

} // namespace AssetStore

#endif // ASSETPACKAGE_H
