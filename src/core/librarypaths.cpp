/**
 * @file librarypaths.cpp
 * @brief Implementation of LibraryPaths. See librarypaths.h.
 */

#include "librarypaths.h"

#include "constants.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>

#include <algorithm>

namespace {

// The stock panorama preferred as the startup default whenever it exists (wherever the user
// filed it — root or any category subfolder). Matched by basename, extension-agnostic, so either
// a .hdr or an .exr of the stock set satisfies it.
constexpr const char* kStockHdri = "Studio Small 09";

// A file's category: its folder path relative to the hdri root — "" for uncategorized root files,
// which therefore sort ahead of every named category.
QString hdriCategory(const QString& relativePath) {
    return relativePath.section(QLatin1Char('/'), 0, -2);
}

} // namespace

QString LibraryPaths::userLibraryRoot() {
    // Prefer a library the user has registered (wherever on disk they keep it) so their existing
    // "My PoseStudio Library" is honoured; the Documents default is the fresh-install fallback and
    // matches the folder initializeDatabase() creates on first launch.
    QSqlQuery query(QSqlDatabase::database(QStringLiteral("db_conn")));
    if (query.exec(QStringLiteral("SELECT AssetLibraryPath FROM AssetLibraries "
                                  "WHERE AssetLibraryEnabled = 1 AND AssetLibraryIsBuiltIn = 0"))) {
        while (query.next()) {
            const QString path = query.value(0).toString();
            if (QDir(path).dirName().compare(QLatin1String(Constants::USER_LIBRARY_DIRNAME),
                                             Qt::CaseInsensitive) == 0) {
                return path;
            }
        }
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QLatin1String(Constants::USER_LIBRARY_DIRNAME));
}

QString LibraryPaths::hdriDirectory() {
    const QString dir = QDir(userLibraryRoot()).filePath(QStringLiteral("hdri"));
    QDir().mkpath(dir);
    return dir;
}

QStringList LibraryPaths::hdriFiles() {
    const QDir root(hdriDirectory());
    QStringList files;
    QDirIterator it(root.path(), QStringList{QStringLiteral("*.hdr"), QStringLiteral("*.exr")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        files.append(it.next());
    }
    // Display order: category (subfolder) first, name within — case-insensitive, matching how the
    // flat folder listed before categorization existed.
    std::sort(files.begin(), files.end(), [&root](const QString& a, const QString& b) {
        const QString relA = root.relativeFilePath(a);
        const QString relB = root.relativeFilePath(b);
        const int byCategory = hdriCategory(relA).compare(hdriCategory(relB), Qt::CaseInsensitive);
        if (byCategory != 0) {
            return byCategory < 0;
        }
        return relA.section(QLatin1Char('/'), -1).compare(relB.section(QLatin1Char('/'), -1),
                                                          Qt::CaseInsensitive) < 0;
    });
    return files;
}

QString LibraryPaths::defaultHdri() {
    const QStringList files = hdriFiles();
    for (const QString& path : files) {
        if (QFileInfo(path).completeBaseName().compare(QLatin1String(kStockHdri),
                                                       Qt::CaseInsensitive) == 0) {
            return path;
        }
    }
    return files.isEmpty() ? QString() : files.first();
}
