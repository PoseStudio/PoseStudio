/**
 * @file assetfileops.h
 * @brief THE FILE OPERATIONS on a saved asset — rename, duplicate, move, delete — which keep the
 *        asset, its icon and its textures together. QtCore only. The standard they keep is
 *        assetstore.h's (read its file comment first).
 *
 * They work on an asset of PoseStudio's own (AssetStore::isOwnAsset: a saved material today) and
 * read it as generic JSON — `material.textures[].source` — rewriting only what they must (a
 * name, a path), so a file written by a newer version keeps every field this build does not
 * know.
 */

#ifndef ASSETFILEOPS_H
#define ASSETFILEOPS_H

#include "assetstore.h"

#include <QString>

namespace AssetStore {

/// True when @p name can be an asset's name: not empty, none of \ / : * ? " < > |, no trailing
/// dot or space, no leading dot (a hidden name: the asset would vanish from the browser it was
/// renamed in, with no way to bring it back there). @p why says what is wrong when it cannot.
bool validAssetName(const QString& name, QString* why = nullptr);

// Rename, duplicate and move are ALL OR NOTHING: an asset is a file with a picture of its name
// beside it, and one left without its picture half-way would be shown by no browser. When a
// step fails (Result::ok false, the reason in Result::error) what was done before it is undone,
// and the asset, its icons and its textures are as they were.

/// The asset under another name, in its folder: the file, its icons, and the name it carries
/// inside. Its textures stay where they are (it names them by path, not by its own name).
Result renameAsset(const QString& assetPath, const QString& newName);

/// A second asset beside the first — "<name> copy", "<name> copy 2", … — with a copy of the
/// icon, naming the same textures (nothing in the store is copied).
Result duplicateAsset(const QString& assetPath);

/// The asset in another folder: its textures are collected into that folder's store (copied, or
/// the same bytes found there), the asset rewritten to name them, the icons moved, and the
/// texture files IT named in the old folder's store that no other asset of that folder names are
/// removed there (for good: the new folder has them). A file in the store that no asset names
/// is never touched.
Result moveAsset(const QString& assetPath, const QString& destinationFolder);

/// The asset, its icons, and the texture files IT names in its folder's store that no other
/// asset of the folder names: to the trash (or, @p permanently, removed for good). Emptied store
/// folders go too. A file in the store that no asset names is never touched: it is the user's.
///
/// For both: when another asset of the folder cannot be READ, what it names is unknown, and no
/// texture is removed at all — the asset and its icons still go, and the textures left behind
/// are listed in Result::kept.
Result deleteAsset(const QString& assetPath, bool permanently = false);

} // namespace AssetStore

#endif // ASSETFILEOPS_H
