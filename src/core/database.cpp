/**
 * @file database.cpp
 * @brief Opens and (when requested) rebuilds PoseStudio's SQLite database.
 *
 * The database lives in the platform's per-user app-data location as posestudio.db (an
 * installed app's own directory is read-only on macOS/Linux, so writable state can't live
 * there). We keep a single named connection ("db_conn") alive for the app's lifetime and hand
 * that same connection back to every caller, rather than opening a new one per call.
 */

#include "database.h"
#include "constants.h"
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QDebug>
#include <QSqlQuery>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>

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

QSqlDatabase appDatabase() {
    return QSqlDatabase::database(QLatin1String(kDatabaseConnectionName));
}

/**
 * @brief The additive schema changes, each a no-op once applied. There is no formal migration
 *        system: a column or table added after the first schema is added HERE as well as in
 *        initialize.sql, or pre-existing installs miss it and fail at the first query touching
 *        it. Schema only — nothing here reads or writes the disk beside the database (the data
 *        layer's checks call it on an in-memory database).
 */
void ensureSchemaCurrent(QSqlDatabase& db) {
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

    // Repair for a 0.3.6 schema-file regression: the header comment in initialize.sql contained a
    // semicolon, which initializeDatabase's naive statement splitter treated as a boundary — corrupting
    // CREATE TABLE Preferences on every fresh install since. Databases created by those builds lack
    // the table entirely; recreate it here (a no-op once present).
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS Preferences("
            "PreferenceID INTEGER PRIMARY KEY AUTOINCREMENT, "
            "PreferenceName TEXT UNIQUE NOT NULL, "
            "PreferenceValue TEXT NOT NULL DEFAULT '', "
            "PreferenceStamp DATETIME DEFAULT CURRENT_TIMESTAMP)"));
        q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_PreferenceName ON Preferences(PreferenceName)"));
    }

    // User-defined manual drag orders (Favorites pane / within a Collection). Seed existing rows
    // with a stable initial order matching their insertion order (the autoincrement ID).
    ensureColumn(db, "Favorites", "FavoriteSortOrder", "INTEGER NOT NULL DEFAULT 0",
                 "UPDATE Favorites SET FavoriteSortOrder = FavoriteID");
    ensureColumn(db, "AssetCollectionItems", "AssetCollectionItemSortOrder", "INTEGER NOT NULL DEFAULT 0",
                 "UPDATE AssetCollectionItems SET AssetCollectionItemSortOrder = AssetCollectionItemID");

    // Flags the single "Maquettes" row (synced by initializeDatabase) as shipped-with-the-app rather than
    // user-added, so the UI knows not to offer removing it.
    ensureColumn(db, "AssetLibraries", "AssetLibraryIsBuiltIn", "INTEGER NOT NULL DEFAULT 0");

    // Every library reader filters on AssetLibraryEnabled = 1 (see core/assetlibraries.h). The
    // column has been in initialize.sql since the first schema, so this guard is belt and braces
    // for a database whose table predates it — without it that first query fails and every
    // library silently vanishes from the UI.
    ensureColumn(db, "AssetLibraries", "AssetLibraryEnabled", "INTEGER NOT NULL DEFAULT 1");

    // Which browser a collection belongs to: '' = the Asset Manager, 'materials' = the Materials
    // Library (see assets/assetmanagerscope.h). Every collection made before the column existed
    // is the Asset Manager's, which the default says.
    ensureColumn(db, "AssetCollections", "AssetCollectionScope", "TEXT NOT NULL DEFAULT ''");
}

/**
 * @brief Opens (or rebuilds) the SQLite database; see database.h for the return contract.
 * @param mode Normal opens the existing file, building the schema only if this is the first
 *             launch (no file yet). FactoryReset deletes the existing file first and always
 *             rebuilds the schema from initialize.sql.
 */
bool initializeDatabase(DbInitMode mode) {
    const QString connectionName = QLatin1String(kDatabaseConnectionName);

    // Writable per-user data belongs in the platform's app-data location, not next to the
    // executable: an installed app's directory is read-only on macOS (.app bundle) and Linux
    // (e.g. /usr/bin), and writing there also breaks code signing. AppDataLocation resolves
    // per-platform (Roaming/<App> on Windows, ~/Library/Application Support/<App> on macOS,
    // ~/.local/share/<App> on Linux) from the APPLICATION name set in main() — and only that:
    // no organization name is ever set, deliberately. Adding setOrganizationName later would
    // insert an <Org>/ segment into this path, move AppDataLocation, and orphan every existing
    // posestudio.db (the legacy-location migration below covers only the next-to-the-exe
    // layout). Create the directory on first use since it may not exist yet.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("posestudio.db"));

    // Older builds stored the database next to the executable. Migrate that file once into the
    // new per-user location so existing libraries/collections/favorites survive the upgrade. We
    // MOVE it (copy, then delete the original) rather than copy: a lingering legacy file would
    // get re-migrated after a Factory Reset wipes the new one, resurrecting deleted data. Only in
    // Normal mode — a reset wants a clean slate, not last session's data migrated back in.
    const QString legacyDbPath = QDir(QCoreApplication::applicationDirPath())
                                     .filePath(QStringLiteral("posestudio.db"));
    if (mode == DbInitMode::Normal && !QFile::exists(dbPath) && QFile::exists(legacyDbPath)) {
        if (QFile::copy(legacyDbPath, dbPath)) {
            QFile::remove(legacyDbPath);
            qDebug() << "Migrated database from legacy location to:" << dbPath;
        } else {
            qWarning() << "[!] Could not migrate legacy database from:" << legacyDbPath;
        }
    }

    // A genuinely fresh install (no file yet) needs the schema built just as much as a
    // FactoryReset does — check this before FactoryReset deletes the file out from under us.
    const bool isFirstLaunch = !QFile::exists(dbPath);

    // Whether the FactoryReset file delete failed (e.g. another process holds the file open on
    // Windows) — if so, the reset falls back to dropping every table in place below.
    bool resetNeedsInPlaceWipe = false;

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
                qWarning() << "[!] Could not delete database file (held open elsewhere?);"
                           << "falling back to an in-place wipe:" << dbPath;
                resetNeedsInPlaceWipe = true;
            }
        }
        // Clear any leftover legacy file too, so a reset is a true clean slate and a later
        // Normal launch can't migrate stale pre-reset data back in.
        QFile::remove(legacyDbPath);
    }

    // Reuse the connection if it's already open; otherwise create it
    QSqlDatabase db = QSqlDatabase::contains(connectionName)
                      ? QSqlDatabase::database(connectionName)
                      : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);

    db.setDatabaseName(dbPath);

    if (!db.open()) {
        qCritical() << "Database Error [initializeDatabase]: Connection failed.";
        qCritical() << "Reason:" << db.lastError().text();
        return false;
    }

    // Fallback wipe for a reset whose file delete failed: drop every user table in place, then
    // let the schema replay below rebuild them. SQLite permits this even while another process
    // holds the file open — which is exactly the case that made the delete fail.
    if (resetNeedsInPlaceWipe) {
        QStringList tableNames;
        QSqlQuery tables(db);
        if (tables.exec(QStringLiteral(
                "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"))) {
            while (tables.next()) tableNames << tables.value(0).toString();
        }
        for (const QString& name : tableNames) {
            QSqlQuery drop(db);
            // Identifier, not user input — table names come from our own schema.
            if (!drop.exec(QStringLiteral("DROP TABLE IF EXISTS \"%1\"").arg(name))) {
                qWarning() << "[!] In-place wipe failed to drop table" << name << ":"
                           << drop.lastError().text();
            }
        }
    }

    // Schema needs (re)building after we just deleted the file above, or on a first-ever launch
    if (mode == DbInitMode::FactoryReset || isFirstLaunch) {
        QFile sqlFile(QStringLiteral(":/resources/database/initialize.sql"));
        if (!sqlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << "Fatal Error: Missing initialize.sql blueprint in resources.";
            return false;
        }

        const QString sqlData = sqlFile.readAll();
        sqlFile.close();

        // Statements are split on ';' — a naive split, not a SQL parser, which is sufficient
        // because the schema file keeps semicolons out of string literals and trigger bodies.
        // Whole-line "--" comments are dropped FIRST: a semicolon in the 0.3.6 header comment
        // once became a statement boundary and corrupted CREATE TABLE Preferences on every fresh
        // install (see the repair migration below). Trailing comments on a statement line are
        // still not supported — keep them on their own lines.
        QStringList sqlLines = sqlData.split(QLatin1Char('\n'));
        sqlLines.erase(std::remove_if(sqlLines.begin(), sqlLines.end(),
                                      [](const QString& line) {
                                          return line.trimmed().startsWith(QLatin1String("--"));
                                      }),
                       sqlLines.end());
        const QStringList sqlStatements =
            sqlLines.join(QLatin1Char('\n')).split(QLatin1Char(';'), Qt::SkipEmptyParts);

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

    // Additive schema changes for databases that predate them (see ensureSchemaCurrent).
    ensureSchemaCurrent(db);

    // One-time default user library: "My PoseStudio Library" in the user's Documents — the
    // writable, user-facing home for personal content, including the hdri/ folder the Environment
    // panel reads and the Materials/ folder the Materials Library tab browses (see
    // src/core/librarypaths.h). Runs only on the very first launch (flag row in
    // Preferences) and only if the user doesn't already have a non-built-in library by that name
    // registered — so a later deliberate rename/removal isn't fought on every start (unlike the
    // built-in Maquettes row below, which IS re-synced every launch by design).
    {
        QSqlQuery flagQuery(db);
        bool alreadyDone = false;
        if (flagQuery.exec(QStringLiteral(
                "SELECT 1 FROM Preferences WHERE PreferenceName = 'UserLibraryCreated'"))) {
            alreadyDone = flagQuery.next();
        }
        if (!alreadyDone) {
            bool haveUserLibrary = false;
            QSqlQuery existing(db);
            if (existing.exec(QStringLiteral(
                    "SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryIsBuiltIn = 0"))) {
                while (existing.next()) {
                    if (QDir(existing.value(0).toString())
                            .dirName()
                            .compare(QLatin1String(Constants::USER_LIBRARY_DIRNAME),
                                     Qt::CaseInsensitive) == 0) {
                        haveUserLibrary = true;
                        break;
                    }
                }
            }
            if (!haveUserLibrary) {
                const QString root =
                    QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                        .filePath(QLatin1String(Constants::USER_LIBRARY_DIRNAME));
                QDir().mkpath(QDir(root).filePath(QStringLiteral("hdri")));
                QDir().mkpath(QDir(root).filePath(QStringLiteral("Materials")));
                QSqlQuery insert(db);
                insert.prepare(QStringLiteral(
                    "INSERT OR IGNORE INTO AssetLibraries (AssetLibraryPath, AssetLibraryIsBuiltIn) "
                    "VALUES (:path, 0)"));
                insert.bindValue(QStringLiteral(":path"), root);
                if (!insert.exec()) {
                    qWarning() << "[!] Failed to create default user library:"
                               << insert.lastError().text();
                }
            }
            QSqlQuery setFlag(db);
            setFlag.exec(QStringLiteral(
                "INSERT OR REPLACE INTO Preferences (PreferenceName, PreferenceValue) "
                "VALUES ('UserLibraryCreated', '1')"));
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

    return true;
}