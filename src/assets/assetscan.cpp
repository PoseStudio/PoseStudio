/**
 * @file assetscan.cpp
 * @brief Implementation of the AssetScan helpers. See the header for the rules they implement.
 */

#include "assetscan.h"

#include "assetstore.h"

#include <QDirIterator>
#include <QFileInfo>

namespace {
// Image file extensions treated as thumbnail candidates throughout the asset manager. An asset
// (any non-image file) is paired with the largest same-basename image in its folder.
const QSet<QString> kImageExtensions = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};

// Extensions that are never listed as assets in their own right: sidecar/companion files that
// belong to another asset rather than being one (e.g. a Wavefront .mtl material library ships
// alongside its .obj). Compared lower-case.
const QSet<QString> kIgnoredAssetExtensions = {"mtl"};

// Recursive body of classifyFolder: `out` collects every state established during this walk
// (consulted before `known` so a folder reached twice in one walk is classified once).
AssetScan::HitState classifyInto(const QString& folderPath,
                                 const QHash<QString, AssetScan::HitState>& known,
                                 QHash<QString, AssetScan::HitState>& out) {
    using AssetScan::HitState;
    if (const auto it = out.constFind(folderPath); it != out.cend()) return it.value();
    if (const auto it = known.constFind(folderPath); it != known.cend()) return it.value();

    // ONE listing per folder: files and subdirectories together. The old code listed files
    // first and directories second, doubling the syscalls on every folder without a direct hit.
    const QFileInfoList entries = QDir(folderPath).entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::NoSort);

    QSet<QString> images, nonImages;
    for (const QFileInfo& f : entries) {
        if (f.isDir()) continue;
        const QString suffix = f.suffix().toLower();
        if (kIgnoredAssetExtensions.contains(suffix)) continue; // sidecar file, not an asset
        const QString base = f.baseName();
        if (kImageExtensions.contains(suffix)) {
            images.insert(base);
            if (nonImages.contains(base)) { out.insert(folderPath, HitState::DirectHit); return HitState::DirectHit; }
        } else {
            nonImages.insert(base);
            if (images.contains(base)) { out.insert(folderPath, HitState::DirectHit); return HitState::DirectHit; }
        }
    }

    for (const QFileInfo& sub : entries) {
        if (!sub.isDir()) continue;
        if (AssetStore::isTextureStore(sub.fileName())) continue; // an asset's textures: never assets
        if (classifyInto(sub.absoluteFilePath(), known, out) != HitState::NoHit) {
            out.insert(folderPath, HitState::IndirectHit);
            return HitState::IndirectHit;
        }
    }

    out.insert(folderPath, HitState::NoHit);
    return HitState::NoHit;
}
} // namespace

namespace AssetScan {

QString normalizePath(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool samePath(const QString& a, const QString& b) {
    return normalizePath(a).compare(normalizePath(b), kPathCase) == 0;
}

bool folderWithinLibrary(const QString& folderPath, const QString& libRoot) {
    const QString root = normalizePath(libRoot);
    const QString folder = normalizePath(folderPath);
    if (folder.compare(root, kPathCase) == 0) return true;
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return folder.startsWith(prefix, kPathCase);
}

QStringList pathsWithin(const QStringList& assetPaths, const QStringList& roots) {
    QStringList within;
    for (const QString& path : assetPaths) {
        for (const QString& root : roots) {
            if (folderWithinLibrary(path, root)) {
                within.append(path);
                break;
            }
        }
    }
    return within;
}

QString folderDisplayName(const QString& path) {
    const QString name = QDir(path).dirName();
    return name.isEmpty() ? QDir::toNativeSeparators(QDir(path).absolutePath()) : name;
}

bool hasSubdirectories(const QString& path) {
    QDirIterator it(path, kSubdirFilter, QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        it.next();
        if (!AssetStore::isTextureStore(it.fileName())) return true;
    }
    return false;
}

QFileInfoList listSubdirectories(const QString& path) {
    QFileInfoList folders = QDir(path).entryInfoList(kSubdirFilter, QDir::Name | QDir::IgnoreCase);
    // A `textures` folder is an asset's texture store (assetstore.h): the pictures the assets
    // beside it sample, never assets. It is not a place to browse.
    folders.removeIf([](const QFileInfo& folder) { return AssetStore::isTextureStore(folder.fileName()); });
    return folders;
}

bool inTextureStore(const QString& path, const QString& root) {
    const QString relative = QDir(root).relativeFilePath(path);
    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (AssetStore::isTextureStore(part)) return true;
    }
    return false;
}

bool hasAncestorIn(const QString& path, const QSet<QString>& set) {
    QString ancestor = path;
    for (;;) {
        const QString parent = QFileInfo(ancestor).path();
        if (parent.isEmpty() || parent == ancestor) return false; // reached the root
        if (set.contains(parent)) return true;
        ancestor = parent;
    }
}

QString truncateSearchPath(const QString& relPath, int maxSegments) {
    QStringList segments = relPath.split('/', Qt::SkipEmptyParts);
    if (segments.size() > maxSegments) {
        segments = segments.mid(segments.size() - maxSegments);
        return QStringLiteral("... / ") + segments.join(" / ");
    }
    return segments.join(" / ");
}

bool folderHasDirectAssetHit(const QString& folderPath) {
    const QFileInfoList files = QDir(folderPath).entryInfoList(kFileFilter, QDir::NoSort);

    QSet<QString> images, nonImages;
    for (const QFileInfo& f : files) {
        const QString suffix = f.suffix().toLower();
        if (kIgnoredAssetExtensions.contains(suffix)) continue; // sidecar file, not an asset
        const QString base = f.baseName();
        if (kImageExtensions.contains(suffix)) {
            images.insert(base);
            if (nonImages.contains(base)) return true;
        } else {
            nonImages.insert(base);
            if (images.contains(base)) return true;
        }
    }
    return false;
}

QHash<QString, HitState> classifyFolder(const QString& folderPath,
                                        const QHash<QString, HitState>& known) {
    QHash<QString, HitState> out;
    classifyInto(folderPath, known, out);
    return out;
}

QList<AssetHit> scanFolderAssets(const QString& folderPath) {
    const QFileInfoList files = QDir(folderPath).entryInfoList(kFileFilter);

    // Only the LARGEST same-basename image is kept: it's the one thumbnail the grid shows,
    // so tracking the runners-up was pure allocation for data nothing read.
    struct FileGroup {
        QList<QFileInfo> nonImages;
        QString bestImage;
        qint64 maxBytes = -1;
    };
    QHash<QString, FileGroup> groups;
    groups.reserve(files.size());

    for (const QFileInfo& fi : files) {
        const QString suffix = fi.suffix().toLower();
        if (kIgnoredAssetExtensions.contains(suffix)) continue; // sidecar file (e.g. .mtl), not a listable asset
        const QString base = fi.baseName();
        if (kImageExtensions.contains(suffix)) {
            const qint64 sz = fi.size();
            auto& g = groups[base];
            if (sz > g.maxBytes) {
                g.bestImage = fi.fileName();
                g.maxBytes = sz;
            }
        } else {
            groups[base].nonImages.append(fi);
        }
    }

    QList<AssetHit> finalHits;
    finalHits.reserve(groups.size());
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const FileGroup& g = it.value();
        if (!g.bestImage.isEmpty() && !g.nonImages.isEmpty()) {
            for (const QFileInfo& nonImg : g.nonImages) {
                AssetHit hit;
                hit.folderPath = folderPath;
                hit.assetFileName = nonImg.fileName();
                hit.bestImage = g.bestImage;
                hit.sizeBytes = nonImg.size();
                hit.lastModified = nonImg.lastModified();
                finalHits.append(std::move(hit));
            }
        }
    }
    return finalHits;
}

QList<AssetHit> resolveAssetHits(const QStringList& assetPaths) {
    // Group the paths by folder so each folder is listed only once. Keep the original full
    // paths as the keys so we can re-emit in input order at the end. No stat here: whether a
    // file still exists is answered by the folder listing below, which we need anyway.
    QHash<QString, QStringList> folderToPaths;
    for (const QString& fullPath : assetPaths)
        folderToPaths[QFileInfo(fullPath).absolutePath()].append(fullPath);

    QHash<QString, AssetHit> hitByPath;
    for (auto folderIt = folderToPaths.cbegin(); folderIt != folderToPaths.cend(); ++folderIt) {
        const QString& folderPath = folderIt.key();
        const QStringList& folderPaths = folderIt.value();

        // Build the set of relevant basenames (for thumbnails) and file names (for the assets
        // themselves) so the directory scan stays focused. Keys are lower-cased to honour the
        // module's case policy.
        QSet<QString> relevantBases;
        QHash<QString, QString> relevantNames; // lower-cased file name -> stored full path
        relevantBases.reserve(folderPaths.size());
        for (const QString& p : folderPaths) {
            const QFileInfo fi(p);
            relevantBases.insert(fi.baseName().toLower());
            relevantNames.insert(fi.fileName().toLower(), p);
        }

        // Single scan of this folder: the best (largest) image per asset basename, and the
        // asset files' own stat data.
        struct ImageGroup { QString bestImage; qint64 maxBytes = -1; };
        QHash<QString, ImageGroup> imagesByBase;
        QList<QPair<QString, AssetHit>> folderHits; // (stored full path, hit) found in this folder
        const QFileInfoList allFiles = QDir(folderPath).entryInfoList(kFileFilter);
        for (const QFileInfo& fi : allFiles) {
            const QString base = fi.baseName().toLower();
            if (!relevantBases.contains(base)) continue;
            if (kImageExtensions.contains(fi.suffix().toLower())) {
                const qint64 sz = fi.size();
                auto& ig = imagesByBase[base];
                if (sz > ig.maxBytes) {
                    ig.bestImage = fi.fileName();
                    ig.maxBytes = sz;
                }
                continue;
            }
            const auto nameIt = relevantNames.constFind(fi.fileName().toLower());
            if (nameIt == relevantNames.cend()) continue;
            AssetHit hit;
            hit.folderPath = folderPath;
            hit.assetFileName = fi.fileName();
            hit.sizeBytes = fi.size();
            hit.lastModified = fi.lastModified();
            folderHits.append({nameIt.value(), std::move(hit)});
        }

        for (auto& [storedPath, hit] : folderHits) {
            const auto igIt = imagesByBase.constFind(QFileInfo(hit.assetFileName).baseName().toLower());
            if (igIt != imagesByBase.cend()) hit.bestImage = igIt->bestImage;
            hitByPath.insert(storedPath, std::move(hit));
        }
    }

    // Re-emit in the caller's original input order; paths not found on disk are dropped.
    QList<AssetHit> finalHits;
    finalHits.reserve(hitByPath.size());
    for (const QString& fullPath : assetPaths) {
        const auto it = hitByPath.constFind(fullPath);
        if (it != hitByPath.cend()) finalHits.append(it.value());
    }
    return finalHits;
}

} // namespace AssetScan
