/**
 * @file assetstore.h
 * @brief WHERE A SAVED ASSET'S FILES LIVE — the texture store beside the asset, and the file
 *        operations that keep an asset and its textures together. QtCore only: no widgets, no
 *        database, no Vulkan.
 *
 * THE STANDARD (2026-11-13). A saved asset is a file, its icon beside it under the same name,
 * and the texture files it samples in a `.textures` folder beside both:
 *
 *     Fabric/
 *         Denim.psm            the asset
 *         Denim.png            its icon (what makes it an asset to the library browser)
 *         .textures/
 *             Denim/           the files Denim brought with it
 *                 denim_color.jpg
 *                 denim_normal.png
 *
 * THE STORE IS HIDDEN BY ITS NAME (2026-11-14): the library browsers show no file or folder
 * whose name begins with a dot (core/hiddennames.h), and the store is simply one such folder.
 * It was `textures`, left out of the browsers by a rule of its own, for a day; a folder of
 * that name is an ordinary folder again (other applications' content has many). Nothing here
 * tests for a hidden name: the store is reached by its path. Where a walk must go INTO it
 * (a search by file name, a folder packed whole) the filter carries QDir::Hidden, since Qt
 * leaves dot names out of a listing on macOS and Linux unless asked.
 *
 * The asset names its textures by paths RELATIVE to its own folder, so the folder is the whole
 * product: it can be moved, copied to another machine, zipped or deleted as one thing, and no
 * central store has to be kept in step with it. There is no application-wide texture folder.
 *
 * COLLECTING (TextureCollector) is what a save does with each texture: a file that already lies
 * beside the asset, or in its folder's store, is named where it is; any other file is copied
 * into `.textures/<asset name>/` — unless a file with the SAME BYTES is already somewhere in the
 * folder's store, which is then named instead (thirty color variants of one fabric share one
 * normal map). A copy never overwrites a different file: it takes the next free name.
 *
 * FINDING (resolveTexture) is what a load does with each name, in this order: relative to the
 * asset's folder; relative to the library's root; the path itself where it is absolute; then
 * by FILE NAME — beside the asset, in its store, and at last anywhere under the library's
 * root. So an asset whose folder was rearranged by hand still finds what is still there.
 *
 * THE FILE OPERATIONS (assetfileops.h: rename, duplicate, move, delete) work on an asset of
 * PoseStudio's own (isOwnAsset: a saved material today) and take its icon and its textures
 * along. They read the asset as generic JSON — `material.textures[].source` — and rewrite only
 * what they must (a name, a path), so a file written by a newer version keeps every field this
 * build does not know. A move and a delete take along only the texture files the asset ITSELF
 * names in its folder's store, and of those only the ones no other asset of the folder still
 * names; a file in the store that no asset names is never removed.
 *
 * A PACKAGE (assetpackage.h) is a plain .zip of assets with their icons and textures, laid out
 * as they lie in their folder: installing one is unzipping it into a library folder
 * (installPackage does it with the checks an archive from elsewhere needs: no entry may leave
 * the destination).
 *
 * THE FILES: this header and assetstore.cpp are the store proper — collecting, finding, and
 * reading what an asset names; assetfileops.{h,cpp} the file operations; assetpackage.{h,cpp}
 * the packages (the one part that needs miniz); assetstore_detail.h the helpers the three
 * implementations share.
 */

#ifndef ASSETSTORE_H
#define ASSETSTORE_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

namespace AssetStore {

/// The name of an asset folder's texture store: ".textures" — a hidden name (hiddennames.h),
/// which is all that keeps the store out of the library browsers.
inline QString texturesFolderName() {
    return QStringLiteral(".textures");
}

/// True when a folder of that NAME is a texture store (case-insensitive): it holds images an
/// asset samples, never assets, so no asset is moved into one.
inline bool isTextureStore(const QString& folderName) {
    return folderName.compare(texturesFolderName(), Qt::CaseInsensitive) == 0;
}

/// True for a file the operations below may rename, move and delete: an asset PoseStudio itself
/// writes (a saved material). Content of other applications' libraries is read in place and is
/// never theirs to rearrange.
bool isOwnAsset(const QString& path);

/// What collecting one texture did.
enum class Placed {
    Linked, ///< It lies beside the asset or in its folder's store already: named where it is.
    Reused, ///< A file with the same bytes is in the store: that one is named, nothing copied.
    Copied, ///< Copied (or written) into `.textures/<asset name>/`.
    Failed  ///< It could not be read or written; nothing is named.
};

/**
 * @class TextureCollector
 * @brief One save's worth of collecting: each texture of the asset `<folder>/<name>.*` is
 *        handed over, and the path to WRITE INTO THE ASSET comes back — relative to the asset's
 *        folder, with forward slashes. The store is listed once, on first need.
 */
class TextureCollector {
public:
    TextureCollector(const QString& assetFolder, const QString& assetName);

    /// A texture that is a file on disk. "" (and Placed::Failed) when it cannot be read or copied.
    QString addFile(const QString& sourcePath, Placed* how = nullptr);

    /// A texture that has no file of its own (an image that lives inside a model's file), given
    /// as the bytes of an image file and the name to store them under.
    QString addBytes(const QByteArray& bytes, const QString& fileName, Placed* how = nullptr);

    /// The folder new files go into: `<assetFolder>/.textures/<asset name>`.
    QString ownFolder() const;

private:
    void    indexStore();
    QString findSame(qint64 size, const QString& sourcePath, const QByteArray* bytes) const;
    QString freeName(const QString& fileName) const;
    QString relative(const QString& absolutePath) const;

    QString                    m_folder;
    QString                    m_name;
    bool                       m_indexed = false;
    QHash<qint64, QStringList> m_bySize; ///< The store's files by size (absolute paths).
    QHash<QString, QString>    m_done;   ///< A source already handed over → the path written for it.
    QHash<QString, Placed>     m_how;
};

/**
 * @class TextureResolver
 * @brief One load's worth of finding (FINDING in the file comment): each texture source ONE
 *        asset names is handed over, and where the file is now comes back ("" = nowhere).
 *
 * A source that is not where it was written is looked for by its file name, which means listing
 * folders: the asset's own, its store, and at last the whole library. Each is listed ONCE per
 * resolver, on first need, so an asset whose maps have all gone costs one walk of its library
 * and not one per map. The walks share one budget of entries (a library on a slow drive must
 * not hold a load for minutes): what lies beyond it is not found.
 */
class TextureResolver {
public:
    /// @p libraryRoot may be empty (an asset outside every library).
    TextureResolver(const QString& assetFolder, const QString& libraryRoot);

    /// Where @p source is now. @p viaName, when given, is set where the file was found by its
    /// name and not at the path as written.
    QString resolve(const QString& source, bool* viaName = nullptr);

private:
    QString     m_folder;
    QString     m_library;
    int         m_budget;                ///< Entries the walks may still visit.
    bool        m_besideListed = false;
    bool        m_storeWalked = false;
    bool        m_libraryWalked = false;
    QStringList m_beside;                ///< The files beside the asset, in listing order.
    QStringList m_inStore;               ///< Every file under its store, in walk order.
    QStringList m_inLibrary;             ///< Every file under the library's root, in walk order.
};

/// One source resolved by a resolver of its own: TextureResolver(@p assetFolder,
/// @p libraryRoot).resolve(@p source, @p viaName). For several sources of one asset, keep the
/// resolver.
QString resolveTexture(const QString& source, const QString& assetFolder, const QString& libraryRoot, bool* viaName = nullptr);

/// The texture sources the saved material @p materialPath names, as written. Empty for a file
/// that is not a material (or cannot be read).
QStringList materialSources(const QString& materialPath, bool* readable = nullptr);

/// The files those sources name that EXIST and lie in or under the material's own folder: what
/// travels with it. Absolute paths, each once. For both: @p readable, when given, says whether
/// the file could be read as a material at all — false, what it names is UNKNOWN, which is not
/// the same as naming nothing (a file that is mid-sync, an online-only placeholder, a damaged
/// copy).
QStringList materialOwnFiles(const QString& materialPath, bool* readable = nullptr);

/// The asset's icons: the image files beside it that carry its name.
QStringList iconsOf(const QString& assetPath);

/// What a file operation or a package operation did (assetfileops.h, assetpackage.h).
struct Result {
    bool        ok = false;
    QString     error;   ///< For people, when !ok.
    QString     path;    ///< The asset where it is now (rename, duplicate, move); the package (export); the folder (install).
    int         files = 0;   ///< Texture files that went along (copied, moved, removed, packed, installed).
    int         reused = 0;  ///< … of a move: textures the destination's store already had.
    /// Files left as they were. install: the ones that were there with other content. delete,
    /// move: the asset's own store textures that were NOT removed because another asset of the
    /// folder could not be read, and may be the one that still names them (paths relative to
    /// the folder).
    QStringList kept;
};

} // namespace AssetStore

#endif // ASSETSTORE_H
