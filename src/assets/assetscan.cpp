/**
 * @file assetscan.cpp
 * @brief Implementation of the AssetScan helpers. See the header for the rules they implement.
 */

#include "assetscan.h"

#include <QDirIterator>
#include <QFileInfo>

namespace {
// Extensions that are never listed as assets in their own right: sidecar/companion files that
// belong to another asset rather than being one (e.g. a Wavefront .mtl material library ships
// alongside its .obj). Compared lower-case.
const QSet<QString> kIgnoredAssetExtensions = {"mtl"};

// A picture of a folder listing, as the choice of a thumbnail needs it.
struct ImageFile {
    QString fileName;
    QString stem;      ///< its name without the image extension, lower-cased ("name", "name.duf", "name.tip")
    qint64  bytes = 0;
};

// THE THUMBNAIL RULE (assetscan.h). `images` are the pictures that share the asset's name up to
// the first dot — what made the file an asset. Among them the asset's own come first: a picture
// NAMED AFTER IT, by its name ("Name.png") or its whole file name ("Name.duf.png"), either also
// with ".tip" before the picture's extension ("Name.tip.png": the larger preview some content
// ships beside the small one). The largest of its own is its thumbnail; an asset with none of
// its own wears the largest picture of the group, as every asset did before names with a dot in
// them ("Material.001", "Material.002") were told apart.
QString thumbnailFor(const QString& assetFileName, const QList<ImageFile>& images) {
    const QString whole = assetFileName.toLower();                            // "name.duf"
    const QString stem = QFileInfo(assetFileName).completeBaseName().toLower(); // "name"
    static const QString kTip = QStringLiteral(".tip");
    const ImageFile* own = nullptr;
    const ImageFile* largest = nullptr;
    for (const ImageFile& image : images) {
        if (!largest || image.bytes > largest->bytes) largest = &image;
        const bool named = image.stem == stem || image.stem == whole || image.stem == stem + kTip || image.stem == whole + kTip;
        if (named && (!own || image.bytes > own->bytes)) own = &image;
    }
    return own ? own->fileName : (largest ? largest->fileName : QString());
}

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
    const QFileInfoList entries = QDir(folderPath).entryInfoList(AssetScan::kEntryFilter, QDir::NoSort);

    // (Whether a file IS an asset is the first-dot question alone — a picture shares its name up
    // to the first dot; which picture it then wears is thumbnailFor's and changes no state.)
    QSet<QString> images, nonImages;
    for (const QFileInfo& f : entries) {
        if (f.isDir() || HiddenNames::isHidden(f.fileName())) continue;
        const QString suffix = f.suffix().toLower();
        if (kIgnoredAssetExtensions.contains(suffix)) continue; // sidecar file, not an asset
        const QString base = f.baseName();
        if (AssetScan::isImageSuffix(suffix)) {
            images.insert(base);
            if (nonImages.contains(base)) { out.insert(folderPath, HitState::DirectHit); return HitState::DirectHit; }
        } else {
            nonImages.insert(base);
            if (images.contains(base)) { out.insert(folderPath, HitState::DirectHit); return HitState::DirectHit; }
        }
    }

    for (const QFileInfo& sub : entries) {
        if (!sub.isDir()) continue;
        if (HiddenNames::isHidden(sub.fileName())) continue; // a hidden folder: nothing in it is listed
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
        if (!HiddenNames::isHidden(it.fileName())) return true;
    }
    return false;
}

QFileInfoList listSubdirectories(const QString& path) {
    QFileInfoList folders = QDir(path).entryInfoList(kSubdirFilter, QDir::Name | QDir::IgnoreCase);
    // A folder whose name begins with a dot is hidden (core/hiddennames.h): not a place to
    // browse. (An asset's texture store, `.textures`, is one: assetstore.h.)
    folders.removeIf([](const QFileInfo& folder) { return HiddenNames::isHidden(folder.fileName()); });
    return folders;
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

QHash<QString, HitState> classifyFolder(const QString& folderPath,
                                        const QHash<QString, HitState>& known) {
    QHash<QString, HitState> out;
    classifyInto(folderPath, known, out);
    return out;
}

QList<AssetHit> scanFolderAssets(const QString& folderPath) {
    const QFileInfoList files = QDir(folderPath).entryInfoList(kFileFilter);

    // The files that share a name up to the first dot: the assets among them and the pictures
    // one of which each asset wears (thumbnailFor).
    struct FileGroup {
        QList<QFileInfo> nonImages;
        QList<ImageFile> images;
    };
    QHash<QString, FileGroup> groups;
    groups.reserve(files.size());

    for (const QFileInfo& fi : files) {
        if (HiddenNames::isHidden(fi.fileName())) continue; // hidden: neither an asset nor a thumbnail
        const QString suffix = fi.suffix().toLower();
        if (kIgnoredAssetExtensions.contains(suffix)) continue; // sidecar file (e.g. .mtl), not a listable asset
        const QString base = fi.baseName();
        if (AssetScan::isImageSuffix(suffix)) {
            groups[base].images.append({fi.fileName(), fi.completeBaseName().toLower(), fi.size()});
        } else {
            groups[base].nonImages.append(fi);
        }
    }

    QList<AssetHit> finalHits;
    finalHits.reserve(groups.size());
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const FileGroup& g = it.value();
        if (!g.images.isEmpty() && !g.nonImages.isEmpty()) {
            for (const QFileInfo& nonImg : g.nonImages) {
                AssetHit hit;
                hit.folderPath = folderPath;
                hit.assetFileName = nonImg.fileName();
                hit.bestImage = thumbnailFor(hit.assetFileName, g.images);
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

        // Single scan of this folder: the pictures per asset basename (the text before the
        // first dot), and the asset files' own stat data.
        QHash<QString, QList<ImageFile>> imagesByBase;
        QList<QPair<QString, AssetHit>> folderHits; // (stored full path, hit) found in this folder
        const QFileInfoList allFiles = QDir(folderPath).entryInfoList(kFileFilter);
        for (const QFileInfo& fi : allFiles) {
            const QString base = fi.baseName().toLower();
            if (!relevantBases.contains(base)) continue;
            if (isImageSuffix(fi.suffix().toLower())) {
                imagesByBase[base].append({fi.fileName(), fi.completeBaseName().toLower(), fi.size()});
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
            if (igIt != imagesByBase.cend()) hit.bestImage = thumbnailFor(hit.assetFileName, *igIt);
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
