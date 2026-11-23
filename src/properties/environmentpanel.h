// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file environmentpanel.h
 * @brief The "Environment" properties tab: live controls for the viewport's image-based lighting.
 *
 * A self-contained docker panel (its own module, like AssetManagerWidget and the Preferences
 * panels) that drives the viewport's lighting: the HDRI environment (through HdriSelector), the
 * backdrop drawn behind the figure (mode / blur / brightness / dome radius), exposure and the
 * tonemap toggle, the IBL diffuse/specular levels, an ambient fill, the key light (intensity +
 * aim) and its shadows (enable / intensity / softness / reach), the skin and rim accents, and the
 * environment rotation. Every control except the HDRI selector is a live shader dial (no IBL
 * re-bake), so tweaking is instant — the point being to dial in the values that will become the
 * defaults. It talks to the viewport through the ViewportWidget facade; it owns no Vulkan state.
 *
 * Undo: every completed edit gesture (a scrub or type-in, a per-row restore button, a toggle, the
 * backdrop-mode pick, restore-all) is ONE entry on the viewport's unified undo stack. The panel
 * brackets each gesture with beginSettingsEdit/commitSettingsEdit — depth-counted, so composite
 * gestures fold into one entry — and registers the pre-edit LightingSettings; an undo/redo that
 * lands renderer-side comes back through ViewportWidget::lightingRestored and is mirrored into
 * the widgets with their signals blocked. HDRI switches are deliberately NOT on the stack (a
 * switch is a re-bake, not a cheap state re-apply).
 *
 * The dials are TABLE-DRIVEN: addValueRow / addToggleRow bind a widget to one LightingSettings
 * member and record the pair, and the live push, the per-row restore, restore-all and the
 * restored-state sync all iterate those records — adding a dial is one addValueRow line.
 */

#ifndef ENVIRONMENTPANEL_H
#define ENVIRONMENTPANEL_H

#include "lightingsettings.h"

#include <QWidget>

#include <vector>

class QCheckBox;
class QFormLayout;

namespace pose {

class DragNumberBox;
class HdriSelector;
class MenuPickerButton;
class ViewportWidget;

class EnvironmentPanel : public QWidget {
    Q_OBJECT

public:
    /// @param viewport The viewport this panel controls (its lighting/environment). Not owned.
    explicit EnvironmentPanel(ViewportWidget* viewport, QWidget* parent = nullptr);

    /// Resets the WHOLE panel to the startup defaults — every dial/toggle/backdrop mode through
    /// the restore-all gesture, and the HDRI caption back to the stock panorama (its
    /// environmentChosen re-bakes it). File → New calls this so the panel matches the reset scene.
    void resetToStartupDefaults();

private:
    void buildUi();
    void buildEnvironmentRow(QFormLayout* form); // the HDRI selector + its restore button
    void pushSettings();                         // sends m_settings to the viewport
    void resetToDefaults();

    // Backdrop mode row (Off / Environment / Dome — a MenuPickerButton, the app's picker pattern)
    // and its state plumbing. setBackdropMode is the one mutation path (undo-bracketed);
    // syncBackdropModeUi mirrors m_settings into the picker's caption + check mark.
    void buildBackdropModeRow(QFormLayout* form);
    void setBackdropMode(int mode);
    void syncBackdropModeUi();

    // Undo bracketing: every committed edit gesture (a dial scrub or type-in, a restore button,
    // a toggle, restore-all) is wrapped begin → mutate/push → commit. begin snapshots
    // m_settings once per outermost gesture; commit registers that snapshot on the viewport's
    // undo stack if anything actually changed. Depth-counted so composite edits (restore-all
    // fires the checkbox's own wrapped handler) register as ONE entry.
    void beginSettingsEdit();
    void commitSettingsEdit();
    // Undo/redo landed a lighting state (already applied renderer-side): mirror it into
    // m_settings + the widgets, signals blocked so nothing re-pushes or re-registers.
    void applyRestoredSettings(const LightingSettings& settings);

    // Builds a labelled DragNumberBox row (the app-standard scrub field — drag to change, click to
    // type) into @p form, bound to @p member: its valueChanged writes the member and pushes live,
    // and a small restore button on its right snaps just that dial back to the member's
    // LightingSettings default. The row is recorded in m_dials for restore-all / restored-state
    // sync. Rows are built while m_settings is still default-constructed, so the initial value
    // IS the default.
    DragNumberBox* addValueRow(QFormLayout* form, const QString& label, double min, double max,
                               double step, float LightingSettings::*member);
    // The checkbox analogue for a bool member (no restore button; a toggle is its own gesture).
    QCheckBox* addToggleRow(QFormLayout* form, const QString& label,
                            bool LightingSettings::*member);

    ViewportWidget*  m_viewport = nullptr;
    LightingSettings m_settings;   // current values; the source of truth pushed to the viewport

    LightingSettings m_preEditSettings; // dial state when the outermost gesture began
    int              m_editDepth = 0;   // nested-gesture guard (see beginSettingsEdit)

    HdriSelector*     m_hdri = nullptr;         // the HDRI row's picker (see hdriselector.h)
    MenuPickerButton* m_backdropMode = nullptr; // Off / Environment / Dome

    /// A dial bound to one float member of LightingSettings.
    struct DialRow {
        DragNumberBox*           box;
        float LightingSettings::*member;
    };
    /// A checkbox bound to one bool member.
    struct ToggleRow {
        QCheckBox*              box;
        bool LightingSettings::*member;
    };
    std::vector<DialRow>   m_dials;   // in build (= panel) order
    std::vector<ToggleRow> m_toggles;
};

} // namespace pose

#endif // ENVIRONMENTPANEL_H
