#ifndef PREFERENCESMANAGER_H
#define PREFERENCESMANAGER_H

#include <QHash>
#include <QString>
#include <QVariant>

class PreferencesManager {
public:
    // This creates a thread-safe Singleton. 
    // It guarantees only ONE instance of the preferences ever exists in memory.
    static PreferencesManager& instance() {
        static PreferencesManager instance;
        return instance;
    }

    // Prevent anyone from accidentally copying the manager
    PreferencesManager(const PreferencesManager&) = delete;
    PreferencesManager& operator=(const PreferencesManager&) = delete;

    // Core functionality
    void loadFromDatabase();
    QVariant getValue(const QString& key, const QVariant& defaultValue = QVariant()) const;
    void setValue(const QString& key, const QVariant& value);

private:
    // Private constructor enforces the Singleton rule
    PreferencesManager() = default;
    ~PreferencesManager() = default;

    // The in-memory cache for ultra-fast lookups
    QHash<QString, QVariant> m_preferences;
};

#endif // PREFERENCESMANAGER_H