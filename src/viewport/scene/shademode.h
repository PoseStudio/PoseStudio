/**
 * @file shademode.h
 * @brief The viewport's shade-mode table: the single source of truth behind the shader picker.
 *
 * Each row names one picker entry and says how the scene draws in it: which mesh.frag mode (if
 * any) shades the surface, whether that surface is real shading, absent, or a hidden-line depth
 * fill, and whether every mesh's triangle edges are drawn over it (wireframe). The picker
 * (ViewportWidget) lists the names in this order with separators where flagged, and the index it
 * hands down through setShadeMode() is an index into THIS table — never a mesh.frag mode
 * directly; the fragMode column is the one place the two meet, so mesh.frag's own mode numbering
 * can stay stable while the picker is rearranged. Qt-free (plain constexpr data — the Qt layer
 * reads the names, the Scene reads the rest).
 *
 * The rows fall into four groups, by what the user is trying to see: LIT shading of the real
 * materials (PBR through Cartoon — with RENDERED, the render engine's path-traced picture of the
 * same scene, second), UNTEXTURED form studies (Matcap through Silhouette), the
 * WIREFRAMES, and the DATA / diagnostic views (Albedo through UV Checker).
 */

#ifndef SHADEMODE_H
#define SHADEMODE_H

namespace pose {

/// How a shade mode draws each model's surface.
enum class FillKind {
    Shaded,     ///< The lit/unlit mesh passes, shaded by mesh.frag's `fragMode`.
    None,       ///< No surface at all (the wire-only mode).
    HiddenLine, ///< Depth only: the surface occludes (the grid, the wires behind it) but shows the
                ///< viewport's clear color — the classic hidden-line drawing.
};

struct ShadeMode {
    const char* name;           ///< Picker label.
    int         fragMode;       ///< mesh.frag mode of the shaded surface (see its table); -1 = none.
    FillKind    fill;
    bool        wireframe;      ///< Draw every mesh's triangle edges (over the surface, if any).
    float       wireLevel;      ///< Linear grey of the wire lines (rides in UBO params4.w).
    bool        separatorAfter; ///< The picker draws a separator under this row.
    /// THE RENDERED MODE: the viewport shows the scene PATH TRACED by the render engine
    /// (VulkanWindow's traced view; rendering/tracedview.h) — and, while a pose is being changed
    /// or the engine has not caught up, the row's own rasterized shading (its fragMode).
    bool        pathTraced = false;
};

// mesh.frag's mode numbers (its `cam.params.x` switch). The skin and metal matcaps (3, 4) still
// exist in the shader but have no picker row.
inline constexpr int kFragModeTextured   = 0;  // "Rendered": textured Blinn-Phong, analytic 3-point rig
inline constexpr int kFragModePbr        = 1;  // image-based-lit; the HDR post chain (SSS/bloom/ACES) runs here
inline constexpr int kFragModeMatcap     = 2;  // the studio matcap (procedural, view-space normal)
inline constexpr int kFragModeToon       = 5;
inline constexpr int kFragModeClay       = 6;
inline constexpr int kFragModeLighting   = 7;  // shading on a neutral white surface
inline constexpr int kFragModeFlat       = 8;  // textured, per-triangle normal
inline constexpr int kFragModeNormals    = 9;
inline constexpr int kFragModeAlbedo     = 10;
inline constexpr int kFragModeUvChecker  = 11;
inline constexpr int kFragModeAo         = 12; // the baked per-vertex ambient occlusion, as grey
inline constexpr int kFragModeSilhouette = 13; // flat light fill: pose readability
inline constexpr int kFragModeSpecular   = 14; // the PBR mode's specular reflection alone (no diffuse, no rim)
inline constexpr int kFragModeRoughness  = 15; // the material roughness (scalar × map) as grey: white = matte

/// The PBR family: modes that render LINEAR HDR through the image-based path and need the HDR
/// post chain (SSS, bloom, ACES) and the HDRI backdrop — PBR Shaded and its specular-only view.
inline constexpr bool fragModeIsHdr(int fragMode) {
    return fragMode == kFragModePbr || fragMode == kFragModeSpecular;
}

// Wire greys, LINEAR (the non-PBR modes pass through the composite untonemapped and the sRGB
// swapchain encodes them): a light wire on the dark viewport grey for the wire-only modes, a
// near-black wire drawn over a lit surface.
inline constexpr float kWireOnDark = 0.55f;
inline constexpr float kWireOnFill = 0.02f;

inline constexpr ShadeMode kShadeModes[] = {
    // Lit shading of the real materials.
    {"PBR Shaded",               kFragModePbr,        FillKind::Shaded,     false, 0.0f,        false},
    // The same scene path traced by the render engine: true shadows from every light, bounced
    // light, reflections of the scene itself, glass that bends what is behind it. The PBR
    // shading stands in while a pose is being changed.
    {"Rendered",                 kFragModePbr,        FillKind::Shaded,     false, 0.0f,        false, true},
    {"Texture Shaded",           kFragModeTextured,   FillKind::Shaded,     false, 0.0f,        false},
    {"Flat Texture Shaded",      kFragModeFlat,       FillKind::Shaded,     false, 0.0f,        false},
    {"Cartoon Shaded",           kFragModeToon,       FillKind::Shaded,     false, 0.0f,        true},
    // Untextured form studies. "Lighting Only" is the standard name for a white-material lighting
    // check (a neutral surface WITH a specular highlight); Clay is lit too — matte and diffuse-only
    // — so a "(Lit)" qualifier on either would mislead.
    {"Matcap",                   kFragModeMatcap,     FillKind::Shaded,     false, 0.0f,        false},
    {"Clay Shaded",              kFragModeClay,       FillKind::Shaded,     false, 0.0f,        false},
    {"Lighting Only",            kFragModeLighting,   FillKind::Shaded,     false, 0.0f,        false},
    {"Silhouette",               kFragModeSilhouette, FillKind::Shaded,     false, 0.0f,        true},
    // Wireframes. "Hidden line" is the technique's name: edges only, with the ones a surface
    // hides removed.
    {"Wireframe",                -1,                  FillKind::None,       true,  kWireOnDark, false},
    {"Hidden Line Wireframe",    -1,                  FillKind::HiddenLine, true,  kWireOnDark, false},
    {"Clay Shaded Wireframe",    kFragModeClay,       FillKind::Shaded,     true,  kWireOnFill, false},
    {"Texture Shaded Wireframe", kFragModeTextured,   FillKind::Shaded,     true,  kWireOnFill, true},
    // Data views. Roughness Map is the map itself; Specular Only is the specular LIGHTING that
    // map (with the spec weight/mask) produces — a component pass, not a map.
    {"Albedo",                   kFragModeAlbedo,     FillKind::Shaded,     false, 0.0f,        false},
    {"Ambient Occlusion",        kFragModeAo,         FillKind::Shaded,     false, 0.0f,        false},
    {"Roughness Map",            kFragModeRoughness,  FillKind::Shaded,     false, 0.0f,        false},
    {"Specular Only",            kFragModeSpecular,   FillKind::Shaded,     false, 0.0f,        false},
    {"Normals",                  kFragModeNormals,    FillKind::Shaded,     false, 0.0f,        false},
    {"UV Checker",               kFragModeUvChecker,  FillKind::Shaded,     false, 0.0f,        false},
};
inline constexpr int kShadeModeCount =
    static_cast<int>(sizeof(kShadeModes) / sizeof(kShadeModes[0]));
inline constexpr int kDefaultShadeMode = 0; // PBR Shaded

/// The table row for picker index @p index (clamped into range, so a stale index can't read
/// past the table).
inline const ShadeMode& shadeModeAt(int index) {
    if (index < 0) {
        index = 0;
    }
    if (index >= kShadeModeCount) {
        index = kShadeModeCount - 1;
    }
    return kShadeModes[index];
}

} // namespace pose

#endif // SHADEMODE_H
