/**
 * @file preferencesmanager.h
 * @brief Defines the global PreferencesManager Singleton.
 *
 * This file contains the class declaration for the application's preference engine.
 * It enforces a strict Singleton pattern to guarantee that only one unified memory 
 * cache of user settings exists during the application's lifecycle, preventing 
 * database read/write collisions.
 */

#ifndef PREFERENCESMANAGER_H
#define PREFERENCESMANAGER_H

#include <QHash>
#include <QString>
#include <QVariant>

/**
 * @class PreferencesManager
 * @brief A globally accessible, thread-safe manager for user settings.
 * * Utilizes an in-memory `QHash` to cache settings loaded from the SQLite database.
 * This guarantees ultra-fast O(1) read access for the UI and rendering threads,
 * while transparently handling the underlying disk writes when values are updated.
 */
class PreferencesManager {
public:
    /**
     * @brief Retrieves the single, globally accessible instance of the manager.
     * * Implements a "Meyers Singleton" pattern. It is instantiated lazily upon 
     * the first call and is inherently thread-safe under C++11 standards.
     * * @return PreferencesManager& A reference to the active global instance.
     */
    static PreferencesManager& instance() {
        static PreferencesManager instance;
        return instance;
    }

    // =========================================================================
    // [ SINGLETON ENFORCEMENT ]
    // Explicitly delete copy and assignment constructors. This prevents the C++ 
    // compiler from implicitly creating duplicate caches that would fall out of sync.
    // =========================================================================
    PreferencesManager(const PreferencesManager&) = delete;
    PreferencesManager& operator=(const PreferencesManager&) = delete;

    // =========================================================================
    // [ CORE FUNCTIONALITY ]
    // =========================================================================

    /**
     * @brief Reads all stored preferences from the database into the RAM cache.
     */
    void loadFromDatabase();

    /**
     * @brief Retrieves a preference from the high-speed memory cache.
     * * @param key The unique identifier for the preference.
     * @param defaultValue The fallback value returned if the key does not exist.
     * @return QVariant The stored value, type-agnostic.
     */
    QVariant getValue(const QString& key, const QVariant& defaultValue = QVariant()) const;

    /**
     * @brief Updates a preference in the memory cache and persists it to the database.
     * * @param key The unique identifier for the preference.
     * @param value The raw data to be stored.
     */
    void setValue(const QString& key, const QVariant& value);

private:
    /**
     * @brief Private default constructor.
     * Forces the application to use the `instance()` method, guaranteeing 
     * no external instantiation can occur.
     */
    PreferencesManager() = default;
    
    /**
     * @brief Private default destructor.
     */
    ~PreferencesManager() = default;

    /**
     * @brief The high-speed memory cache mapping preference keys to their values.
     */
    QHash<QString, QVariant> m_preferences;
};

#endif // PREFERENCESMANAGER_H