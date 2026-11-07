/**
 * @file softbodysettings.h
 * @brief The process-wide soft-body switches the posing engine reads (Qt-free): whether the
 *        breasts follow the pose (armaturebreasts.cpp), whether the buttocks jiggle with movement
 *        (armaturebuttocks.cpp), and how strongly. Set by the Soft Body Physics tab
 *        (SoftBodyPanel) from the stored preferences; the parts are OFF by default, so the IK
 *        harness and every tool that never sets them run the engine as it was.
 */

#ifndef SOFTBODYSETTINGS_H
#define SOFTBODYSETTINGS_H

namespace pose::softbody {

/// The master switch (the tab's "Enable soft body physics"): off, neither part moves.
inline bool  g_enabled = true;
/// The breasts hang and swing with the chest's orientation against gravity, by their size.
inline bool  g_breastMotion = false;
/// A multiplier on everything the breasts do — the hang and the bounce alike (1 = the calibrated
/// amount; the tab's Softness, 0-200 %).
inline float g_breastStrength = 1.0f;
/// A multiplier on the breasts' DYNAMIC response alone — a jolt's and a landing's — on top of
/// the strength (the tab's Bounce, 0-200 %).
inline float g_breastBounce = 1.0f;
/// The buttocks jiggle with the hips' movement (never a hang).
inline bool  g_buttockMotion = false;
/// A multiplier on the buttocks' jiggle (the tab's Softness of their own).
inline float g_buttockStrength = 1.0f;

inline float clampShare(float v) { return v < 0.0f ? 0.0f : v > 2.0f ? 2.0f : v; }

inline bool  breastMotion() { return g_enabled && g_breastMotion; }
inline bool  buttockMotion() { return g_enabled && g_buttockMotion; }
inline float breastStrength() { return g_breastStrength; }
inline float breastBounce() { return g_breastBounce; }
inline float buttockStrength() { return g_buttockStrength; }
inline void  setEnabled(bool on) { g_enabled = on; }
inline void  setBreastMotion(bool on, float strength, float bounce = 1.0f) {
    g_breastMotion = on;
    g_breastStrength = clampShare(strength);
    g_breastBounce = clampShare(bounce);
}
inline void  setButtockMotion(bool on, float strength = 1.0f) {
    g_buttockMotion = on;
    g_buttockStrength = clampShare(strength);
}

} // namespace pose::softbody

#endif // SOFTBODYSETTINGS_H
