// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetstore.cpp
 * @brief Implementation of the asset store proper (assetstore.h): collecting an asset's
 *        textures beside it, finding them again, and reading what an asset names. The file
 *        operations are assetfileops.cpp, the packages assetpackage.cpp.
 */

#include "assetstore.h"

#include "assetstore_detail.h"
#include "importformats.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <utility>

using namespace AssetStore::detail;

namespace {

/// What one resolver's searches by file name may visit before they give up: a library on a
/// slow drive must not hold a load for minutes because a map has gone.
constexpr int kNameSearchBudget = 250000;

bool sameContent(const QString& a, const QString& b) {
    QFile fa(a);
    QFile fb(b);
    if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly) || fa.size() != fb.size()) {
        return false;
    }
    while (!fa.atEnd()) {
        const QByteArray ca = fa.read(kChunk);
        const QByteArray cb = fb.read(kChunk);
        if (ca.isEmpty() || ca != cb) {
            return false;
        }
    }
    return fb.atEnd();
}

bool sameContent(const QString& a, const QByteArray& bytes) {
    QByteArray on;
    return readAll(a, on) && on == bytes;
}

/// The asset's name as a folder's: Windows drops a folder name's trailing dots and spaces.
QString storeName(const QString& assetName) {
    QString name = assetName.trimmed();
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("asset") : name;
}

} // namespace

namespace AssetStore {

bool isOwnAsset(const QString& path) {
    return pose::ImportFormats::isMaterialFile(path);
}

// ============================================================================
// COLLECTING
// ============================================================================

TextureCollector::TextureCollector(const QString& assetFolder, const QString& assetName)
    : m_folder(clean(QDir(assetFolder).absolutePath())), m_name(storeName(assetName)) {}

QString TextureCollector::ownFolder() const {
    return storeRoot(m_folder) + QLatin1Char('/') + m_name;
}

QString TextureCollector::relative(const QString& absolutePath) const {
    return QDir(m_folder).relativeFilePath(absolutePath);
}

void TextureCollector::indexStore() {
    if (m_indexed) {
        return;
    }
    m_indexed = true;
    QDirIterator it(storeRoot(m_folder), kEveryFile, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        m_bySize[it.fileInfo().size()].append(clean(it.filePath()));
    }
}

QString TextureCollector::findSame(qint64 size, const QString& sourcePath, const QByteArray* bytes) const {
    const QStringList candidates = m_bySize.value(size);
    for (const QString& candidate : candidates) {
        if (bytes != nullptr ? sameContent(candidate, *bytes) : sameContent(candidate, sourcePath)) {
            return candidate;
        }
    }
    return {};
}

QString TextureCollector::freeName(const QString& fileName) const {
    const QFileInfo info(fileName);
    const QString   base = info.completeBaseName().isEmpty() ? QStringLiteral("texture") : info.completeBaseName();
    const QString   suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    const QDir      folder(ownFolder());
    QString         name = base + suffix;
    for (int n = 2; folder.exists(name); ++n) {
        name = base + QLatin1Char('_') + QString::number(n) + suffix;
    }
    return name;
}

QString TextureCollector::addFile(const QString& sourcePath, Placed* how) {
    const auto report = [how](Placed placed) {
        if (how != nullptr) {
            *how = placed;
        }
    };
    const QFileInfo info(sourcePath);
    if (!info.isFile()) {
        report(Placed::Failed);
        return {};
    }
    const QString absolute = clean(info.absoluteFilePath());
    const QString id = key(absolute);
    if (const auto it = m_done.constFind(id); it != m_done.cend()) {
        report(m_how.value(id, Placed::Linked));
        return it.value();
    }

    QString written;
    Placed  placed = Placed::Linked;
    // Beside the asset, or in its folder's store: it is where it belongs.
    if (samePath(info.absolutePath(), m_folder) || within(absolute, storeRoot(m_folder))) {
        written = relative(absolute);
    } else {
        indexStore();
        const QString same = findSame(info.size(), absolute, nullptr);
        if (!same.isEmpty()) {
            written = relative(same);
            placed = Placed::Reused;
        } else {
            if (!QDir().mkpath(ownFolder())) {
                report(Placed::Failed);
                return {};
            }
            const QString target = ownFolder() + QLatin1Char('/') + freeName(info.fileName());
            if (!QFile::copy(absolute, target)) {
                report(Placed::Failed);
                return {};
            }
            makeWritable(target);
            m_bySize[info.size()].append(target);
            written = relative(target);
            placed = Placed::Copied;
        }
    }
    m_done.insert(id, written);
    m_how.insert(id, placed);
    report(placed);
    return written;
}

QString TextureCollector::addBytes(const QByteArray& bytes, const QString& fileName, Placed* how) {
    const auto report = [how](Placed placed) {
        if (how != nullptr) {
            *how = placed;
        }
    };
    if (bytes.isEmpty()) {
        report(Placed::Failed);
        return {};
    }
    indexStore();
    const QString same = findSame(bytes.size(), QString(), &bytes);
    if (!same.isEmpty()) {
        report(Placed::Reused);
        return relative(same);
    }
    if (!QDir().mkpath(ownFolder())) {
        report(Placed::Failed);
        return {};
    }
    const QString target = ownFolder() + QLatin1Char('/') + freeName(fileName);
    if (!writeAll(target, bytes)) {
        report(Placed::Failed);
        return {};
    }
    m_bySize[bytes.size()].append(target);
    report(Placed::Copied);
    return relative(target);
}

// ============================================================================
// FINDING
// ============================================================================

namespace {

/// Every file under @p root, in the order the drive lists them, for as long as @p budget lasts.
QStringList filesUnder(const QString& root, int& budget) {
    QStringList files;
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        return files;
    }
    QDirIterator it(root, kEveryFile, QDirIterator::Subdirectories); // (the stores are hidden folders)
    while (it.hasNext() && budget > 0) {
        it.next();
        --budget;
        files.append(clean(it.filePath()));
    }
    return files;
}

/// True when the file @p path is named @p name (any case).
bool isNamed(const QString& path, const QString& name) {
    return QStringView(path).mid(path.lastIndexOf(QLatin1Char('/')) + 1).compare(name, Qt::CaseInsensitive) == 0;
}

/// The file named @p name (any case) among @p files that lies nearest @p nearTo, or "".
QString nearestByName(const QStringList& files, const QString& name, const QString& nearTo) {
    const QStringList nearParts = key(nearTo).split(QLatin1Char('/'));
    QString           best;
    int               bestShared = -1;
    for (const QString& found : files) {
        if (!isNamed(found, name)) {
            continue;
        }
        const QStringList parts = key(found).split(QLatin1Char('/'));
        int               shared = 0;
        while (shared < parts.size() && shared < nearParts.size() && parts[shared] == nearParts[shared]) {
            ++shared;
        }
        // (The nearest; of equally near ones the first by name, so the answer does not depend
        // on the order the drive lists its folders in.)
        if (shared > bestShared || (shared == bestShared && found.compare(best, Qt::CaseInsensitive) < 0)) {
            best = found;
            bestShared = shared;
        }
    }
    return best;
}

} // namespace

TextureResolver::TextureResolver(const QString& assetFolder, const QString& libraryRoot)
    : m_folder(assetFolder), m_library(libraryRoot), m_budget(kNameSearchBudget) {}

QString resolveTexture(const QString& source, const QString& assetFolder, const QString& libraryRoot, bool* viaName) {
    return TextureResolver(assetFolder, libraryRoot).resolve(source, viaName);
}

QString TextureResolver::resolve(const QString& source, bool* viaName) {
    const QString& assetFolder = m_folder;
    const QString& libraryRoot = m_library;
    if (viaName != nullptr) {
        *viaName = false;
    }
    const QString written = portable(source);
    if (written.isEmpty()) {
        return {};
    }
    const bool isRelative = QDir::isRelativePath(written);
    if (isRelative) {
        const QString beside = clean(QDir(assetFolder).absoluteFilePath(written));
        if (QFileInfo(beside).isFile()) {
            return beside;
        }
        if (!libraryRoot.isEmpty()) {
            const QString inLibrary = clean(QDir(libraryRoot).absoluteFilePath(written));
            if (QFileInfo(inLibrary).isFile()) {
                return inLibrary;
            }
        }
    } else if (QFileInfo(written).isFile()) {
        return clean(written);
    }

    // Not where it was written to be: by its NAME — beside the asset, in the asset's store, then
    // anywhere under the library's root.
    const QString name = QFileInfo(written).fileName();
    if (name.isEmpty()) {
        return {};
    }
    QString found;
    if (!m_besideListed) {
        m_besideListed = true;
        const QFileInfoList files = QDir(assetFolder).entryInfoList(AssetScan::kFileFilter);
        for (const QFileInfo& f : files) {
            m_beside.append(clean(f.absoluteFilePath()));
        }
    }
    for (const QString& beside : std::as_const(m_beside)) {
        if (isNamed(beside, name)) {
            found = beside;
            break;
        }
    }
    if (found.isEmpty()) {
        if (!m_storeWalked) {
            m_storeWalked = true;
            m_inStore = filesUnder(storeRoot(assetFolder), m_budget);
        }
        found = nearestByName(m_inStore, name, assetFolder);
    }
    if (found.isEmpty() && !libraryRoot.isEmpty()) {
        if (!m_libraryWalked) {
            m_libraryWalked = true;
            m_inLibrary = filesUnder(clean(libraryRoot), m_budget);
        }
        found = nearestByName(m_inLibrary, name, assetFolder);
    }
    if (!found.isEmpty() && viaName != nullptr) {
        *viaName = true;
    }
    return found;
}

// ============================================================================
// WHAT AN ASSET NAMES
// ============================================================================

QStringList materialSources(const QString& materialPath, bool* readable) {
    QStringList sources;
    json        root;
    const json* textures = readJson(materialPath, root) ? materialTextures(root) : nullptr;
    if (readable != nullptr) {
        *readable = textures != nullptr;
    }
    if (textures == nullptr) {
        return sources;
    }
    for (const json& entry : *textures) {
        const QString source = sourceOf(entry);
        if (!source.isEmpty()) {
            sources.append(source);
        }
    }
    return sources;
}

QStringList materialOwnFiles(const QString& materialPath, bool* readable) {
    QStringList   files;
    QSet<QString> seen;
    const QString folder = clean(QFileInfo(materialPath).absolutePath());
    for (const QString& source : materialSources(materialPath, readable)) {
        const QString written = portable(source);
        const QString absolute = clean(QDir::isRelativePath(written) ? QDir(folder).absoluteFilePath(written) : written);
        if (!QFileInfo(absolute).isFile() || !within(absolute, folder) || seen.contains(key(absolute))) {
            continue;
        }
        seen.insert(key(absolute));
        files.append(absolute);
    }
    return files;
}

QStringList iconsOf(const QString& assetPath) {
    QStringList     icons;
    const QFileInfo info(assetPath);
    const QString   name = info.completeBaseName();
    const QFileInfoList files = info.absoluteDir().entryInfoList(AssetScan::kFileFilter, QDir::Name);
    for (const QFileInfo& f : files) {
        // (A picture, by the library browser's own list: an image beside an asset is its icon.)
        if (AssetScan::isImageSuffix(f.suffix().toLower()) && f.completeBaseName().compare(name, Qt::CaseInsensitive) == 0) {
            icons.append(clean(f.absoluteFilePath()));
        }
    }
    return icons;
}

} // namespace AssetStore
