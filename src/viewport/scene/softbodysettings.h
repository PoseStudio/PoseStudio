/**
 * @file softbodysettings.h
 * @brief The process-wide soft-body switches the posing engine reads (Qt-free): whether the
 *        breasts follow the pose (armaturebreasts.cpp) and how strongly. Set by the Posing
 *        preferences page (PosingPreferencesPanel) from the stored preferences; OFF by default,
 *        so the IK harness and every tool that never sets them run the engine as it was.
 */

#ifndef SOFTBODYSETTINGS_H
#define SOFTBODYSETTINGS_H

namespace pose::softbody {

/// The breasts hang and swing with the chest's orientation against gravity, by their size.
inline bool  g_breastMotion = false;
/// A multiplier on the swing (1 = the calibrated amount; the preference's 0-200 %).
inline float g_breastStrength = 1.0f;

inline bool  breastMotion() { return g_breastMotion; }
inline float breastStrength() { return g_breastStrength; }
inline void  setBreastMotion(bool on, float strength) {
    g_breastMotion = on;
    g_breastStrength = strength < 0.0f ? 0.0f : strength > 2.0f ? 2.0f : strength;
}

} // namespace pose::softbody

#endif // SOFTBODYSETTINGS_H
