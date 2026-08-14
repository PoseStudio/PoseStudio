/**
 * @file librarypaths.cpp
 * @brief Implementation of LibraryPaths. See librarypaths.h.
 */

#include "librarypaths.h"

#include "constants.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>

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
