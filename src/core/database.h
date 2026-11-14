/**
 * @file database.h
 * @brief The application's SQLite database: bootstrap, migrations, and the shared connection.
 *
 * One named connection is opened once at startup (see initializeDatabase) and handed to every
 * caller through appDatabase() — never open a second connection per call. The schema lives in
 * resources/database/initialize.sql; additive migrations for existing databases live in
 * initializeDatabase() itself (see database.cpp).
 */

#ifndef DATABASE_H
#define DATABASE_H

#include <QSqlDatabase>

/// The name of the single process-wide QSqlDatabase connection. Only database.cpp should ever
/// need it directly; everything else goes through appDatabase().
inline constexpr const char* kDatabaseConnectionName = "db_conn";

/// The shared, already-open application database connection. Valid after initializeDatabase()
/// has run (main.cpp calls it before any widget is constructed). GUI thread only.
QSqlDatabase appDatabase();

enum class DbInitMode {
    Normal,       ///< Open the existing database, creating it if it doesn't exist yet.
    FactoryReset  ///< Delete the existing database file and rebuild the schema from scratch.
};

/**
 * @brief Opens (or rebuilds) the application's SQLite database; the connection is then
 *        available through appDatabase().
 * @param mode Normal to just connect, FactoryReset to wipe and rebuild the schema.
 * @return false when nothing usable came up: the connection could not be opened, or the schema
 *         had to be built and the embedded initialize.sql is missing. main() treats that as
 *         fatal. Individual schema statements that fail are logged but do NOT fail the launch —
 *         the additive migrations that follow them are the designed repair path (they rebuilt
 *         the Preferences table for every install the 0.3.6 schema regression broke), and a
 *         degraded launch beats locking the user out.
 */
bool initializeDatabase(DbInitMode mode);

/// Brings an open database's schema up to date: every column and table added since the first
/// schema, each step a no-op once applied. initializeDatabase() runs it on the application's
/// database; exposed so a check can run the same steps on a database of its own.
void ensureSchemaCurrent(QSqlDatabase& db);

#endif // DATABASE_H