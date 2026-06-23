#ifndef CONSTANTS_H
#define CONSTANTS_H

namespace Constants {
    // --- Application Info ---
    // const char* (not QString) avoids a heap allocation for strings that never change.
    inline constexpr const char* APP_NAME = "PoseStudio";
    inline constexpr const char* APP_VERSION = "0.1.3";

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

    // Grid cell text layout — shared between AssetGridDelegate::sizeHint and ::paint
    // so the two never drift apart.
    inline constexpr int GRID_ICON_TOP_MARGIN   = 8;
    inline constexpr int GRID_ICON_TEXT_GAP     = 6; // breathing room between thumbnail and label
    inline constexpr int GRID_TEXT_BOTTOM_MARGIN = 6;

    // =========================================================================
    // UI COLORS (C++ & Rich Text)
    // =========================================================================
    // Accent Colors
    inline constexpr const char* COLOR_ACCENT_BLUE = "#497fd4"; // Used for title counts & highlights

    // Thumbnail Grid Canvas
    inline constexpr const char* COLOR_THUMB_BG_START = "#2a2d30"; // Top gradient color
    inline constexpr const char* COLOR_THUMB_BG_END   = "#0d0d0e"; // Bottom gradient color

    // Tooltips
    inline constexpr const char* COLOR_TOOLTIP_ACCENT = "#5b87cc"; // Blue extension text
    inline constexpr const char* COLOR_TOOLTIP_MUTED  = "#888888"; // Grey path text

    // =========================================================================
    // TIMING & DELAYS
    // =========================================================================

    // How many milliseconds to hover before a tooltip appears (Qt Default is ~700)
    inline constexpr int TOOLTIP_WAKE_DELAY_MS = 750;
    inline constexpr int TOOLTIP_SLEEP_DELAY_MS = 0;
    inline constexpr int TOOLTIP_HIDE_DELAY_MS = 10;

}

#endif // CONSTANTS_H