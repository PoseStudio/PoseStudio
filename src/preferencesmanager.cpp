/**
 * @file preferencesmanager.cpp
 * @brief Implementation of the global PreferencesManager singleton.
 *
 * This file acts as the bridge between the persistent SQLite database and the 
 * application's high-speed memory. It caches user settings in RAM (O(1) lookup time) 
 * so the UI can instantly read preferences without bottlenecking the hard drive, 
 * while ensuring any changes are safely written back to the disk.
 */

#include "preferencesmanager.h"
#include <QSqlDatabase> 
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>

/**
 * @brief Populates the in-memory cache from the local SQLite database.
 * * Typically called once during application bootstrap (in main.cpp). It locks onto
 * the global "db_conn" connection pool and caches the entire Preferences table.
 */
void PreferencesManager::loadFromDatabase() {
    m_preferences.clear();
    
    // Tap into the globally established connection pool
    QSqlDatabase db = QSqlDatabase::database("db_conn");
    QSqlQuery query(db);
    
    // Fetch all key-value pairs
    if (!query.exec("SELECT PreferenceName, PreferenceValue FROM Preferences")) {
        qWarning() << "Preferences Error: Could not query the database during bootstrap." 
                   << query.lastError().text();
        return;
    }

    // Populate the QHash cache
    while (query.next()) {
        QString key = query.value("PreferenceName").toString();
        QVariant value = query.value("PreferenceValue");
        m_preferences.insert(key, value);
    }
    
    qDebug() << "Success: Loaded" << m_preferences.size() << "preferences into memory cache.";
}

/**
 * @brief Retrieves a preference value.
 * * Reads directly from the RAM cache rather than querying the database,
 * making this safe to call inside heavy rendering loops or UI updates.
 * * @param key The unique string identifier for the preference.
 * @param defaultValue Returned if the key does not exist in the database.
 * @return QVariant The stored value, type-agnostic.
 */
QVariant PreferencesManager::getValue(const QString& key, const QVariant& defaultValue) const {
    return m_preferences.value(key, defaultValue);
}

/**
 * @brief Saves a preference to both the memory cache and the physical database.
 * * Utilizes a SQLite UPSERT constraint. If the key doesn't exist, it is inserted. 
 * If the key already exists (violating the UNIQUE constraint), it cleanly updates 
 * the existing record instead of throwing an error.
 * * @param key The unique string identifier for the preference.
 * @param value The value to be saved.
 */
void PreferencesManager::setValue(const QString& key, const QVariant& value) {
    // 1. Instantly update the RAM cache so the UI stays responsive
    m_preferences.insert(key, value);
    
    // 2. Persist the change to the disk
    QSqlDatabase db = QSqlDatabase::database("db_conn");
    QSqlQuery query(db);
    
    // Standard SQLite UPSERT pattern
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