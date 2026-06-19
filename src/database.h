#ifndef DATABASE_H
#define DATABASE_H

#include <QSqlDatabase>

enum class DbInitMode {
    Normal,       ///< Open the existing database, creating it if it doesn't exist yet.
    FactoryReset  ///< Delete the existing database file and rebuild the schema from scratch.
};

/**
 * @brief Opens (or rebuilds) the application's SQLite database and returns the connection.
 * @param mode Normal to just connect, FactoryReset to wipe and rebuild the schema.
 */
QSqlDatabase initializeDatabase(DbInitMode mode);

#endif // DATABASE_H