/**
 * @file database.cpp
 * @brief Implementation of global SQLite database connection and initialization routines.
 * * This file handles the establishment of secure SQLite database connections. 
 * It manages connection pooling via named connections to prevent memory leaks,
 * and handles raw schema execution during factory resets or initial launches.
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
 * @brief Retrieves an active connection to the SQLite database.
 * * Implements a Singleton-style check to verify if a connection pool named 
 * "db_conn" already exists. If it does, the existing connection is returned 
 * to prevent duplicate locks on the physical database file.
 * * @return QSqlDatabase The active database connection object.
 */
QSqlDatabase connectDatabase() {
    QString connectionName = "db_conn";

    if (QSqlDatabase::contains(connectionName)) {
        return QSqlDatabase::database(connectionName);
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    db.setDatabaseName("posestudio.db");
    
    if (!db.open()) {
        qCritical() << "Database Error [connectDatabase]: Failed to open connection." 
                    << db.lastError().text();
    }

    return db;
}

/**
 * @brief Bootstraps the application database, optionally forcing a factory reset.
 * * This function locates the `.db` file relative to the application executable.
 * If mode 1 is passed, it executes a destructive reset: it forces connection 
 * closures, deletes the physical SQLite file, creates a new one, and runs the 
 * embedded `initialize.sql` script from the Qt Resource System to reconstruct 
 * the schema.
 * * @param mode Operation mode flag. 0 = Normal Connection, 1 = Destructive Factory Reset.
 * @return QSqlDatabase The active, initialized database connection.
 */
QSqlDatabase initializeDatabase(int mode) {
    // Resolve absolute path dynamically relative to the application binary
    QString exeFolder = QCoreApplication::applicationDirPath();
    QString dbPath = QDir(exeFolder).filePath("posestudio.db");

    // --- DESTRUCTIVE RESET ---
    if (mode == 1) {
        // Sever any active handles to prevent OS-level file locking
        if (QSqlDatabase::contains("db_conn")) {
            QSqlDatabase::removeDatabase("db_conn");
        }

        QFile dbFile(dbPath);
        if (dbFile.exists()) {
            if (dbFile.remove()) {
                qDebug() << "Success: Database nuked from orbit at:" << dbPath;
            } else {
                qCritical() << "Fatal Error: OS denied permission to delete database file.";
            }
        }
    }

    // --- ESTABLISH CONNECTION ---
    QSqlDatabase db;
    if (QSqlDatabase::contains("db_conn")) {
        db = QSqlDatabase::database("db_conn");
    } else {
        db = QSqlDatabase::addDatabase("QSQLITE", "db_conn");
    }
    
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        qCritical() << "Database Error [initializeDatabase]: Connection failed.";
        qCritical() << "Reason:" << db.lastError().text();
        return db; 
    }

    // --- SCHEMA RECONSTRUCTION ---
    // If a destructive reset was requested, parse and execute the SQL blueprint
    if (mode == 1) {
        QFile sqlFile(":/resources/database/initialize.sql");
        if (!sqlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << "Fatal Error: Missing initialize.sql blueprint in resources.";
            return db; 
        }

        QString sqlData = sqlFile.readAll();
        sqlFile.close();
        
        // Isolate individual SQL commands
        QStringList sqlStatements = sqlData.split(';', Qt::SkipEmptyParts);

        QSqlQuery query(db);
        for (QString statement : sqlStatements) {
            statement = statement.trimmed();
            if (!statement.isEmpty()) {
                if (!query.exec(statement)) {
                    qWarning() << "SQL Execution failed for statement:" << statement;
                    qWarning() << "Error:" << query.lastError().text();
                }
            }
        }
        qDebug() << "Success: Database architecture successfully reconstructed.";
    }

    return db;
}