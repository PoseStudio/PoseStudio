/**
 * @file preferencesmanager.cpp
 * @brief Implementation of the PreferencesManager singleton.
 */

#include "preferencesmanager.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>

/**
 * @brief Replaces the in-memory cache with the full contents of the Preferences table.
 *        Called once during startup, after the database connection is established.
 */
void PreferencesManager::loadFromDatabase() {
    m_preferences.clear();

    QSqlDatabase db = QSqlDatabase::database("db_conn");
    QSqlQuery query(db);

    if (!query.exec("SELECT PreferenceName, PreferenceValue FROM Preferences")) {
        qWarning() << "Preferences Error: Could not query the database during bootstrap."
                   << query.lastError().text();
        return;
    }

    while (query.next()) {
        m_preferences.insert(query.value("PreferenceName").toString(), query.value("PreferenceValue"));
    }

    qDebug() << "Success: Loaded" << m_preferences.size() << "preferences into memory cache.";
}

QVariant PreferencesManager::getValue(const QString& key, const QVariant& defaultValue) const {
    return m_preferences.value(key, defaultValue);
}

void PreferencesManager::setValue(const QString& key, const QVariant& value) {
    // Update the cache first so callers see the new value immediately, even if the write below fails
    m_preferences.insert(key, value);

    QSqlDatabase db = QSqlDatabase::database("db_conn");
    QSqlQuery query(db);
    query.prepare(
        "INSERT INTO Preferences (PreferenceName, PreferenceValue) "
        "VALUES (:key, :val) "
        "ON CONFLICT(PreferenceName) DO UPDATE SET PreferenceValue = excluded.PreferenceValue"
    );
    query.bindValue(":key", key);
    query.bindValue(":val", value);

    if (!query.exec()) {
        qWarning() << "Preferences Error: Failed to commit" << key << "to the database.";
        qWarning() << "Reason:" << query.lastError().text();
    }
}