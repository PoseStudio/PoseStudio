/**
 * @file assetstore.h
 * @brief WHERE A SAVED ASSET'S FILES LIVE — the texture store beside the asset, and the file
 *        operations that keep an asset and its textures together. QtCore only: no widgets, no
 *        database, no Vulkan.
 *
 * THE STANDARD (2026-11-13). A saved asset is a file, its icon beside it under the same name,
 * and the texture files it samples in a `textures` folder beside both:
 *
 *     Fabric/
 *         Denim.psm            the asset
 *         Denim.png            its icon (what makes it an asset to the library browser)
 *         textures/
 *             Denim/           the files Denim brought with it
 *                 denim_color.jpg
 *                 denim_normal.png
 *
 * The asset names its textures by paths RELATIVE to its own folder, so the folder is the whole
 * product: it can be moved, copied to another machine, zipped or deleted as one thing, and no
 * central store has to be kept in step with it. There is no application-wide texture folder.
 *
 * COLLECTING (TextureCollector) is what a save does with each texture: a file that already lies
 * beside the asset, or in its folder's store, is named where it is; any other file is copied
 * into `textures/<asset name>/` — unless a file with the SAME BYTES is already somewhere in the
 * folder's store, which is then named instead (thirty color variants of one fabric share one
 * normal map). A copy never overwrites a different file: it takes the next free name.
 *
 * FINDING (resolveTexture) is what a load does with each name, in this order: relative to the
 * asset's folder; relative to the library's root; the path itself where it is absolute; then
 * by FILE NAME — beside the asset, in its store, and at last anywhere under the library's
 * root. So an asset whose folder was rearranged by hand still finds what is still there.
 *
 * THE FILE OPERATIONS (rename, duplicate, move, delete, export, install) work on an asset of
 * PoseStudio's own (isOwnAsset: a saved material today) and take its icon and its textures
 * along. They read the asset as generic JSON — `material.textures[].source` — and rewrite only
 * what they must (a name, a path), so a file written by a newer version keeps every field this
 * build does not know. A texture in the store is removed only when no other asset of the folder
 * still names it.
 *
 * A PACKAGE is a plain .zip of assets with their icons and textures, laid out as they lie in
 * their folder: installing one is unzipping it into a library folder (installPackage does it
 * with the checks an archive from elsewhere needs: no entry may leave the destination).
 */

#ifndef ASSETSTORE_H
#define ASSETSTORE_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

namespace AssetStore {

/// The name of an asset folder's texture store: "textures".
inline QString texturesFolderName() {
    return QStringLiteral("textures");
}

/// True when a folder of that NAME is a texture store (case-insensitive): the library browser
/// does not list such a folder — it holds images an asset samples, never assets.
/// (Inline, with the name above: the browser's scan needs these two and nothing else of the store.)
inline bool isTextureStore(const QString& folderName) {
    return folderName.compare(QLatin1String("textures"), Qt::CaseInsensitive) == 0;
}

/// True for a file the operations below may rename, move and delete: an asset PoseStudio itself
/// writes (a saved material). Content of other applications' libraries is read in place and is
/// never theirs to rearrange.
bool isOwnAsset(const QString& path);

/// What collecting one texture did.
enum class Placed {
    Linked, ///< It lies beside the asset or in its folder's store already: named where it is.
    Reused, ///< A file with the same bytes is in the store: that one is named, nothing copied.
    Copied, ///< Copied (or written) into `textures/<asset name>/`.
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

    /// The folder new files go into: `<assetFolder>/textures/<asset name>`.
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

/// Where the texture a saved asset names as @p source is now ("" = nowhere): see FINDING in the
/// file comment. @p libraryRoot may be empty (an asset outside every library). @p viaName, when
/// given, is set where the file was found by its name and not at the path as written.
QString resolveTexture(const QString& source, const QString& assetFolder, const QString& libraryRoot, bool* viaName = nullptr);

/// The texture sources the saved material @p materialPath names, as written. Empty for a file
/// that is not a material (or cannot be read).
QStringList materialSources(const QString& materialPath);

/// The files those sources name that EXIST and lie in or under the material's own folder: what
/// travels with it. Absolute paths, each once.
QStringList materialOwnFiles(const QString& materialPath);

/// The asset's icons: the image files beside it that carry its name.
QStringList iconsOf(const QString& assetPath);

/// What a file operation did.
struct Result {
    bool        ok = false;
    QString     error;   ///< For people, when !ok.
    QString     path;    ///< The asset where it is now (rename, duplicate, move); the package (export); the folder (install).
    int         files = 0;   ///< Texture files that went along (copied, moved, removed, packed, installed).
    int         reused = 0;  ///< … of a move: textures the destination's store already had.
    QStringList kept;    ///< install: files that were there with other content and were left as they were.
};

/// True when @p name can be an asset's name: not empty, none of \ / : * ? " < > |, no trailing
/// dot or space. @p why says what is wrong when it cannot.
bool validAssetName(const QString& name, QString* why = nullptr);

/// The asset under another name, in its folder: the file, its icons, and the name it carries
/// inside. Its textures stay where they are (it names them by path, not by its own name).
Result renameAsset(const QString& assetPath, const QString& newName);

/// A second asset beside the first — "<name> copy", "<name> copy 2", … — with a copy of the
/// icon, naming the same textures (nothing in the store is copied).
Result duplicateAsset(const QString& assetPath);

/// The asset in another folder: its textures are collected into that folder's store (copied, or
/// the same bytes found there), the asset rewritten to name them, the icons moved, and what the
/// old folder's store then holds for nobody is removed.
Result moveAsset(const QString& assetPath, const QString& destinationFolder);

/// The asset, its icons and the textures of its folder's store that no other asset of the folder
/// names: to the trash (or, @p permanently, removed for good). Emptied store folders go too.
Result deleteAsset(const QString& assetPath, bool permanently = false);

/// Of @p files, removes those that lie in @p folder's texture store and that NO asset of the
/// folder names any more — what an asset saved over itself with other maps leaves behind — to
/// the trash (or, @p permanently, for good), with the store folders that empties. Returns how
/// many went.
int removeUnnamed(const QString& folder, const QStringList& files, bool permanently = false);

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

#endif // ASSETSTORE_H
