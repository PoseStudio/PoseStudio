#ifndef CONSTANTS_H
#define CONSTANTS_H

namespace Constants {
    // --- Application Info ---
    // Using const char* instead of QString prevents unnecessary memory 
    // allocation until the string is actually needed by the UI.
    inline constexpr const char* APP_NAME = "PoseStudio";
    inline constexpr const char* APP_VERSION = "0.1.0";

    // --- Database ---
    inline constexpr const char* DB_ERRORS_TABLE = "appErrors";

    // --- Favorites naming conventions ---
    inline const QString TERM_FAV_PLURAL = QStringLiteral("Shortcuts");
    inline const QString TERM_FAV_SINGULAR = QStringLiteral("Shortcut");
    
    // --- Collections naming conventions ---
    inline const QString TERM_COL_PLURAL = QStringLiteral("Collections");
    inline const QString TERM_COL_SINGULAR = QStringLiteral("Collection");

    // =========================================================================
    // UI DIMENSIONS & LAYOUT
    // =========================================================================
    
    // The maximum bounding box Qt will use to display an icon in the grid
    inline constexpr int GRID_ICON_DISPLAY_SIZE = 120; 

    // The high-res internal render dimensions for the custom QPainter thumbnail
    inline constexpr int THUMB_RENDER_SIZE = GRID_ICON_DISPLAY_SIZE;
    inline constexpr int THUMB_CANVAS_HEIGHT = (GRID_ICON_DISPLAY_SIZE + 8); // Extra padding for text

    // Grid View Cell Sizes
    inline constexpr int GRID_CELL_WIDTH = (GRID_ICON_DISPLAY_SIZE + 10);
    inline constexpr int GRID_CELL_HEIGHT = (GRID_ICON_DISPLAY_SIZE + 40);

    // =========================================================================
    // UI COLORS (C++ & Rich Text)
    // =========================================================================
    // Accent Colors
    inline constexpr const char* COLOR_ACCENT_BLUE = "#497fd4"; // Used for title counts & highlights
    
    // Tree View
    inline constexpr const char* COLOR_TREE_COUNT  = "#696969"; // Subdued gray for unselected folder hit counts
    inline constexpr const char* COLOR_TREE_SEP    = "#3c3c3c"; // Fallback color for tree separators
    
    // Thumbnail Grid Canvas
    inline constexpr const char* COLOR_THUMB_BG_START = "#2a2d30"; // Top gradient color
    inline constexpr const char* COLOR_THUMB_BG_END   = "#0d0d0e"; // Bottom gradient color

    // Tooltips
    inline constexpr const char* COLOR_TOOLTIP_ACCENT = "#5b87cc"; // Blue extension text
    inline constexpr const char* COLOR_TOOLTIP_MUTED  = "#888888"; // Grey path text

    // =========================================================================
    // UI DIMENSIONS & LAYOUT
    // =========================================================================
    
    // How many milliseconds to hover before a tooltip appears (Qt Default is ~700)
    inline constexpr int TOOLTIP_WAKE_DELAY_MS = 750;
    inline constexpr int TOOLTIP_SLEEP_DELAY_MS = 0;
    inline constexpr int TOOLTIP_HIDE_DELAY_MS = 10;

}

#endif // CONSTANTS_H