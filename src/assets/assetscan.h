/**
 * @file assetscan.h
 * @brief Filesystem scanning for the Asset Manager: what counts as an asset, how folders are
 *        classified, how paths are compared. QtCore only, no widgets, no database.
 *
 * The one rule everything here implements: an ASSET is a non-image file (sidecar extensions
 * such as .mtl excluded) that shares its basename with an image file in the same folder; that
 * image is the asset's thumbnail, and when several qualify the LARGEST wins. A folder that
 * holds such a pair is a DirectHit, one that only has a DirectHit somewhere below it is an
 * IndirectHit, and anything else is NoHit (greyed in the tree). Every scan site in the module
 * (tree lazy-load, grid subfolders, search, hit classification) goes through the filters and
 * helpers below so the symlink and case policies are decided exactly once:
 *
 * - Symlinked/junctioned directories are NEVER followed (kSubdirFilter carries NoSymLinks).
 *   The recursive walkers have no cycle protection (QDirIterator doesn't either), and before
 *   this header the tree included such folders while the grid/search/hit walk excluded them,
 *   so a junction showed in the tree but never as a grid folder and painted its parent "empty".
 * - Paths compare CASE-INSENSITIVELY everywhere (kPathCase). Stored library paths and scanned
 *   paths come from the same QDir/QFileDialog sources, so the only way they differ by case is
 *   the user retyping one; a case-sensitive compare then silently broke breadcrumbs and
 *   "Find In Library" on Windows, where the filesystem itself is case-insensitive.
 *
 * The classification (classifyFolder) is a pure function of the filesystem so the proxy model
 * can run it on a worker thread; it never touches Qt GUI types.
 */

#ifndef ASSETSCAN_H
#define ASSETSCAN_H

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
    QString bestImage;      ///< Filename of the paired thumbnail (largest same-basename image), or empty
    qint64 sizeBytes = 0;   ///< Asset file size
    QDateTime lastModified; ///< Asset file mtime
};

/// How a folder relates to assets (see the file banner). The numeric order matters to nobody;
/// the names do: only DirectHit folders become search results and list assets in the grid.
enum class HitState { NoHit = 0, IndirectHit = 1, DirectHit = 2 };

/// The subdirectory filter EVERY scan uses: real directories only, never symlinks/junctions.
inline constexpr QDir::Filters kSubdirFilter = QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks;
/// The file filter every scan uses.
inline constexpr QDir::Filters kFileFilter = QDir::Files | QDir::NoSymLinks;

/// The module-wide case policy for comparing paths (see the file banner).
inline constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;

/// cleanPath(fromNativeSeparators(path)): the canonical spelling every comparison works on.
QString normalizePath(const QString& path);

/// True when the two paths name the same location under the module's case policy.
bool samePath(const QString& a, const QString& b);

/// True when `folderPath` IS `libRoot` or lies under it. Both are normalized first: a library
/// registered at a drive root keeps its trailing slash ("D:/"), where a naive `lib + "/"`
/// prefix test builds "D://" and never matches, silently breaking breadcrumbs and
/// "Find In Library" for every subfolder of that library.
bool folderWithinLibrary(const QString& folderPath, const QString& libRoot);

/// The members of `assetPaths` that lie in or under one of `roots`, in input order: what a
/// browser that looks at part of the disk shows of a list kept for the whole application
/// (Favorites, in the Materials Library).
QStringList pathsWithin(const QStringList& assetPaths, const QStringList& roots);

/// Display name for a folder: its dirName(), or the path itself for a drive root ("D:/"), whose
/// dirName() is empty. A drive-root library would otherwise render as a nameless tree row.
QString folderDisplayName(const QString& path);

/// True when `path` has at least one real (non-symlink) subdirectory that is listed (a texture
/// store is not): decides whether a tree row gets the "..." lazy-load placeholder that makes it
/// expandable.
bool hasSubdirectories(const QString& path);

/// The real subdirectories of `path`, sorted by name, case-insensitive: the order the tree
/// and the grid list them in. Texture stores (folders named `textures`) are left out.
QFileInfoList listSubdirectories(const QString& path);

/// True when `path`, below `root`, is a texture store or lies inside one (a folder named
/// `textures`, assetstore.h): the tree and the grid never list such a folder
/// (hasSubdirectories / listSubdirectories / classifyFolder leave it out), and the search
/// passes over what is in it.
bool inTextureStore(const QString& path, const QString& root);

/// True if any ancestor directory of `path` is present in `set`. Walks up with
/// QFileInfo::path() so a drive-root ancestor ("D:/") is reached: truncating at the last '/'
/// turned "D:/x" into "D:", which never matched a root library stored as "D:/". The set holds
/// paths produced by the same scan as `path`, so membership is exact-case by design.
bool hasAncestorIn(const QString& path, const QSet<QString>& set);

/// Caps a search result's displayed path to its last `maxSegments` segments, prefixing "..."
/// when truncated, joined with " / " (the separator AssetTreeDelegate greys the prefix of).
QString truncateSearchPath(const QString& relPath, int maxSegments = 4);

/// True when `folderPath` directly contains at least one asset (a non-image file with a
/// same-basename image beside it).
bool folderHasDirectAssetHit(const QString& folderPath);

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

/// The assets directly inside `folderPath` (non-recursive), in directory order: each non-image
/// file paired with the largest same-basename image. Files without an image are not assets.
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
