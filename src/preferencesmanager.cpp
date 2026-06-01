#include "preferencesmanager.h"
#include <QSqlDatabase> // <-- ADD THIS
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>

void PreferencesManager::loadFromDatabase() {
    m_preferences.clear();
    
    // Explicitly grab your named connection!
    QSqlDatabase db = QSqlDatabase::database("db_conn");
    QSqlQuery query(db);
    
    if (!query.exec("SELECT PreferenceName, PreferenceValue FROM Preferences")) {
        qWarning() << "Preferences Warning: Could not query Preferences table." 
                 << query.lastError().text();
        return;
    }

    while (query.next()) {
        QString key = query.value("PreferenceName").toString();
        QVariant value = query.value("PreferenceValue");
        m_preferences.insert(key, value);
    }
    
    qDebug() << "Successfully loaded" << m_preferences.size() << "preferences into memory.";
}

QVariant PreferencesManager::getValue(const QString& key, const QVariant& defaultValue) const {
    return m_preferences.value(key, defaultValue);
}

void PreferencesManager::setValue(const QString& key, const QVariant& value) {
    m_preferences.insert(key, value);
    
    // Explicitly grab your named connection!
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
        qWarning() << "Failed to save preference:" << query.lastError().text();
    }
}