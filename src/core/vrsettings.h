/**
 * @file vrsettings.h
 * @brief The VR Mode settings (Preferences → VR) and the viewport's report on whether a headset
 *        is available — the one place the Preferences page, the View menu and the viewport meet.
 *
 * VR support is OPT-IN and takes a restart: the Vulkan instance and device have to be created
 * with the extensions the headset runtime asks for, and the runtime is only consulted at startup
 * when the preference is on (connecting to a runtime at every launch would start SteamVR for
 * users who never put a headset on). ViewportWidget reports the outcome of that startup
 * negotiation here (setStatus / setReady); the Preferences page shows it and the View menu's
 * VR Mode entry explains itself from it when VR cannot start. Qt GUI thread only (it reads
 * PreferencesManager).
 */

#ifndef VRSETTINGS_H
#define VRSETTINGS_H

#include <QString>

class VrSettings {
public:
    /// Preferences → VR → "Enable VR support". Absent = off. Applies at the next start.
    static bool isEnabled();
    static void setEnabled(bool enabled);

    /// The per-eye render resolution as a fraction of the headset's recommended size, in
    /// [kMinRenderScale, kMaxRenderScale] (absent = 1.0). Applies when VR Mode next starts.
    static double renderScale();
    static void setRenderScale(double scale);
    static constexpr double kMinRenderScale = 0.5;
    static constexpr double kMaxRenderScale = 1.5;

    /// Preferences → VR → "Reduced effects in the headset": skip the subsurface blur and bloom
    /// per eye (absent = off). Applies when VR Mode next starts.
    static bool reducedEffects();
    static void setReducedEffects(bool reduced);

    /// What the viewport found at startup, in the user's words ("VR support is off", "No OpenXR
    /// runtime is installed", "Ready: Oculus / Meta Quest 3") — the Preferences page's status
    /// line, and the explanation behind a VR Mode that cannot start.
    static QString status();
    static void setStatus(const QString& status);

    /// Whether VR Mode can be started right now: the runtime and a headset were found at startup
    /// and the Vulkan device was created to the runtime's requirements.
    static bool ready();
    static void setReady(bool ready);
};

#endif // VRSETTINGS_H
