/**
 * @file assetstore.cpp
 * @brief Implementation of the asset store (assetstore.h): collecting an asset's textures
 *        beside it, finding them again, and the file operations that keep the two together.
 */

#include "assetstore.h"

#include "importformats.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <string>

namespace {

using nlohmann::json;

/// What a recursive search by file name may visit before it gives up: a library on a slow
/// drive must not hold a load for minutes because one map has gone.
constexpr int kNameSearchBudget = 250000;
constexpr qint64 kChunk = 1 << 20;

const QSet<QString>& imageExtensions() {
    // (The library browser's own list, assetscan.cpp: an image beside an asset is its icon.)
    static const QSet<QString> set = {QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("bmp"),
                                      QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("tif"),  QStringLiteral("tiff")};
    return set;
}

/// Formats whose bytes are compressed already: stored in a package as they are.
bool isCompressedFormat(const QString& suffix) {
    static const QSet<QString> set = {QStringLiteral("png"), QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("webp"),
                                      QStringLiteral("gif"), QStringLiteral("exr"),  QStringLiteral("zip"),  QStringLiteral("duf"),
                                      QStringLiteral("dsf"), QStringLiteral("glb"),  QStringLiteral("usdz"), QStringLiteral("spz")};
    return set.contains(suffix.toLower());
}

QString clean(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

/// A path as it is compared: cleaned, case folded (the library's case policy, assetscan.h).
QString key(const QString& path) {
    return clean(path).toLower();
}

bool samePath(const QString& a, const QString& b) {
    return key(a) == key(b);
}

/// True when @p path IS @p folder or lies under it.
bool within(const QString& path, const QString& folder) {
    const QString p = key(path);
    const QString f = key(folder);
    if (p == f) {
        return true;
    }
    return p.startsWith(f.endsWith(QLatin1Char('/')) ? f : f + QLatin1Char('/'));
}

bool readAll(const QString& path, QByteArray& out) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    out = in.readAll();
    return in.error() == QFileDevice::NoError;
}

bool writeAll(const QString& path, const QByteArray& bytes) {
    QSaveFile out(path); // (whole or not at all)
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.commit();
}

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

mz_ulong fileCrc(const QString& path, bool* ok) {
    QFile in(path);
    *ok = in.open(QIODevice::ReadOnly);
    mz_ulong crc = MZ_CRC32_INIT;
    while (*ok && !in.atEnd()) {
        const QByteArray chunk = in.read(kChunk);
        if (chunk.isEmpty()) {
            break;
        }
        crc = mz_crc32(crc, reinterpret_cast<const unsigned char*>(chunk.constData()), static_cast<size_t>(chunk.size()));
    }
    return crc;
}

/// A copy must be the user's to remove again, whatever the source's attributes were.
void makeWritable(const QString& path) {
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::WriteOwner | QFileDevice::WriteUser);
}

/// The asset's name as a folder's: Windows drops a folder name's trailing dots and spaces.
QString storeName(const QString& assetName) {
    QString name = assetName.trimmed();
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("asset") : name;
}

/// A file read as a JSON object (false: not there, not JSON, not an object).
bool readJson(const QString& path, json& root) {
    QByteArray bytes;
    if (!readAll(path, bytes)) {
        return false;
    }
    root = json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, /*allow_exceptions=*/false);
    return root.is_object();
}

bool writeJson(const QString& path, const json& root) {
    try {
        const std::string text = root.dump(2) + "\n"; // (materialFileText's own form)
        return writeAll(path, QByteArray(text.data(), static_cast<qsizetype>(text.size())));
    } catch (const json::exception&) {
        return false;
    }
}

/// The texture list of a saved material's JSON, or null when @p root is not one.
json* materialTextures(json& root) {
    if (!root.is_object() || !root.contains("format") || !root["format"].is_string() ||
        root["format"].get<std::string>() != "posestudio.material" || !root.contains("material") || !root["material"].is_object()) {
        return nullptr;
    }
    json& material = root["material"];
    if (!material.contains("textures") || !material["textures"].is_array()) {
        material["textures"] = json::array();
    }
    return &material["textures"];
}

bool isMaterial(json& root) {
    return materialTextures(root) != nullptr;
}

QString sourceOf(const json& entry) {
    if (!entry.is_object() || !entry.contains("source") || !entry["source"].is_string()) {
        return {};
    }
    return QString::fromStdString(entry["source"].get<std::string>());
}

/// A source as a path of THIS system: a file written on Windows may name `textures\a.png`.
QString portable(const QString& source) {
    QString s = source;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return s;
}

QString storeRoot(const QString& folder) {
    return clean(folder) + QLatin1Char('/') + AssetStore::texturesFolderName();
}

/// Every store file the OTHER own assets of @p folder name (keys), so that a texture is removed
/// only when nobody is left to name it.
QSet<QString> namedByOthers(const QString& folder, const QString& exceptAsset) {
    QSet<QString> named;
    const QFileInfoList files = QDir(folder).entryInfoList(QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& f : files) {
        if (!AssetStore::isOwnAsset(f.fileName()) || samePath(f.absoluteFilePath(), exceptAsset)) {
            continue;
        }
        for (const QString& file : AssetStore::materialOwnFiles(f.absoluteFilePath())) {
            named.insert(key(file));
        }
    }
    return named;
}

bool removeFile(const QString& path, bool permanently) {
    if (!QFileInfo::exists(path)) {
        return true;
    }
    makeWritable(path);
    return permanently ? QFile::remove(path) : QFile::moveToTrash(path);
}

/// Removes the folders the removal of @p file emptied, up to and including the store itself.
void pruneEmptyFolders(const QString& file, const QString& store) {
    QString dir = QFileInfo(file).absolutePath();
    while (within(dir, store)) {
        if (!QDir().rmdir(dir)) {
            break; // not empty (or not ours to remove)
        }
        if (samePath(dir, store)) {
            break;
        }
        dir = QFileInfo(dir).absolutePath();
    }
}

AssetStore::Result failed(const QString& why) {
    AssetStore::Result r;
    r.error = why;
    return r;
}

/// The deepest folder all of @p paths lie in or under.
QString commonFolder(const QStringList& folders) {
    if (folders.isEmpty()) {
        return {};
    }
    QStringList common = clean(folders.first()).split(QLatin1Char('/'));
    for (const QString& folder : folders) {
        const QStringList parts = clean(folder).split(QLatin1Char('/'));
        int               same = 0;
        while (same < common.size() && same < parts.size() && common[same].compare(parts[same], Qt::CaseInsensitive) == 0) {
            ++same;
        }
        common = common.mid(0, same);
    }
    QString joined = common.join(QLatin1Char('/'));
    if (joined.endsWith(QLatin1Char(':'))) {
        joined += QLatin1Char('/'); // a drive: "D:" is the drive's CURRENT folder, "D:/" its root
    }
    return joined;
}

bool isClutter(const QString& fileName) {
    return fileName.compare(QLatin1String("Thumbs.db"), Qt::CaseInsensitive) == 0 ||
           fileName.compare(QLatin1String("desktop.ini"), Qt::CaseInsensitive) == 0 || fileName == QLatin1String(".DS_Store");
}

// --- the zip's I/O through Qt (miniz's own file functions take paths in the system code page) ---

struct ZipSink {
    QSaveFile* file = nullptr;
};

size_t zipWrite(void* opaque, mz_uint64 offset, const void* buffer, size_t count) {
    auto* sink = static_cast<ZipSink*>(opaque);
    if (sink->file->pos() != static_cast<qint64>(offset) && !sink->file->seek(static_cast<qint64>(offset))) {
        return 0;
    }
    const qint64 written = sink->file->write(static_cast<const char*>(buffer), static_cast<qint64>(count));
    return written < 0 ? 0 : static_cast<size_t>(written);
}

size_t zipRead(void* opaque, mz_uint64 offset, void* buffer, size_t count) {
    auto* file = static_cast<QFile*>(opaque);
    if (!file->seek(static_cast<qint64>(offset))) {
        return 0;
    }
    const qint64 read = file->read(static_cast<char*>(buffer), static_cast<qint64>(count));
    return read < 0 ? 0 : static_cast<size_t>(read);
}

/// One file of a package, checked: where it goes under the destination.
struct PackageEntry {
    mz_uint  index = 0;
    QString  name; ///< Relative, forward slashes, no step out of the destination.
    quint64  size = 0;
    mz_ulong crc = 0;
};

/// The package's files (directories skipped). False with @p error when the archive cannot be
/// read, holds something this build cannot unpack, or an entry would leave the destination.
bool packageEntries(mz_zip_archive& zip, QList<PackageEntry>& entries, QString& error) {
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            error = QObject::tr("The package cannot be read.");
            return false;
        }
        if (stat.m_is_directory) {
            continue;
        }
        QString name = portable(QString::fromUtf8(stat.m_filename));
        while (name.startsWith(QLatin1String("./"))) {
            name.remove(0, 2);
        }
        const QStringList parts = name.split(QLatin1Char('/'));
        const bool        leaves = name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char(':')) ||
                                   parts.contains(QStringLiteral("..")) || parts.contains(QString());
        if (leaves) {
            error = QObject::tr("The package holds a file with a path that is not allowed (\"%1\"), and was not installed.").arg(name);
            return false;
        }
        if (!stat.m_is_supported || stat.m_is_encrypted) {
            error = QObject::tr("The package holds a file this version cannot unpack (\"%1\").").arg(name);
            return false;
        }
        entries.append({i, name, static_cast<quint64>(stat.m_uncomp_size), static_cast<mz_ulong>(stat.m_crc32)});
    }
    return true;
}

/// 0 = not there, 1 = there with the same content, 2 = there with other content.
int existingState(const QString& target, const PackageEntry& entry) {
    const QFileInfo info(target);
    if (!info.exists()) {
        return 0;
    }
    if (!info.isFile() || static_cast<quint64>(info.size()) != entry.size) {
        return 2;
    }
    bool           ok = false;
    const mz_ulong crc = fileCrc(target, &ok);
    return ok && crc == entry.crc ? 1 : 2;
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
    QDirIterator it(storeRoot(m_folder), QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
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

/// The file named @p name (any case) under @p root that lies nearest @p near, or "".
QString findByName(const QString& root, const QString& name, const QString& near, int& budget) {
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        return {};
    }
    const QStringList nearParts = key(near).split(QLatin1Char('/'));
    QString           best;
    int               bestShared = -1;
    QDirIterator      it(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext() && budget > 0) {
        it.next();
        --budget;
        if (it.fileName().compare(name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString     found = clean(it.filePath());
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

QString resolveTexture(const QString& source, const QString& assetFolder, const QString& libraryRoot, bool* viaName) {
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
    {
        const QDir          folder(assetFolder);
        const QFileInfoList files = folder.entryInfoList(QDir::Files | QDir::NoSymLinks);
        for (const QFileInfo& f : files) {
            if (f.fileName().compare(name, Qt::CaseInsensitive) == 0) {
                found = clean(f.absoluteFilePath());
                break;
            }
        }
    }
    int budget = kNameSearchBudget;
    if (found.isEmpty()) {
        found = findByName(storeRoot(assetFolder), name, assetFolder, budget);
    }
    if (found.isEmpty() && !libraryRoot.isEmpty()) {
        found = findByName(clean(libraryRoot), name, assetFolder, budget);
    }
    if (!found.isEmpty() && viaName != nullptr) {
        *viaName = true;
    }
    return found;
}

// ============================================================================
// WHAT AN ASSET NAMES
// ============================================================================

QStringList materialSources(const QString& materialPath) {
    QStringList sources;
    json        root;
    if (!readJson(materialPath, root)) {
        return sources;
    }
    const json* textures = materialTextures(root);
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

QStringList materialOwnFiles(const QString& materialPath) {
    QStringList   files;
    QSet<QString> seen;
    const QString folder = clean(QFileInfo(materialPath).absolutePath());
    for (const QString& source : materialSources(materialPath)) {
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
    const QFileInfoList files = info.absoluteDir().entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& f : files) {
        if (imageExtensions().contains(f.suffix().toLower()) && f.completeBaseName().compare(name, Qt::CaseInsensitive) == 0) {
            icons.append(clean(f.absoluteFilePath()));
        }
    }
    return icons;
}

// ============================================================================
// THE FILE OPERATIONS
// ============================================================================

bool validAssetName(const QString& name, QString* why) {
    const auto no = [why](const QString& reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };
    if (name.trimmed().isEmpty()) {
        return no(QObject::tr("The name is empty."));
    }
    static const QString kForbidden = QStringLiteral("\\/:*?\"<>|");
    for (const QChar c : name) {
        if (kForbidden.contains(c) || c.unicode() < 32) {
            return no(QObject::tr("A name cannot contain any of these characters: \\ / : * ? \" < > |"));
        }
    }
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        return no(QObject::tr("A name cannot end with a dot or a space."));
    }
    return true;
}

Result renameAsset(const QString& assetPath, const QString& newName) {
    const QFileInfo info(assetPath);
    if (!info.isFile()) {
        return failed(QObject::tr("The file is no longer there:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    const QString name = newName.trimmed();
    QString       why;
    if (!validAssetName(name, &why)) {
        return failed(why);
    }
    const QDir    folder = info.absoluteDir();
    const QString oldName = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    Result        r;
    if (name == oldName) {
        r.ok = true;
        r.path = clean(info.absoluteFilePath());
        return r;
    }
    // (A change of case alone names the same file on Windows and macOS: nothing is in its way.)
    const bool    caseOnly = name.compare(oldName, Qt::CaseInsensitive) == 0;
    const QString target = folder.filePath(name + suffix);
    if (!caseOnly && QFileInfo::exists(target)) {
        return failed(QObject::tr("\"%1\" is already in this folder.").arg(name + suffix));
    }
    const QStringList icons = iconsOf(assetPath);
    QStringList       iconTargets;
    for (const QString& icon : icons) {
        const QString iconTarget = folder.filePath(name + QLatin1Char('.') + QFileInfo(icon).suffix());
        if (!caseOnly && QFileInfo::exists(iconTarget)) {
            return failed(QObject::tr("A picture named \"%1\" is already in this folder.").arg(QFileInfo(iconTarget).fileName()));
        }
        iconTargets.append(iconTarget);
    }

    if (!QFile::rename(info.absoluteFilePath(), target)) {
        return failed(QObject::tr("The file could not be renamed:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    // The name it carries inside (for people; a file this build cannot read keeps its content).
    json root;
    if (readJson(target, root) && isMaterial(root)) {
        root["name"] = name.toStdString();
        writeJson(target, root);
    }
    for (int i = 0; i < icons.size(); ++i) {
        if (!QFile::rename(icons[i], iconTargets[i])) {
            r.error = QObject::tr("The asset was renamed, but its icon could not be:\n%1").arg(QDir::toNativeSeparators(icons[i]));
        }
    }
    r.ok = r.error.isEmpty();
    r.path = clean(target);
    return r;
}

Result duplicateAsset(const QString& assetPath) {
    const QFileInfo info(assetPath);
    if (!info.isFile()) {
        return failed(QObject::tr("The file is no longer there:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    const QDir        folder = info.absoluteDir();
    const QString     oldName = info.completeBaseName();
    const QString     suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    const QStringList icons = iconsOf(assetPath);

    // "<name> copy", "<name> copy 2", …: the first under which neither the file nor a picture is.
    QString name;
    for (int n = 1;; ++n) {
        name = n == 1 ? QObject::tr("%1 copy").arg(oldName) : QObject::tr("%1 copy %2").arg(oldName).arg(n);
        bool taken = QFileInfo::exists(folder.filePath(name + suffix));
        for (const QString& icon : icons) {
            taken = taken || QFileInfo::exists(folder.filePath(name + QLatin1Char('.') + QFileInfo(icon).suffix()));
        }
        if (!taken) {
            break;
        }
    }
    const QString target = folder.filePath(name + suffix);
    if (!QFile::copy(info.absoluteFilePath(), target)) {
        return failed(QObject::tr("The copy could not be written:\n%1").arg(QDir::toNativeSeparators(target)));
    }
    makeWritable(target);
    json root;
    if (readJson(target, root) && isMaterial(root)) {
        root["name"] = name.toStdString();
        writeJson(target, root);
    }
    Result r;
    for (const QString& icon : icons) {
        const QString iconTarget = folder.filePath(name + QLatin1Char('.') + QFileInfo(icon).suffix());
        if (QFile::copy(icon, iconTarget)) {
            makeWritable(iconTarget);
        } else {
            r.error = QObject::tr("The asset was copied, but its icon could not be:\n%1").arg(QDir::toNativeSeparators(icon));
        }
    }
    r.ok = r.error.isEmpty();
    r.path = clean(target);
    return r;
}

Result moveAsset(const QString& assetPath, const QString& destinationFolder) {
    const QFileInfo info(assetPath);
    if (!info.isFile()) {
        return failed(QObject::tr("The file is no longer there:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    if (!QFileInfo(destinationFolder).isDir()) {
        return failed(QObject::tr("The folder is not there:\n%1").arg(QDir::toNativeSeparators(destinationFolder)));
    }
    const QString folder = clean(info.absolutePath());
    const QDir    destination(clean(QDir(destinationFolder).absolutePath()));
    Result        r;
    if (samePath(folder, destination.path())) {
        r.ok = true;
        r.path = clean(info.absoluteFilePath());
        return r;
    }
    if (isTextureStore(destination.dirName()) || within(destination.path(), storeRoot(folder))) {
        return failed(QObject::tr("A \"textures\" folder holds the pictures assets use, not assets. Choose another folder."));
    }
    const QString target = destination.filePath(info.fileName());
    if (QFileInfo::exists(target)) {
        return failed(QObject::tr("\"%1\" is already in that folder.").arg(info.fileName()));
    }
    const QStringList icons = iconsOf(assetPath);
    for (const QString& icon : icons) {
        if (QFileInfo::exists(destination.filePath(QFileInfo(icon).fileName()))) {
            return failed(QObject::tr("A picture named \"%1\" is already in that folder.").arg(QFileInfo(icon).fileName()));
        }
    }

    // The textures first: a move that cannot take them along must not have moved anything.
    json        root;
    const bool  material = readJson(info.absoluteFilePath(), root) && isMaterial(root);
    QStringList fromStore; // what the asset named in the OLD folder's store
    if (material) {
        TextureCollector collector(destination.path(), info.completeBaseName());
        for (json& entry : *materialTextures(root)) {
            const QString written = portable(sourceOf(entry));
            if (written.isEmpty() || !QDir::isRelativePath(written)) {
                continue; // an absolute path is as good from the new folder as from the old
            }
            const QString absolute = clean(QDir(folder).absoluteFilePath(written));
            if (!QFileInfo(absolute).isFile()) {
                continue; // (missing already: the name is kept, for the search by name)
            }
            Placed        how = Placed::Failed;
            const QString placed = collector.addFile(absolute, &how);
            if (how == Placed::Failed) {
                return failed(QObject::tr("A texture could not be copied to the new folder, so nothing was moved:\n%1")
                                  .arg(QDir::toNativeSeparators(absolute)));
            }
            entry["source"] = placed.toStdString();
            ++r.files;
            r.reused += how == Placed::Reused ? 1 : 0;
            if (within(absolute, storeRoot(folder))) {
                fromStore.append(absolute);
            }
        }
    }
    if (material ? !writeJson(target, root) : !QFile::copy(info.absoluteFilePath(), target)) {
        return failed(QObject::tr("The file could not be written:\n%1").arg(QDir::toNativeSeparators(target)));
    }
    for (const QString& icon : icons) {
        if (!QFile::rename(icon, destination.filePath(QFileInfo(icon).fileName()))) {
            r.error = QObject::tr("The asset was moved, but its icon could not be:\n%1").arg(QDir::toNativeSeparators(icon));
        }
    }
    makeWritable(info.absoluteFilePath());
    if (!QFile::remove(info.absoluteFilePath())) {
        r.error = QObject::tr("The asset was copied to the new folder, but the old file could not be removed:\n%1")
                      .arg(QDir::toNativeSeparators(assetPath));
    } else {
        // What the old folder's store now holds for nobody goes (the new folder has it).
        const QSet<QString> named = namedByOthers(folder, QString());
        for (const QString& file : fromStore) {
            if (!named.contains(key(file)) && removeFile(file, /*permanently=*/true)) {
                pruneEmptyFolders(file, storeRoot(folder));
            }
        }
    }
    r.ok = r.error.isEmpty();
    r.path = clean(target);
    return r;
}

Result deleteAsset(const QString& assetPath, bool permanently) {
    const QFileInfo info(assetPath);
    if (!info.isFile()) {
        return failed(QObject::tr("The file is no longer there:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    const QString folder = clean(info.absolutePath());
    // Read before anything goes: what it names in the store, and what the others still name.
    QStringList inStore;
    for (const QString& file : materialOwnFiles(info.absoluteFilePath())) {
        if (within(file, storeRoot(folder))) {
            inStore.append(file);
        }
    }
    const QSet<QString> named = namedByOthers(folder, info.absoluteFilePath());
    const QStringList   icons = iconsOf(assetPath);

    if (!removeFile(info.absoluteFilePath(), permanently)) {
        return failed(permanently ? QObject::tr("The file could not be removed:\n%1").arg(QDir::toNativeSeparators(assetPath))
                                  : QObject::tr("The file could not be moved to the trash:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    Result r;
    for (const QString& icon : icons) {
        removeFile(icon, permanently);
    }
    for (const QString& file : inStore) {
        if (named.contains(key(file))) {
            continue; // another asset of the folder still uses it
        }
        if (removeFile(file, permanently)) {
            ++r.files;
            pruneEmptyFolders(file, storeRoot(folder));
        }
    }
    r.ok = true;
    return r;
}

int removeUnnamed(const QString& folder, const QStringList& files, bool permanently) {
    const QString       store = storeRoot(folder);
    const QSet<QString> named = namedByOthers(folder, QString());
    int                 removed = 0;
    for (const QString& file : files) {
        if (!within(file, store) || named.contains(key(file)) || !QFileInfo(file).isFile()) {
            continue;
        }
        if (removeFile(file, permanently)) {
            ++removed;
            pruneEmptyFolders(file, store);
        }
    }
    return removed;
}

// ============================================================================
// PACKAGES
// ============================================================================

Result exportPackage(const QStringList& items, const QString& zipPath) {
    // What goes in, by its path in the package (one entry per path, in name order).
    QStringList parents;
    for (const QString& item : items) {
        parents.append(QFileInfo(clean(item)).absolutePath());
    }
    const QDir             base(commonFolder(parents));
    QMap<QString, QString> entries; // lower-cased archive name -> (archive name, file) via two maps
    QMap<QString, QString> names;
    const auto             add = [&](const QString& file) {
        const QString name = base.relativeFilePath(file);
        if (name.startsWith(QLatin1String("..")) || isClutter(QFileInfo(file).fileName())) {
            return;
        }
        if (!entries.contains(name.toLower())) {
            entries.insert(name.toLower(), clean(file));
            names.insert(name.toLower(), name);
        }
    };
    for (const QString& item : items) {
        const QFileInfo info(clean(item));
        if (info.isDir()) {
            QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                add(it.filePath());
            }
        } else if (info.isFile()) {
            add(info.absoluteFilePath());
            for (const QString& icon : iconsOf(info.absoluteFilePath())) {
                add(icon);
            }
            for (const QString& texture : materialOwnFiles(info.absoluteFilePath())) {
                add(texture);
            }
        }
    }
    if (entries.isEmpty()) {
        return failed(QObject::tr("There is nothing to put into the package."));
    }

    QSaveFile out(zipPath);
    if (!out.open(QIODevice::WriteOnly)) {
        return failed(QObject::tr("The package could not be written:\n%1").arg(out.errorString()));
    }
    ZipSink        sink{&out};
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    zip.m_pWrite = zipWrite;
    zip.m_pIO_opaque = &sink;
    if (!mz_zip_writer_init_v2(&zip, 0, 0)) {
        return failed(QObject::tr("The package could not be written."));
    }
    Result r;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        QByteArray bytes;
        if (!readAll(it.value(), bytes)) {
            mz_zip_writer_end(&zip);
            return failed(QObject::tr("A file could not be read, so no package was written:\n%1").arg(QDir::toNativeSeparators(it.value())));
        }
        const QByteArray name = names.value(it.key()).toUtf8();
        const mz_uint    level = isCompressedFormat(QFileInfo(it.value()).suffix()) ? static_cast<mz_uint>(MZ_NO_COMPRESSION)
                                                                                     : static_cast<mz_uint>(MZ_DEFAULT_LEVEL);
        if (!mz_zip_writer_add_mem(&zip, name.constData(), bytes.constData(), static_cast<size_t>(bytes.size()), level)) {
            mz_zip_writer_end(&zip);
            return failed(QObject::tr("The package could not be written."));
        }
        ++r.files;
    }
    const bool finished = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!finished || !out.commit()) {
        return failed(QObject::tr("The package could not be written:\n%1").arg(out.errorString()));
    }
    r.ok = true;
    r.path = clean(zipPath);
    return r;
}

int packageConflicts(const QString& zipPath, const QString& destinationFolder) {
    QFile in(zipPath);
    if (!in.open(QIODevice::ReadOnly)) {
        return -1;
    }
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    zip.m_pRead = zipRead;
    zip.m_pIO_opaque = &in;
    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(in.size()), 0)) {
        return -1;
    }
    QList<PackageEntry> entries;
    QString             error;
    int                 conflicts = -1;
    if (packageEntries(zip, entries, error)) {
        conflicts = 0;
        const QDir destination(destinationFolder);
        for (const PackageEntry& entry : entries) {
            conflicts += existingState(destination.filePath(entry.name), entry) == 2 ? 1 : 0;
        }
    }
    mz_zip_reader_end(&zip);
    return conflicts;
}

Result installPackage(const QString& zipPath, const QString& destinationFolder, bool replaceExisting) {
    QFile in(zipPath);
    if (!in.open(QIODevice::ReadOnly)) {
        return failed(QObject::tr("The package could not be read:\n%1").arg(in.errorString()));
    }
    if (!QFileInfo(destinationFolder).isDir()) {
        return failed(QObject::tr("The folder is not there:\n%1").arg(QDir::toNativeSeparators(destinationFolder)));
    }
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    zip.m_pRead = zipRead;
    zip.m_pIO_opaque = &in;
    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(in.size()), 0)) {
        return failed(QObject::tr("This file is not a package PoseStudio can read (a .zip archive is expected)."));
    }
    // Every entry is checked BEFORE the first is written.
    QList<PackageEntry> entries;
    QString             error;
    if (!packageEntries(zip, entries, error)) {
        mz_zip_reader_end(&zip);
        return failed(error);
    }
    Result     r;
    const QDir destination(clean(QDir(destinationFolder).absolutePath()));
    for (const PackageEntry& entry : entries) {
        const QString target = destination.filePath(entry.name);
        const int     state = existingState(target, entry);
        if (state == 1) {
            continue; // there already, the same
        }
        if (state == 2 && !replaceExisting) {
            r.kept.append(entry.name);
            continue;
        }
        size_t size = 0;
        void*  data = mz_zip_reader_extract_to_heap(&zip, entry.index, &size, 0);
        if (data == nullptr && entry.size != 0) {
            mz_zip_reader_end(&zip);
            r.error = QObject::tr("A file of the package is damaged (\"%1\"). What came before it was installed.").arg(entry.name);
            return r;
        }
        const bool written = QDir().mkpath(QFileInfo(target).absolutePath()) &&
                             writeAll(target, QByteArray::fromRawData(static_cast<const char*>(data), static_cast<qsizetype>(size)));
        mz_free(data);
        if (!written) {
            mz_zip_reader_end(&zip);
            r.error = QObject::tr("A file could not be written:\n%1").arg(QDir::toNativeSeparators(target));
            return r;
        }
        ++r.files;
    }
    mz_zip_reader_end(&zip);
    r.ok = true;
    r.path = destination.path();
    return r;
}

} // namespace AssetStore
