/**
 * @file vrpreferencespanel.h
 * @brief "VR" page of the Preferences dialog: VR support on/off, the headset status, the
 *        per-eye render scale, reduced effects in the headset, and the splat scale.
 */

#ifndef VRPREFERENCESPANEL_H
#define VRPREFERENCESPANEL_H

#include "preferencespanel.h"

/// VR Mode's settings (see src/core/vrsettings.h). The enable toggle takes effect at the next
/// start — the headset runtime is consulted when PoseStudio starts, before the Vulkan device
/// exists — which the page says; the status line is what that startup found. The other three
/// settings are read when VR Mode next starts.
class VrPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit VrPreferencesPanel(QWidget* parent = nullptr);
};

#endif // VRPREFERENCESPANEL_H
