#ifndef CONSTANTS_H
#define CONSTANTS_H

namespace Constants {
    // --- Application Info ---
    // Using const char* instead of QString prevents unnecessary memory 
    // allocation until the string is actually needed by the UI.
    inline constexpr const char* APP_NAME = "PoseStudio";
    inline constexpr const char* APP_VERSION = "0.0.8";

    // --- Database ---
    inline constexpr const char* DB_ERRORS_TABLE = "appErrors";

    // --- Favorites naming conventions ---
    inline const QString TERM_FAV_PLURAL = QStringLiteral("Shortcuts");
    inline const QString TERM_FAV_SINGULAR = QStringLiteral("Shortcut");
    
    // --- Collections naming conventions ---
    inline const QString TERM_COL_PLURAL = QStringLiteral("Collections");
    inline const QString TERM_COL_SINGULAR = QStringLiteral("Collection");

}

#endif // CONSTANTS_H