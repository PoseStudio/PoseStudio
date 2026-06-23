/**
 * @file database.cpp
 * @brief Opens and (when requested) rebuilds PoseStudio's SQLite database.
 *
 * The database lives next to the executable as posestudio.db. We keep a single named
 * connection ("db_conn") alive for the app's lifetime and hand that same connection back
 * to every caller, rather than opening a new one per call.
 */

#include "database.h"
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QDebug>
#include <QSqlQuery>
#include <QDir>
#include <QFile>
#include <QStringList>

/**
 * @brief Opens (or rebuilds) the SQLite database and returns the active connection.
 * @param mode Normal opens the existing file, building the schema only if this is the first
 *             launch (no file yet). FactoryReset deletes the existing file first and always
 *             rebuilds the schema from initialize.sql.
 */
QSqlDatabase initializeDatabase(DbInitMode mode) {
    const QString connectionName = QStringLiteral("db_conn");

    // The database lives next to the .exe, not the current working directory
    const QString dbPath = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("posestudio.db"));

    // A genuinely fresh install (no file yet) needs the schema built just as much as a
    // FactoryReset does — check this before FactoryReset deletes the file out from under us.
    const bool isFirstLaunch = !QFile::exists(dbPath);

    if (mode == DbInitMode::FactoryReset) {
        // Close any open handle first — SQLite can't delete a file Qt still has locked
        if (QSqlDatabase::contains(connectionName)) {
            QSqlDatabase::removeDatabase(connectionName);
        }

        QFile dbFile(dbPath);
        if (dbFile.exists()) {
            if (dbFile.remove()) {
                qDebug() << "Success: Database reset. File removed at:" << dbPath;
            } else {
                qCritical() << "Fatal Error: OS denied permission to delete database file.";
            }
        }
    }

    // Reuse the connection if it's already open; otherwise create it
    QSqlDatabase db = QSqlDatabase::contains(connectionName)
                      ? QSqlDatabase::database(connectionName)
                      : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);

    db.setDatabaseName(dbPath);

    if (!db.open()) {
        qCritical() << "Database Error [initializeDatabase]: Connection failed.";
        qCritical() << "Reason:" << db.lastError().text();
        return db;
    }

    // Schema needs (re)building after we just deleted the file above, or on a first-ever launch
    if (mode == DbInitMode::FactoryReset || isFirstLaunch) {
        QFile sqlFile(QStringLiteral(":/resources/database/initialize.sql"));
        if (!sqlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << "Fatal Error: Missing initialize.sql blueprint in resources.";
            return db;
        }

        const QString sqlData = sqlFile.readAll();
        sqlFile.close();

        // The schema file has no semicolons inside string literals or trigger bodies,
        // so a naive split is sufficient — no need for a real SQL statement parser here.
        const QStringList sqlStatements = sqlData.split(QLatin1Char(';'), Qt::SkipEmptyParts);

        QSqlQuery query(db);
        for (const QString& statement : sqlStatements) {
            const QString trimmedStatement = statement.trimmed();
            if (trimmedStatement.isEmpty()) continue;

            if (!query.exec(trimmedStatement)) {
                qWarning() << "SQL Execution failed for statement:" << trimmedStatement;
                qWarning() << "Error:" << query.lastError().text();
            }
        }
        qDebug() << "Success: Database architecture successfully reconstructed.";
    }

    // There's no formal migration system yet, so additive schema changes for existing
    // databases are applied here, guarded so they're a no-op once already present.
    {
        bool hasParentColumn = false;
        QSqlQuery pragma(db);
        if (pragma.exec(QStringLiteral("PRAGMA table_info(AssetCollections)"))) {
            while (pragma.next()) {
                if (pragma.value(QStringLiteral("name")).toString() == QStringLiteral("AssetCollectionParentID")) {
                    hasParentColumn = true;
                    break;
                }
            }
        }
        if (!hasParentColumn) {
            QSqlQuery alter(db);
            if (!alter.exec(QStringLiteral("ALTER TABLE AssetCollections ADD COLUMN AssetCollectionParentID INTEGER NOT NULL DEFAULT 0"))) {
                qWarning() << "[!] Failed to add AssetCollectionParentID column:" << alter.lastError().text();
            }
        }
    }

    // Favorites (a single flat list of favorited asset paths) was added after the initial
    // schema, so older databases are missing the table. CREATE TABLE IF NOT EXISTS is a no-op
    // once present.
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS Favorites("
            "FavoriteID INTEGER PRIMARY KEY AUTOINCREMENT, "
            "FavoritePath TEXT NOT NULL UNIQUE, "
            "FavoriteSortOrder INTEGER NOT NULL DEFAULT 0)"));
    }

    // FavoriteSortOrder (user-defined drag order in the Favorites pane) was added after the
    // Favorites table itself, so databases that already have Favorites may lack the column.
    {
        bool hasSortColumn = false;
        QSqlQuery pragma(db);
        if (pragma.exec(QStringLiteral("PRAGMA table_info(Favorites)"))) {
            while (pragma.next()) {
                if (pragma.value(QStringLiteral("name")).toString() == QStringLiteral("FavoriteSortOrder")) {
                    hasSortColumn = true;
                    break;
                }
            }
        }
        if (!hasSortColumn) {
            QSqlQuery alter(db);
            if (!alter.exec(QStringLiteral("ALTER TABLE Favorites ADD COLUMN FavoriteSortOrder INTEGER NOT NULL DEFAULT 0"))) {
                qWarning() << "[!] Failed to add FavoriteSortOrder column:" << alter.lastError().text();
            } else {
                // Seed existing rows with a stable initial order (their insertion order).
                QSqlQuery seed(db);
                seed.exec(QStringLiteral("UPDATE Favorites SET FavoriteSortOrder = FavoriteID"));
            }
        }
    }

    // AssetCollectionItemSortOrder (user-defined drag order within a Collection) was added after
    // the AssetCollectionItems table itself, so existing databases may lack the column.
    {
        bool hasSortColumn = false;
        QSqlQuery pragma(db);
        if (pragma.exec(QStringLiteral("PRAGMA table_info(AssetCollectionItems)"))) {
            while (pragma.next()) {
                if (pragma.value(QStringLiteral("name")).toString() == QStringLiteral("AssetCollectionItemSortOrder")) {
                    hasSortColumn = true;
                    break;
                }
            }
        }
        if (!hasSortColumn) {
            QSqlQuery alter(db);
            if (!alter.exec(QStringLiteral("ALTER TABLE AssetCollectionItems ADD COLUMN AssetCollectionItemSortOrder INTEGER NOT NULL DEFAULT 0"))) {
                qWarning() << "[!] Failed to add AssetCollectionItemSortOrder column:" << alter.lastError().text();
            } else {
                // Seed existing rows with a stable initial order (their insertion order).
                QSqlQuery seed(db);
                seed.exec(QStringLiteral("UPDATE AssetCollectionItems SET AssetCollectionItemSortOrder = AssetCollectionItemID"));
            }
        }
    }

    // AssetLibraryIsBuiltIn flags the single "Maquettes" row below as shipped-with-the-app
    // rather than user-added, so the UI knows not to offer removing it.
    {
        bool hasBuiltInColumn = false;
        QSqlQuery pragma(db);
        if (pragma.exec(QStringLiteral("PRAGMA table_info(AssetLibraries)"))) {
            while (pragma.next()) {
                if (pragma.value(QStringLiteral("name")).toString() == QStringLiteral("AssetLibraryIsBuiltIn")) {
                    hasBuiltInColumn = true;
                    break;
                }
            }
        }
        if (!hasBuiltInColumn) {
            QSqlQuery alter(db);
            if (!alter.exec(QStringLiteral("ALTER TABLE AssetLibraries ADD COLUMN AssetLibraryIsBuiltIn INTEGER NOT NULL DEFAULT 0"))) {
                qWarning() << "[!] Failed to add AssetLibraryIsBuiltIn column:" << alter.lastError().text();
            }
        }
    }

    // Ensure the built-in "Maquettes" library exists, on disk and in AssetLibraries, every
    // launch — re-synced to the current install location in case the app was moved since
    // the row was first created. CMake mirrors resources/Maquettes next to the executable
    // at build time; mkpath here is just a safety net if that step hasn't run yet.
    {
        const QString maquettesPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Maquettes"));
        QDir().mkpath(maquettesPath);

        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE AssetLibraries SET AssetLibraryPath = :path WHERE AssetLibraryIsBuiltIn = 1"));
        update.bindValue(":path", maquettesPath);
        if (!update.exec()) {
            qWarning() << "[!] Failed to sync built-in Maquettes library path:" << update.lastError().text();
        } else if (update.numRowsAffected() == 0) {
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral("INSERT OR IGNORE INTO AssetLibraries (AssetLibraryPath, AssetLibraryIsBuiltIn) VALUES (:path, 1)"));
            insert.bindValue(":path", maquettesPath);
            if (!insert.exec()) {
                qWarning() << "[!] Failed to create built-in Maquettes library:" << insert.lastError().text();
            }
        }
    }

    return db;
}