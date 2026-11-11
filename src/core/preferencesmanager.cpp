/**
 * @file preferencesmanager.cpp
 * @brief Implementation of the PreferencesManager singleton.
 */

#include "preferencesmanager.h"
#include "database.h"
#include <QDir>
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

    QSqlDatabase db = appDatabase();
    QSqlQuery query(db);

    if (!query.exec("SELECT PreferenceName, PreferenceValue FROM Preferences")) {
        qWarning() << "Preferences Error: Could not query the database during bootstrap."
                   << query.lastError().text();
        return;
    }

    while (query.next()) {
        m_preferences.insert(query.value("PreferenceName").toString(), query.value("PreferenceValue"));
    }
}

QVariant PreferencesManager::getValue(const QString& key, const QVariant& defaultValue) const {
    return m_preferences.value(key, defaultValue);
}

QString PreferencesManager::rememberedDirectory(const QString& key, const QString& fallback) const {
    const QString stored = getValue(key, fallback).toString();
    if (stored.isEmpty() || !QDir(stored).exists()) return fallback;
    return stored;
}

bool PreferencesManager::isolatedRun() {
    static const bool isolated = qEnvironmentVariableIsSet("POSESTUDIO_IK_SCRIPT") &&
                                 qEnvironmentVariableIntValue("POSESTUDIO_IK_SCRIPT_PREFS") == 0;
    return isolated;
}

void PreferencesManager::setValue(const QString& key, const QVariant& value) {
    // Update the cache first so callers see the new value immediately, even if the write below fails
    m_preferences.insert(key, value);
    if (isolatedRun()) {
        return; // a scripted test's settings are its own (see isolatedRun)
    }

    QSqlDatabase db = appDatabase();
    QSqlQuery query(db);
    // The upsert also refreshes PreferenceStamp: the column's DEFAULT only fires on the INSERT
    // path, so without this a row's stamp froze at its first write.
    query.prepare(
        "INSERT INTO Preferences (PreferenceName, PreferenceValue) "
        "VALUES (:key, :val) "
        "ON CONFLICT(PreferenceName) DO UPDATE SET PreferenceValue = excluded.PreferenceValue, "
        "PreferenceStamp = CURRENT_TIMESTAMP"
    );
    query.bindValue(":key", key);
    query.bindValue(":val", value);

    if (!query.exec()) {
        qWarning() << "Preferences Error: Failed to commit" << key << "to the database.";
        qWarning() << "Reason:" << query.lastError().text();
    }
}