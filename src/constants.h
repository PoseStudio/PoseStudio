#ifndef CONSTANTS_H
#define CONSTANTS_H

namespace Constants {
    // --- Application Info ---
    // const char* (not QString) avoids a heap allocation for strings that never change.
    inline constexpr const char* APP_NAME = "PoseStudio";
    inline constexpr const char* APP_VERSION = "0.3.0";

    // --- Collections naming conventions ---
    inline const QString TERM_COL_PLURAL = QStringLiteral("Collections");
    inline const QString TERM_COL_SINGULAR = QStringLiteral("Collection");

    // --- Preference keys (rows in the Preferences table) ---
    // Parent directory of the most recently added asset-library folder. The "Add Asset Folder"
    // browser starts here next time, so adding sibling libraries doesn't re-navigate from home.
    // Defined once here because both add-folder entry points (Asset Manager + Preferences) use it.
    inline constexpr const char* PREF_LAST_ASSET_FOLDER_PARENT = "LastAssetFolderParent";

    // Folder of the most recently imported model file. File → Import's browser starts here next
    // time, so importing several models from one folder doesn't re-navigate from Documents.
    inline constexpr const char* PREF_LAST_IMPORT_DIR = "LastImportDir";

    // Newline-separated list of content-library root folders (each directly containing a "data/"
    // subfolder). The figure importer resolves a preset's cross-file references (geometry, morphs,
    // skin, UVs) against these, in addition to auto-detecting the root from the imported file's own
    // location. This is what lets a figure browsed from a presets-only folder (no co-located "data/")
    // still find its geometry. Populated by the on-import "locate content library" recovery prompt.
    inline constexpr const char* PREF_FIGURE_CONTENT_ROOTS = "FigureContentRoots";

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

    // How long an asset drag must hover over a collapsed tree node before it springs open, so the
    // user can drill into a child node without dropping (classic "spring-loaded folder" behavior).
    inline constexpr int DRAG_AUTO_EXPAND_HOVER_MS = 700;

}

#endif // CONSTANTS_H