// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file environmentpanel.cpp
 * @brief Implementation of EnvironmentPanel. See environmentpanel.h.
 */

#include "environmentpanel.h"

#include "dragnumberbox.h"
#include "hdriselector.h"
#include "menupickerbutton.h"
#include "propertyrows.h"
#include "viewportwidget.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace pose {

// The group, restore-button and row builders are the properties tabs' shared ones (propertyrows.h):
// one builder for every row kind, so the dials, the HDRI row and the backdrop-mode row — and the
// other tabs' rows — can't drift apart in margins or spacing.
using propertyrows::addGroup;
using propertyrows::makeRestoreButton;
using propertyrows::rowWithRestore;

EnvironmentPanel::EnvironmentPanel(ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("EnvironmentPanel"));
    buildUi();
    if (m_viewport) {
        // Undo/redo of a lighting edit applies renderer-side in the viewport; mirror it here so
        // the dials and checkboxes track the restored state.
        connect(m_viewport, &ViewportWidget::lightingRestored, this,
                &EnvironmentPanel::applyRestoredSettings);
        // A loaded project's panorama (File → Open): the caption follows it — setCurrentPath, not a
        // pick, so the selector does not ask the viewport to bake what it just baked.
        connect(m_viewport, &ViewportWidget::environmentRestored, this,
                [this](const QString& path) { m_hdri->setCurrentPath(path); });
    }
}

DragNumberBox* EnvironmentPanel::addValueRow(QFormLayout* form, const QString& label, double min,
                                             double max, double step,
                                             float LightingSettings::*member) {
    auto* box = new DragNumberBox;
    box->setRange(min, max);
    box->setSingleStep(step);
    box->setDecimals(step >= 1.0 ? 0 : 2);
    box->setValue(m_settings.*member);
    // Undo bracketing: one entry per completed gesture (scrub or type-in), not one per increment
    // — the box brackets user-driven changes with these signals around its valueChanged stream.
    connect(box, &DragNumberBox::editingStarted, this, &EnvironmentPanel::beginSettingsEdit);
    connect(box, &DragNumberBox::editingFinished, this, &EnvironmentPanel::commitSettingsEdit);
    // Live push: every value the box lands on updates the member and goes to the viewport.
    connect(box, &DragNumberBox::valueChanged, this, [this, member](double d) {
        m_settings.*member = static_cast<float>(d);
        pushSettings();
    });

    // Per-row restore: snaps just this dial back to its LightingSettings default. setValue on a
    // changed box fires valueChanged, which pushes the updated settings live exactly like a hand
    // edit (and no-ops when already at the default).
    auto* restore = makeRestoreButton();
    connect(restore, &QToolButton::clicked, this, [this, box, member]() {
        beginSettingsEdit();
        box->setValue(LightingSettings().*member);
        commitSettingsEdit(); // registers no entry if the dial was already at its default
    });

    form->addRow(label, rowWithRestore(box, restore));
    m_dials.push_back({box, member});
    return box;
}

QCheckBox* EnvironmentPanel::addToggleRow(QFormLayout* form, const QString& label,
                                          bool LightingSettings::*member) {
    auto* box = new QCheckBox(label);
    box->setChecked(m_settings.*member);
    form->addRow(QString(), box);
    connect(box, &QCheckBox::toggled, this, [this, member](bool on) {
        beginSettingsEdit(); // a toggle is a complete gesture in itself
        m_settings.*member = on;
        pushSettings();
        commitSettingsEdit();
    });
    m_toggles.push_back({box, member});
    return box;
}

void EnvironmentPanel::buildUi() {
    const propertyrows::ScrollColumn tab = propertyrows::makeScrollColumn(this, QStringLiteral("EnvironmentContent"), 12);
    QVBoxLayout*                     col = tab.column;

    // --- Environment ---
    QFormLayout* env = addGroup(col, tr("Environment"));
    buildEnvironmentRow(env);
    addValueRow(env, tr("Rotation°"), 0.0, 360.0, 1.0, &LightingSettings::environmentRotationDeg);

    // --- Backdrop (the visible environment behind the figure; PBR mode only) ---
    QFormLayout* backdrop = addGroup(col, tr("Backdrop"));
    buildBackdropModeRow(backdrop);
    addValueRow(backdrop, tr("Blur"), 0.0, 4.0, 0.05, &LightingSettings::backdropBlur);
    addValueRow(backdrop, tr("Brightness"), 0.0, 2.0, 0.01, &LightingSettings::backdropBrightness);
    addValueRow(backdrop, tr("Dome radius"), 2.0, 100.0, 0.5, &LightingSettings::domeRadius);

    // --- Exposure & Tone ---
    QFormLayout* tone = addGroup(col, tr("Exposure && Tone")); // && — a lone & becomes an accelerator underline
    addValueRow(tone, tr("Exposure"), 0.0, 3.0, 0.01, &LightingSettings::exposure);
    addToggleRow(tone, tr("ACES filmic tonemap"), &LightingSettings::tonemap);

    // --- Image-Based Lighting ---
    QFormLayout* ibl = addGroup(col, tr("Image-Based Lighting"));
    addValueRow(ibl, tr("Diffuse"), 0.0, 3.0, 0.01, &LightingSettings::diffuseIntensity);
    addValueRow(ibl, tr("Specular"), 0.0, 2.0, 0.01, &LightingSettings::specularIntensity);
    addValueRow(ibl, tr("Ambient fill"), 0.0, 1.0, 0.01, &LightingSettings::ambientFill);

    // --- Key Light ---
    QFormLayout* key = addGroup(col, tr("Key Light"));
    addValueRow(key, tr("Intensity"), 0.0, 5.0, 0.05, &LightingSettings::keyIntensity);
    addValueRow(key, tr("Azimuth°"), -180.0, 180.0, 1.0, &LightingSettings::keyAzimuthDeg);
    addValueRow(key, tr("Elevation°"), 0.0, 85.0, 1.0, &LightingSettings::keyElevationDeg);

    // --- Shadow (the key light's shadows: figure self-shadowing + the PCSS ground shadow) ---
    QFormLayout* shadow = addGroup(col, tr("Shadow"));
    addToggleRow(shadow, tr("Enable shadows"), &LightingSettings::shadowsEnabled);
    addValueRow(shadow, tr("Intensity"), 0.0, 1.0, 0.01, &LightingSettings::shadowIntensity);
    addValueRow(shadow, tr("Softness"), 0.0, 1.0, 0.01, &LightingSettings::shadowSoftness);
    addValueRow(shadow, tr("Reach"), 1.0, 25.0, 0.5, &LightingSettings::shadowReach);

    // --- Skin & Rim (PBR-mode accents) ---
    QFormLayout* accents = addGroup(col, tr("Skin && Rim"));
    addValueRow(accents, tr("Subsurface"), 0.0, 1.0, 0.01, &LightingSettings::subsurface);
    addValueRow(accents, tr("Rim light"), 0.0, 1.5, 0.01, &LightingSettings::rimIntensity);

    // --- Reset ---
    auto* reset = new QPushButton(tr("Restore All Defaults"));
    reset->setObjectName(QStringLiteral("PropertyResetButton"));
    col->addWidget(reset, 0, Qt::AlignLeft);
    connect(reset, &QPushButton::clicked, this, &EnvironmentPanel::resetToDefaults);

    col->addStretch(1);
    tab.finish();
}

void EnvironmentPanel::buildEnvironmentRow(QFormLayout* form) {
    m_hdri = new HdriSelector;
    // A pick (or a restore to the default) loads the panorama: an IBL re-bake in the viewport,
    // off the GUI thread. Deliberately NOT on the undo stack, like any HDRI switch.
    connect(m_hdri, &HdriSelector::environmentChosen, this, [this](const QString& path) {
        if (m_viewport) {
            m_viewport->setEnvironment(path);
        }
    });

    // Restore-default for the HDRI row: back to the stock/first panorama (the same resolution the
    // viewport uses at startup). The selector skips it when already there — switching HDRIs
    // re-bakes the IBL, so a no-op click shouldn't cost a bake.
    auto* restore = makeRestoreButton();
    connect(restore, &QToolButton::clicked, m_hdri, &HdriSelector::restoreDefault);

    form->addRow(tr("HDRI"), rowWithRestore(m_hdri, restore));
}

void EnvironmentPanel::buildBackdropModeRow(QFormLayout* form) {
    m_backdropMode = new MenuPickerButton;
    // The HDRI selector's field look, deliberately: the QSS ID selector styles every widget with
    // that object name identically (nothing looks widgets up by it), so the mode picker and the
    // HDRI field above it are one kind of field.
    propertyrows::stylePickerField(m_backdropMode);
    // Entries in LightingSettings::backdropMode order (0 = Off, 1 = Environment, 2 = Dome); the
    // picker shows the one tooltip (Dome's) because an item carries it.
    m_backdropMode->setItems({
        {tr("Off")},
        {tr("Environment")},
        {tr("Dome"), false, tr("Ground-projected dome: the figure stands on the environment's floor")},
    });
    connect(m_backdropMode, &MenuPickerButton::currentChanged, this,
            &EnvironmentPanel::setBackdropMode);
    syncBackdropModeUi();

    auto* restore = makeRestoreButton();
    connect(restore, &QToolButton::clicked, this,
            [this]() { setBackdropMode(LightingSettings().backdropMode); });

    form->addRow(tr("Mode"), rowWithRestore(m_backdropMode, restore));
}

void EnvironmentPanel::setBackdropMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    beginSettingsEdit(); // one undo entry per change (no-op entry when already at this mode)
    m_settings.backdropMode = mode;
    syncBackdropModeUi();
    pushSettings();
    commitSettingsEdit();
}

void EnvironmentPanel::syncBackdropModeUi() {
    if (m_backdropMode != nullptr) {
        // setCurrentIndex is silent (never re-fires currentChanged), so this can't loop.
        m_backdropMode->setCurrentIndex(std::clamp(m_settings.backdropMode, 0, 2));
    }
}

void EnvironmentPanel::pushSettings() {
    if (m_viewport) {
        m_viewport->setLightingSettings(m_settings);
    }
}

void EnvironmentPanel::resetToStartupDefaults() {
    resetToDefaults();        // dials / toggles / backdrop mode (the restore-all gesture)
    m_hdri->restoreDefault(); // HDRI caption back to the stock panorama (re-bakes it)
}

void EnvironmentPanel::resetToDefaults() {
    const LightingSettings defaults;
    // One gesture, one undo entry: the depth counter folds the nested brackets (the checkboxes'
    // toggled handlers and setBackdropMode run their own begin/commit) into this outermost pair.
    beginSettingsEdit();
    // Setting the widgets (unblocked) fires each control's handler, which rebuilds m_settings from the
    // defaults and pushes — so the viewport lands back on the default look.
    for (const DialRow& row : m_dials) {
        row.box->setValue(defaults.*row.member);
    }
    for (const ToggleRow& row : m_toggles) {
        row.box->setChecked(defaults.*row.member);
    }
    setBackdropMode(defaults.backdropMode); // nested begin/commit — folds into this one gesture
    pushSettings(); // covers the case where every widget was already at its default (no signals fired)
    commitSettingsEdit();
}

void EnvironmentPanel::beginSettingsEdit() {
    if (m_editDepth++ == 0) {
        m_preEditSettings = m_settings;
    }
}

void EnvironmentPanel::commitSettingsEdit() {
    if (m_editDepth == 0) {
        return; // unbalanced commit (shouldn't happen) — never underflow the depth
    }
    if (--m_editDepth == 0 && m_viewport && m_settings != m_preEditSettings) {
        m_viewport->registerLightingUndo(m_preEditSettings);
    }
}

void EnvironmentPanel::applyRestoredSettings(const LightingSettings& settings) {
    m_settings = settings;
    // Sync the widgets without firing their handlers: the viewport already applied these values,
    // and firing would re-enter the gesture machinery.
    for (const DialRow& row : m_dials) {
        const QSignalBlocker blocker(row.box);
        row.box->setValue(settings.*row.member);
    }
    for (const ToggleRow& row : m_toggles) {
        const QSignalBlocker blocker(row.box);
        row.box->setChecked(settings.*row.member);
    }
    syncBackdropModeUi(); // caption + check mark (never re-fires currentChanged)
}

} // namespace pose
