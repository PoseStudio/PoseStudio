#ifndef DATABASE_H
#define DATABASE_H

// includes
#include <QSqlDatabase>

// Function declarations
bool initializeDatabase();
QSqlDatabase connectDatabase();

#endif // DATABASE_H