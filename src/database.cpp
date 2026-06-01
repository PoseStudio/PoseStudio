#include "database.h" // Links this source file to header
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QDebug>
#include <QSqlQuery>
#include <QDir>
#include <QFile>
#include <QStringList>


QSqlDatabase connectDatabase() {
    // Define a unique connection name
    QString connectionName = "db_conn";

    // Check if the connection already exists to prevent duplicates
    if (QSqlDatabase::contains(connectionName)) {
        return QSqlDatabase::database(connectionName);
    }

    // Create a new database connection
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    db.setDatabaseName("posestudio.db");
    
    // Open the connection and check for errors
    if (!db.open()) {
        qCritical() << "Database open error:" << db.lastError().text();
    }

    // Return the database object by value
    return db;
}

QSqlDatabase initializeDatabase(int mode) {
    // Dynamically locate the database file
    QString exeFolder = QCoreApplication::applicationDirPath();
    QString dbPath = QDir(exeFolder).filePath("posestudio.db");

    // If mode is 1, we NUKE the database
    if (mode == 1) {
        // Ensure no active connections are holding the file hostage
        if (QSqlDatabase::contains("db_conn")) {
            QSqlDatabase::removeDatabase("db_conn");
        }

        QFile dbFile(dbPath);
        if (dbFile.exists()) {
            if (dbFile.remove()) {
                qDebug() << "Success: Database nuked from orbit at:" << dbPath;
            } else {
                qCritical() << "Error: Could not delete database. It might be locked by the OS.";
            }
        }
    }

    // Create the connection
    QSqlDatabase db;
    if (QSqlDatabase::contains("db_conn")) {
        db = QSqlDatabase::database("db_conn");
    } else {
        db = QSqlDatabase::addDatabase("QSQLITE", "db_conn");
    }
    
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        qCritical() << "Database Error: Could not connect to SQLite database.";
        qCritical() << "Reason:" << db.lastError().text();
        return db; // Return the broken connection so the app knows it failed
    }

    // If mode is 1, rebuild the tables from the embedded SQL script
    if (mode == 1) {
        QFile sqlFile(":/resources/database/initialize.sql");
        if (!sqlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << "Failed to open SQL initialization script from resources!";
            return db; 
        }

        // Read the entire file and split it into individual SQL commands by semicolon
        QString sqlData = sqlFile.readAll();
        sqlFile.close();
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
        qDebug() << "Success: Database successfully rebuilt from initialize.sql";
    }

    // Hand the open, ready-to-use connection back to the application
    return db;
}