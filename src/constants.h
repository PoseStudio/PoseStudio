#ifndef CONSTANTS_H
#define CONSTANTS_H

namespace Constants {
    // --- Application Info ---
    // Using const char* instead of QString prevents unnecessary memory 
    // allocation until the string is actually needed by the UI.
    inline constexpr const char* APP_NAME = "PoseStudio";
    inline constexpr const char* APP_VERSION = "0.0.7";

    // --- Database ---
    inline constexpr const char* DB_ERRORS_TABLE = "appErrors";
}

#endif // CONSTANTS_H