/**
 * @file database.cpp
 * @brief Implementation of global SQLite database connection and initialization routines.
 * * This file manages the lifecycle of the SQLite database for PoseStudio.
 * It provides connection pooling to prevent memory leaks and file locks, 
 * and handles the raw execution of SQL schemas during application bootstrap or factory resets.
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
 * @brief Retrieves the active connection to the SQLite database.
 * * Implements a check to verify if the "db_conn" connection pool already exists. 
 * If it does, the existing connection is reused to prevent duplicate OS-level locks 
 * on the physical SQLite file.
 * * @return QSqlDatabase The active database connection object.
 */
QSqlDatabase connectDatabase() {
    // QStringLiteral resolves the string at compile-time, saving runtime memory allocation
    const QString connectionName = QStringLiteral("db_conn");

    // Return the existing connection if it is already established
    if (QSqlDatabase::contains(connectionName)) {
        return QSqlDatabase::database(connectionName);
    }

    // Otherwise, instantiate a new SQLite connection pool
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(QStringLiteral("posestudio.db"));
    
    if (!db.open()) {
        qCritical() << "Database Error [connectDatabase]: Failed to open connection." 
                    << db.lastError().text();
    }

    return db;
}

/**
 * @brief Bootstraps the application database, optionally forcing a complete factory reset.
 * * This function resolves the database path relative to the application executable.
 * If mode 1 is passed, it executes a destructive reset: severing connections, deleting 
 * the physical SQLite file, and rebuilding the schema using the embedded initialize.sql blueprint.
 * * @param mode Operation mode flag (0 = Normal Connection, 1 = Destructive Factory Reset).
 * @return QSqlDatabase The active, initialized database connection.
 */
QSqlDatabase initializeDatabase(int mode) {
    const QString connectionName = QStringLiteral("db_conn");
    
    // Resolve absolute path dynamically relative to the application binary
    const QString exeFolder = QCoreApplication::applicationDirPath();
    const QString dbPath = QDir(exeFolder).filePath(QStringLiteral("posestudio.db"));

    // =========================================================================
    // [ DESTRUCTIVE RESET ]
    // =========================================================================
    if (mode == 1) {
        // Sever any active handles to prevent OS-level file locking
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

    // =========================================================================
    // [ ESTABLISH CONNECTION ]
    // =========================================================================
    
    // Fetch the existing connection, or create a new one if it was purged
    QSqlDatabase db = QSqlDatabase::contains(connectionName) 
                      ? QSqlDatabase::database(connectionName) 
                      : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        qCritical() << "Database Error [initializeDatabase]: Connection failed.";
        qCritical() << "Reason:" << db.lastError().text();
        return db; 
    }

    // =========================================================================
    // [ SCHEMA RECONSTRUCTION ]
    // =========================================================================
    
    // If a destructive reset was requested, parse and execute the SQL blueprint
    if (mode == 1) {
        QFile sqlFile(QStringLiteral(":/resources/database/initialize.sql"));
        if (!sqlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << "Fatal Error: Missing initialize.sql blueprint in resources.";
            return db; 
        }

        const QString sqlData = sqlFile.readAll();
        sqlFile.close();
        
        // Isolate individual SQL commands using a fast Latin1 char split
        const QStringList sqlStatements = sqlData.split(QLatin1Char(';'), Qt::SkipEmptyParts);

        QSqlQuery query(db);
        
        // Pass by const reference to avoid deep copying strings during iteration
        for (const QString& statement : sqlStatements) {
            const QString trimmedStatement = statement.trimmed();
            
            if (!trimmedStatement.isEmpty()) {
                if (!query.exec(trimmedStatement)) {
                    qWarning() << "SQL Execution failed for statement:" << trimmedStatement;
                    qWarning() << "Error:" << query.lastError().text();
                }
            }
        }
        qDebug() << "Success: Database architecture successfully reconstructed.";
    }

    return db;
}