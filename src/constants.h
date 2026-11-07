/**
 * @file constants.h
 * @brief Cross-cutting compile-time constants: app identity, preference keys, shared UI
 *        geometry/colours/timings.
 *
 * Anything two or more subsystems agree on lives here rather than as duplicated magic numbers
 * (e.g. the grid delegates' cell geometry, the one separator grey every divider uses). Viewport-
 * internal tuning that nothing outside src/viewport/ needs stays next to its use; promote a value
 * here only once a second subsystem depends on it.
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <QString>

// The version string is stamped by CMake from project(PoseStudio VERSION x.y.z) — the ONE place
// a release bump edits (CHANGELOG.md is the human-readable record of the same number). A build
// outside CMake would show the placeholder below, which the install ping's server rejects.
#ifndef POSESTUDIO_VERSION
#define POSESTUDIO_VERSION "0.0.0"
#endif

namespace Constants {
    // --- Application Info ---
    // const char* (not QString) avoids a heap allocation for strings that never change.
    inline constexpr const char* APP_NAME = "PoseStudio";
    inline constexpr const char* APP_VERSION = POSESTUDIO_VERSION;

    // Folder name of the default per-user asset library ("My PoseStudio Library"). Created in the
    // user's Documents on first launch (unless a library by this name is already registered) and
    // resolved through the AssetLibraries table thereafter — see src/core/librarypaths.h. Its hdri/
    // subfolder is the user-facing home for environment panoramas.
    inline constexpr const char* USER_LIBRARY_DIRNAME = "My PoseStudio Library";

    // --- Preference keys (rows in the Preferences table) ---
    // Parent directory of the most recently added asset-library folder. The "Add Asset Folder"
    // browser starts here next time, so adding sibling libraries doesn't re-navigate from home.
    // Defined once here because both add-folder entry points (Asset Manager + Preferences) use it.
    inline constexpr const char* PREF_LAST_ASSET_FOLDER_PARENT = "LastAssetFolderParent";

    // Folder of the most recently imported model file. File → Import's browser starts here next
    // time, so importing several models from one folder doesn't re-navigate from Documents.
    inline constexpr const char* PREF_LAST_IMPORT_DIR = "LastImportDir";

    // Folder of the most recently saved or loaded .pose file — File → Save Pose / Load Pose start
    // here next time (a pose library lives somewhere specific; Documents every time was a chore).
    inline constexpr const char* PREF_LAST_POSE_DIR = "LastPoseDir";

    // Folder of the most recently saved or opened .pss project — File → Save / Save As / Open
    // start here next time (a project's figures live somewhere specific, same as its pose files).
    inline constexpr const char* PREF_LAST_PROJECT_DIR = "LastProjectDir";

    // Newline-separated list of content-root folders (each directly containing a "data/"
    // subfolder). The figure importer resolves a preset's cross-file references (geometry, morphs,
    // skin, UVs) against these, in addition to auto-detecting the root from the imported file's own
    // location. This is what lets a figure browsed from a presets-only folder (no co-located "data/")
    // still find its geometry. Populated by the on-import "locate content folder" recovery prompt.
    inline constexpr const char* PREF_FIGURE_CONTENT_ROOTS = "FigureContentRoots";

    // "1"/"0" — whether camera moves (orbit/pan/dolly, the view hotkeys, Home) count as unsaved
    // changes for the close/New prompt. Absent = off: the framing is still saved into the .pss
    // document, but fiddling with the view alone must not trip the prompt.
    inline constexpr const char* PREF_CAMERA_CHANGES_MARK_DIRTY = "CameraChangesMarkDirty";
    // --- Anonymous install ping (src/core/installping.h) ---
    // Random per-installation UUID the ping carries; created on first use. A Factory Reset wipes
    // the Preferences table, so a reset install counts as a new one — acceptable.
    inline constexpr const char* PREF_INSTALL_ID = "InstallId";
    // "1"/"0" — the Preferences → General toggle. Absent = on.
    inline constexpr const char* PREF_PING_ENABLED = "InstallPingEnabled";
    // The startup update check (src/core/updatecheck.h): "1"/"0", absent = on; and the one
    // release version the user chose to skip ("0.3.14"), silenced at startup until a newer one.
    inline constexpr const char* PREF_UPDATE_CHECK_ENABLED = "UpdateCheckEnabled";
    inline constexpr const char* PREF_UPDATE_SKIPPED_VERSION = "UpdateCheckSkippedVersion";
    /// The What's New window (src/help/whatsnewdialog.h): whether it opens on the first start of a
    /// new version ("1"/"0", absent = on), and the last version it was decided for.
    inline constexpr const char* PREF_WHATS_NEW_ENABLED = "WhatsNewEnabled";
    inline constexpr const char* PREF_WHATS_NEW_SEEN_VERSION = "WhatsNewSeenVersion";
    /// VR Mode (src/core/vrsettings.h): whether the headset runtime is consulted at startup
    /// ("1"/"0", absent = OFF — opt-in, takes a restart), and the per-eye render scale ("1.00",
    /// a fraction of the headset's recommended resolution, applied when VR Mode next starts).
    /// The Soft Body Physics tab (properties/softbodypanel): the switches (absent = on) and the dials in percent (absent = 100).
    inline constexpr const char* PREF_SOFTBODY_BREASTS = "SoftBodyBreasts";
    inline constexpr const char* PREF_SOFTBODY_BREAST_STRENGTH = "SoftBodyBreastStrength";
    inline constexpr const char* PREF_SOFTBODY_BUTTOCKS = "SoftBodyButtocks";
    inline constexpr const char* PREF_SOFTBODY_ENABLED = "SoftBodyEnabled";                 // the Soft Body Physics tab's master switch (absent = on)
    inline constexpr const char* PREF_SOFTBODY_BREAST_BOUNCE = "SoftBodyBreastBounce";      // the breasts' Bounce dial, 0-200 (absent = 100)
    inline constexpr const char* PREF_SOFTBODY_BUTTOCK_STRENGTH = "SoftBodyButtockStrength"; // the buttocks' own Softness dial, 0-200 (absent = 100)
    inline constexpr const char* PREF_VR_ENABLED = "VrEnabled";
    inline constexpr const char* PREF_VR_RENDER_SCALE = "VrRenderScale";
    /// "1"/"0", absent = off: skip the subsurface blur and bloom in the headset (a lighter GPU's
    /// frame rate over skin realism); applies when VR Mode next starts.
    inline constexpr const char* PREF_VR_REDUCED_EFFECTS = "VrReducedEffects";
    /// The MOST of the eye's size the headset's photo layer — a Gaussian splat scene — is drawn
    /// at (0.25..1, absent = 1): VR Mode's dynamic scale lowers it from here while a frame runs
    /// late (the splats' fill is what a headset frame cannot afford at full size on most GPUs,
    /// hdrtarget.h) and brings it back as the GPU finds room; applies when VR Mode next starts.
    inline constexpr const char* PREF_VR_SPLAT_SCALE = "VrSplatScale";

    // =========================================================================
    // UI DIMENSIONS & LAYOUT
    // =========================================================================
    
    // The logical edge of a grid thumbnail: both the icon rect AssetGridDelegate paints into and
    // the square canvas AssetThumbnailLoader renders (at the device pixel ratio). One value, so
    // a thumbnail is never resampled a second time to fit its cell.
    inline constexpr int GRID_ICON_DISPLAY_SIZE = 120;

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
    // THE app accent blue: the QSS hover borders and check glyphs, the DragNumberBox editor
    // border, the tooltip's extension text, and (sRGB-decoded to linear in the render core) the
    // viewport's selection outline. QSS can't read C++ constants, so the same value is spelled
    // out in the stylesheets — change them together. Distinct from the darker selection FILL
    // blue (#314D7A) the menus and DragNumberBox range fill use.
    inline constexpr const char* COLOR_ACCENT = "#5b87cc";

    // A brighter, more saturated blue used ONLY for the Asset Manager's drag-reorder drop line and
    // its tree drop highlight — it has to read over the selection fill, which the accent doesn't.
    // Not the app accent; don't reach for it for anything else.
    inline constexpr const char* COLOR_ACCENT_BLUE = "#497fd4";

    // Thumbnail Grid Canvas
    inline constexpr const char* COLOR_THUMB_BG_START = "#2a2d30"; // Top gradient color
    inline constexpr const char* COLOR_THUMB_BG_END   = "#0d0d0e"; // Bottom gradient color

    // Tooltips
    inline constexpr const char* COLOR_TOOLTIP_MUTED  = "#888888"; // Grey path text

    // Separator lines — one grey for every divider drawn in C++ (the HDRI menu's category
    // rules) AND, by value, the QSS ones (QMenu::separator in _menumanager.qss, the Asset
    // Manager's tree/search dividers in _assetmanager.qss): a clearly visible mid grey, since a
    // near-black hairline vanishes against the app's dark surfaces. Change all three together.
    inline constexpr const char* COLOR_SEPARATOR = "#6a6b6e";

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