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

    /// Updates the cache and immediately persists the new value to the database — except in an
    /// isolated run (below), where the cache alone changes.
    void setValue(const QString& key, const QVariant& value);

    /// A SCRIPTED TEST RUN (POSESTUDIO_IK_SCRIPT, tools/ikscripts) is ISOLATED from the stored
    /// preferences: a script may drive a preferences page or a tab's switches, and what it sets
    /// must serve that run and never become the user's own settings — so setValue() updates the
    /// cache and writes nothing, and the appearance settings a run's pictures depend on
    /// (Preferences → Selection) start at their defaults whatever is stored. With
    /// POSESTUDIO_IK_SCRIPT_PREFS=1 a scripted run reads and writes the preferences as any
    /// other run does (how the storing itself is checked).
    static bool isolatedRun();

    /// The folder stored under `key` if it is set and still exists on disk, else `fallback`.
    /// The one implementation of the "start the file dialog where the user was last time"
    /// rule shared by File → Import, Add Asset Folder (both entry points), and pose save/load —
    /// a remembered folder that was since moved or deleted must never leave a dialog stranded.
    QString rememberedDirectory(const QString& key, const QString& fallback) const;

private:
    PreferencesManager() = default;
    ~PreferencesManager() = default;

    QHash<QString, QVariant> m_preferences;
};

#endif // PREFERENCESMANAGER_H