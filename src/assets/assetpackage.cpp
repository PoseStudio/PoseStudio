/**
 * @file assetpackage.cpp
 * @brief Implementation of packages (assetpackage.h): assets and folders written to a .zip, and
 *        a .zip unpacked into a library folder. The one file of the store that needs miniz.
 */

#include "assetpackage.h"

#include "assetstore_detail.h"

#include <miniz.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSaveFile>
#include <QSet>

using namespace AssetStore::detail;

namespace {

/// Formats whose bytes are compressed already: stored in a package as they are.
bool isCompressedFormat(const QString& suffix) {
    static const QSet<QString> set = {QStringLiteral("png"), QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("webp"),
                                      QStringLiteral("gif"), QStringLiteral("exr"),  QStringLiteral("zip"),  QStringLiteral("duf"),
                                      QStringLiteral("dsf"), QStringLiteral("glb"),  QStringLiteral("usdz"), QStringLiteral("spz")};
    return set.contains(suffix.toLower());
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

/// The deepest folder all of @p paths lie in or under.
QString commonFolder(const QStringList& folders) {
    if (folders.isEmpty()) {
        return {};
    }
    QStringList common = clean(folders.first()).split(QLatin1Char('/'));
    for (const QString& folder : folders) {
        const QStringList parts = clean(folder).split(QLatin1Char('/'));
        int               same = 0;
        while (same < common.size() && same < parts.size() && common[same].compare(parts[same], kFileCase) == 0) {
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

/// What a package of @p items holds, by its path in the package (one entry per path, in name
/// order): @p entries maps the lower-cased archive name to the file, @p names to the archive
/// name as it is written.
void packageContents(const QStringList& items, QMap<QString, QString>& entries, QMap<QString, QString>& names) {
    QStringList parents;
    for (const QString& item : items) {
        parents.append(QFileInfo(clean(item)).absolutePath());
    }
    const QDir base(commonFolder(parents));
    const auto add = [&](const QString& file) {
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
            // (Everything, hidden names too: the folder's texture store is one, and a package
            // is the folder as it lies on disk, not as a browser shows it.)
            QDirIterator it(info.absoluteFilePath(), kEveryFile, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                add(it.filePath());
            }
        } else if (info.isFile()) {
            add(info.absoluteFilePath());
            for (const QString& icon : AssetStore::iconsOf(info.absoluteFilePath())) {
                add(icon);
            }
            for (const QString& texture : AssetStore::materialOwnFiles(info.absoluteFilePath())) {
                add(texture);
            }
        }
    }
}

} // namespace

namespace AssetStore {

PackageSize packageSize(const QStringList& items) {
    QMap<QString, QString> entries;
    QMap<QString, QString> names;
    packageContents(items, entries, names);
    PackageSize size;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        ++size.files;
        size.bytes += QFileInfo(it.value()).size();
    }
    return size;
}

Result exportPackage(const QStringList& items, const QString& zipPath) {
    QMap<QString, QString> entries;
    QMap<QString, QString> names;
    packageContents(items, entries, names);
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
