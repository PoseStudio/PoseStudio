#include "database.h" // Links this source file to header
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QFile>

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

bool initializeDatabase() {

    // Define a unique connection name
    QString connectionName = "db_conn";

    // Close the db connection
    {
        if (QSqlDatabase::contains(connectionName)) {
            QSqlDatabase db = QSqlDatabase::database(connectionName);
            if (db.isOpen()) {
                db.close();
                qDebug() << "Database connection closed.";
            }
        }
    }

    // Remove the connection from Qt's manager
    QSqlDatabase::removeDatabase(connectionName);

    QString databaseSql = "D:/PoseStudio/initialize.sql";
    QString databaseFile = "D:/PoseStudio/build/Debug/posestudio.db";

    // Delete the physical db file if it exists
    QFile dbFile(databaseFile);
    if (dbFile.exists()) {
        if (dbFile.remove()) {
            qDebug() << "Success: Database file deleted completely.";
        } else {
            qCritical() << "Error: Could not delete the database file. It may be locked by another program.";
            return false;
        }
    }

    QSqlDatabase db = connectDatabase();

    if (!db.open()) {
        qCritical() << "Database Error: Could not connect to SQLite database.";
        qCritical() << "Reason:" << db.lastError().text();
        return false;
    }

    // Open .sql db initialization file
    QFile file(databaseSql);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCritical() << "Database Error: Could not initialize SQLite database.";
        qCritical() << "Failed to open SQL file:" << file.errorString();
        return false;
    }

    // Read the initialization file
    QTextStream in(&file);
    QString sqlContent = in.readAll();
    file.close();

    // Split initialization string into individual queries by semicolon
    QStringList queries = sqlContent.split(';', Qt::SkipEmptyParts);

    // Start a transaction
    db.transaction();

    // Pass the active connection into the query
    QSqlQuery query(db);

    // Loop through and execute each command
    for (QString singleQuery : queries) {
        singleQuery = singleQuery.trimmed(); // Remove leading/trailing whitespace
        
        // Skip entirely empty lines
        if (singleQuery.isEmpty()) {
            continue; 
        }

        if (!query.exec(singleQuery)) {
            qCritical() << "Database Error: Failed to execute statement in script.";
            qCritical() << "Reason:" << query.lastError().text();
            qCritical() << "Failing Query:" << singleQuery;
            
            // If one fails, cancel the whole batch so you don't get corrupt data
            db.rollback(); 
            return false;
        }
    }

    // If everything succeeded, commit the changes to the database!
    db.commit();
    return true;

}