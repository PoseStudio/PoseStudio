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

namespace {

/// True if `table` already has a column named `column`.
bool tableHasColumn(QSqlDatabase& db, const QString& table, const QString& column) {
    // Identifiers can't be bound as parameters, but `table` is always a hardcoded literal
    // from our own migration calls below — never user input — so interpolation is safe here.
    QSqlQuery pragma(db);
    if (!pragma.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) return false;
    while (pragma.next()) {
        if (pragma.value(QStringLiteral("name")).toString() == column) return true;
    }
    return false;
}

/// Adds `column` (with SQL `definition`) to `table` if it's missing — the additive-migration
/// workhorse for existing databases. A no-op once the column is present. If `seedSql` is given,
/// it runs once right after the column is created to backfill existing rows.
void ensureColumn(QSqlDatabase& db, const QString& table, const QString& column,
                  const QString& definition, const QString& seedSql = QString()) {
    if (tableHasColumn(db, table, column)) return;

    QSqlQuery alter(db);
    if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, definition))) {
        qWarning() << "[!] Failed to add column" << column << "to" << table << ":" << alter.lastError().text();
        return;
    }
    if (!seedSql.isEmpty()) {
        QSqlQuery seed(db);
        if (!seed.exec(seedSql))
            qWarning() << "[!] Failed to seed column" << column << ":" << seed.lastError().text();
    }
}

} // namespace

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

    // There's no formal migration system yet, so additive schema changes for existing databases
    // are applied here via the ensureColumn helper (each a no-op once already present). Add new
    // ones here, not only in initialize.sql, or pre-existing installs miss them.

    // Nested collections: parent pointer (0 = top-level).
    ensureColumn(db, "AssetCollections", "AssetCollectionParentID", "INTEGER NOT NULL DEFAULT 0");

    // Favorites (a single flat list of favorited asset paths) was added after the initial schema,
    // so older databases lack the table entirely. CREATE TABLE IF NOT EXISTS is a no-op once present.
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS Favorites("
            "FavoriteID INTEGER PRIMARY KEY AUTOINCREMENT, "
            "FavoritePath TEXT NOT NULL UNIQUE, "
            "FavoriteSortOrder INTEGER NOT NULL DEFAULT 0)"));
    }

    // User-defined manual drag orders (Favorites pane / within a Collection). Seed existing rows
    // with a stable initial order matching their insertion order (the autoincrement ID).
    ensureColumn(db, "Favorites", "FavoriteSortOrder", "INTEGER NOT NULL DEFAULT 0",
                 "UPDATE Favorites SET FavoriteSortOrder = FavoriteID");
    ensureColumn(db, "AssetCollectionItems", "AssetCollectionItemSortOrder", "INTEGER NOT NULL DEFAULT 0",
                 "UPDATE AssetCollectionItems SET AssetCollectionItemSortOrder = AssetCollectionItemID");

    // Flags the single "Maquettes" row (synced below) as shipped-with-the-app rather than
    // user-added, so the UI knows not to offer removing it.
    ensureColumn(db, "AssetLibraries", "AssetLibraryIsBuiltIn", "INTEGER NOT NULL DEFAULT 0");

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