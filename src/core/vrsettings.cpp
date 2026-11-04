/**
 * @file vrsettings.cpp
 * @brief Implementation of VrSettings. See vrsettings.h.
 */

#include "vrsettings.h"

#include "constants.h"
#include "preferencesmanager.h"

#include <algorithm>

namespace {
// The startup report lives for the process: the viewport writes it once, the UI reads it.
QString g_status = QStringLiteral("VR support is off.");
bool    g_ready = false;
} // namespace

bool VrSettings::isEnabled() {
    // Absent = off (opt-in, see the header). Stored as "1"/"0".
    return PreferencesManager::instance().getValue(Constants::PREF_VR_ENABLED, false).toBool();
}

void VrSettings::setEnabled(bool enabled) {
    PreferencesManager::instance().setValue(Constants::PREF_VR_ENABLED,
                                           enabled ? QStringLiteral("1") : QStringLiteral("0"));
}

double VrSettings::renderScale() {
    bool ok = false;
    const double stored =
        PreferencesManager::instance().getValue(Constants::PREF_VR_RENDER_SCALE, 1.0).toDouble(&ok);
    return ok ? std::clamp(stored, kMinRenderScale, kMaxRenderScale) : 1.0;
}

void VrSettings::setRenderScale(double scale) {
    PreferencesManager::instance().setValue(
        Constants::PREF_VR_RENDER_SCALE,
        QString::number(std::clamp(scale, kMinRenderScale, kMaxRenderScale), 'f', 2));
}

bool VrSettings::reducedEffects() {
    return PreferencesManager::instance().getValue(Constants::PREF_VR_REDUCED_EFFECTS, false).toBool();
}

void VrSettings::setReducedEffects(bool reduced) {
    PreferencesManager::instance().setValue(Constants::PREF_VR_REDUCED_EFFECTS,
                                           reduced ? QStringLiteral("1") : QStringLiteral("0"));
}

QString VrSettings::status() {
    return g_status;
}

void VrSettings::setStatus(const QString& status) {
    g_status = status;
}

bool VrSettings::ready() {
    return g_ready;
}

void VrSettings::setReady(bool ready) {
    g_ready = ready;
}
