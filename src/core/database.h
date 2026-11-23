// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file database.h
 * @brief The application's SQLite database: bootstrap, migrations, and the shared connection.
 *
 * One named connection is opened once at startup (see initializeDatabase) and handed to every
 * caller through appDatabase() — never open a second connection per call. The schema lives in
 * resources/database/initialize.sql, whose statements only create what is missing and are run on
 * every launch; the columns added to existing tables since are ensureSchemaCurrent(), which
 * initializeDatabase() runs after it.
 */

#ifndef DATABASE_H
#define DATABASE_H

#include <QSqlDatabase>
#include <QString>
#include <QStringList>

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
 *         the next launch runs the schema again (that is what rebuilds the Preferences table
 *         for an install the 0.3.6 schema regression broke), and a degraded launch beats
 *         locking the user out.
 */
bool initializeDatabase(DbInitMode mode);

/// Brings the tables of an open database up to date: every COLUMN added to one since it was
/// first created, each step a no-op once applied. (Whole tables are initialize.sql's, which
/// initializeDatabase() runs first.) initializeDatabase() runs it on the application's
/// database; exposed so a check can run the same steps on a database of its own.
void ensureSchemaCurrent(QSqlDatabase& db);

/// The statements of a schema file's text (resources/database/initialize.sql), in order, each
/// trimmed: whole-line `--` comments are dropped, then the text is split on ';'.
/// initializeDatabase() runs the schema through it; exposed so a check splits the file with the
/// application's own splitter and not a copy of it.
QStringList splitSchemaStatements(const QString& sql);

#endif // DATABASE_H