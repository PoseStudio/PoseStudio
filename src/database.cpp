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
 * @param mode Normal just opens the existing file (creating it if missing). FactoryReset
 *             deletes the existing file first and rebuilds the schema from initialize.sql.
 */
QSqlDatabase initializeDatabase(DbInitMode mode) {
    const QString connectionName = QStringLiteral("db_conn");

    // The database lives next to the .exe, not the current working directory
    const QString dbPath = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("posestudio.db"));

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

    // Schema only needs rebuilding after we just deleted the file above
    if (mode == DbInitMode::FactoryReset) {
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

    return db;
}