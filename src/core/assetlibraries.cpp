// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetlibraries.cpp
 * @brief Implementation of the AssetLibraries repository. See assetlibraries.h.
 */

#include "assetlibraries.h"

#include "database.h"
#include "librarypaths.h"

#include <QDebug>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

#include <algorithm>

namespace AssetLibraries {

QList<Library> enabled() {
    QList<Library> builtIn;
    QList<Library> user;

    QSqlQuery query(appDatabase());
    if (!query.exec(QStringLiteral(
            "SELECT AssetLibraryID, AssetLibraryPath, AssetLibraryIsBuiltIn FROM AssetLibraries "
            "WHERE AssetLibraryEnabled = 1"))) {
        qWarning() << "[!] Failed to load asset libraries:" << query.lastError().text();
        return {};
    }
    while (query.next()) {
        Library lib;
        lib.id = query.value(0).toInt();
        lib.path = query.value(1).toString();
        lib.builtIn = query.value(2).toBool();
        (lib.builtIn ? builtIn : user).append(lib);
    }

    // User libraries are alphabetized by folder NAME (see the header for why not full path).
    std::sort(user.begin(), user.end(), [](const Library& a, const Library& b) {
        return QDir(a.path).dirName().compare(QDir(b.path).dirName(), Qt::CaseInsensitive) < 0;
    });
    return builtIn + user;
}

QList<Library> userManaged() {
    QList<Library> libraries;
    QSqlQuery query(appDatabase());
    if (!query.exec(QStringLiteral(
            "SELECT AssetLibraryID, AssetLibraryPath FROM AssetLibraries "
            "WHERE AssetLibraryIsBuiltIn = 0 ORDER BY AssetLibraryPath COLLATE NOCASE"))) {
        // Surface the failure — an empty list from a silent error reads as "my libraries are gone".
        qWarning() << "[!] Failed to load asset libraries:" << query.lastError().text();
        return {};
    }
    while (query.next()) {
        Library lib;
        lib.id = query.value(0).toInt();
        lib.path = query.value(1).toString();
        lib.builtIn = false;
        libraries.append(lib);
    }
    return libraries;
}

QStringList enabledPaths() {
    QStringList paths;
    const QList<Library> libraries = enabled();
    paths.reserve(libraries.size());
    for (const Library& lib : libraries) paths.append(lib.path);
    return paths;
}

AddResult add(const QString& path) {
    QSqlQuery query(appDatabase());
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO AssetLibraries (AssetLibraryPath) VALUES (:path)"));
    query.bindValue(QStringLiteral(":path"), path);
    if (!query.exec()) {
        qWarning() << "[!] Failed to add asset library:" << query.lastError().text();
        return AddResult::Failed;
    }
    // INSERT OR IGNORE "succeeds" with zero rows on the UNIQUE(AssetLibraryPath) conflict.
    if (query.numRowsAffected() == 0) return AddResult::AlreadyRegistered;
    LibraryPaths::invalidateCache(); // the user-library root may now resolve differently
    return AddResult::Added;
}

bool remove(int id) {
    QSqlQuery query(appDatabase());
    query.prepare(QStringLiteral("DELETE FROM AssetLibraries WHERE AssetLibraryID = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        qWarning() << "[!] Failed to remove asset library:" << query.lastError().text();
        return false;
    }
    LibraryPaths::invalidateCache(); // the removed row may have been the user-library root
    return true;
}

} // namespace AssetLibraries
