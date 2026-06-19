/**
 * @file preferencesmanager.h
 * @brief Declares PreferencesManager, a singleton in-memory cache of user settings.
 *
 * Settings are loaded from SQLite once at startup and kept in a QHash so reads never
 * touch the database. Writes update the cache immediately and persist to disk.
 */

#ifndef PREFERENCESMANAGER_H
#define PREFERENCESMANAGER_H

#include <QHash>
#include <QString>
#include <QVariant>

/**
 * @class PreferencesManager
 * @brief Process-wide cache of user settings, backed by the Preferences SQLite table.
 *
 * Not thread-safe — like the rest of PoseStudio's UI code, this is meant to be used
 * only from the Qt main/GUI thread.
 */
class PreferencesManager {
public:
    /// Returns the single shared instance (lazily constructed on first call).
    static PreferencesManager& instance() {
        static PreferencesManager instance;
        return instance;
    }

    // Non-copyable: a second cache could fall out of sync with the database.
    PreferencesManager(const PreferencesManager&) = delete;
    PreferencesManager& operator=(const PreferencesManager&) = delete;

    /// Loads every row of the Preferences table into the in-memory cache. Call once at startup.
    void loadFromDatabase();

    /// Returns the cached value for `key`, or `defaultValue` if it isn't set.
    QVariant getValue(const QString& key, const QVariant& defaultValue = QVariant()) const;

    /// Updates the cache and immediately persists the new value to the database.
    void setValue(const QString& key, const QVariant& value);

private:
    PreferencesManager() = default;
    ~PreferencesManager() = default;

    QHash<QString, QVariant> m_preferences;
};

#endif // PREFERENCESMANAGER_H