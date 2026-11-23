// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vrpreferencespanel.cpp
 * @brief Implements the "VR" preferences page.
 */

#include "vrpreferencespanel.h"
#include "dragnumberbox.h"
#include "vrsettings.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace {

/// Stores a scale box's value when a gesture on it ENDS — a scrub's release, a typed value's
/// commit — and only if it moved, not at every step of a scrub: both scales are read when VR
/// Mode next starts, so nothing needs the values in between, and each store is a database write.
void storeAtGestureEnd(pose::DragNumberBox* box, void (*store)(double)) {
    QObject::connect(box, &pose::DragNumberBox::editingFinished, box, [box, store, stored = box->value()]() mutable {
        if (box->value() != stored) {
            stored = box->value();
            store(stored);
        }
    });
}

} // namespace

VrPreferencesPanel::VrPreferencesPanel(QWidget* parent)
    : PreferencesPanel(tr("VR"), parent) {

    // --- VR support (opt-in, a restart) ---
    auto* enableToggle = new QCheckBox(tr("Enable VR support (takes effect the next time PoseStudio starts)"), this);
    enableToggle->setChecked(VrSettings::isEnabled());
    contentLayout()->addWidget(enableToggle);
    addDescription(
        tr("View → VR Mode puts the scene in a VR headset, life-size: the figure stands on your floor and "
           "you walk around her. PoseStudio talks to the headset through OpenXR, so any PC VR setup works — "
           "a Meta Quest over Quest Link or Air Link, Virtual Desktop, or SteamVR. Connect the headset "
           "before starting PoseStudio; the headset runtime is consulted as PoseStudio starts, which is "
           "why this setting takes a restart."));
    connect(enableToggle, &QCheckBox::toggled, this, [](bool enabled) {
        VrSettings::setEnabled(enabled);
    });

    // --- What the startup found ---
    addDescription(tr("Status: %1").arg(VrSettings::status()));

    // --- Render scale ---
    // The headset's own recommended per-eye resolution is 1.0; below it the headset image
    // softens but the frame rate climbs on a lighter GPU, above it a strong GPU buys sharpness.
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 8, 0, 0);
    form->setHorizontalSpacing(12);
    auto* scaleBox = new pose::DragNumberBox(this);
    scaleBox->setRange(VrSettings::kMinRenderScale, VrSettings::kMaxRenderScale);
    scaleBox->setSingleStep(0.05);
    scaleBox->setFineStep(0.01); // Shift: hundredths (what is stored)
    scaleBox->setDecimals(2);
    scaleBox->setValue(VrSettings::renderScale());
    scaleBox->setFixedWidth(120);
    form->addRow(rowLabel(tr("Render scale")), scaleBox);
    contentLayout()->addLayout(form);
    addDescription(
        tr("The per-eye render resolution as a fraction of the headset's recommended size. 1.00 is the "
           "headset's own; lower it if the headset image stutters, raise it on a strong GPU for a sharper "
           "picture. Applies when VR Mode next starts."));
    storeAtGestureEnd(scaleBox, &VrSettings::setRenderScale);

    // --- Reduced effects ---
    auto* reducedToggle = new QCheckBox(tr("Reduced effects in the headset"), this);
    reducedToggle->setChecked(VrSettings::reducedEffects());
    contentLayout()->addWidget(reducedToggle);
    addDescription(
        tr("Skips the skin's subsurface blur and the bloom for each eye — a lighter GPU gets its frame rate "
           "back at the cost of some skin realism. The desktop view keeps the full look. Applies when VR Mode "
           "next starts."));
    connect(reducedToggle, &QCheckBox::toggled, this, [](bool reduced) {
        VrSettings::setReducedEffects(reduced);
    });

    // --- Splat scale ---
    auto* splatForm = new QFormLayout();
    splatForm->setContentsMargins(0, 8, 0, 0);
    splatForm->setHorizontalSpacing(12);
    auto* splatBox = new pose::DragNumberBox(this);
    splatBox->setRange(VrSettings::kMinSplatScale, 1.0);
    splatBox->setSingleStep(0.05);
    splatBox->setFineStep(0.01);
    splatBox->setDecimals(2);
    splatBox->setValue(VrSettings::splatScale());
    splatBox->setFixedWidth(120);
    splatForm->addRow(rowLabel(tr("Splat scale")), splatBox);
    contentLayout()->addLayout(splatForm);
    addDescription(
        tr("The most of each eye's size a Gaussian splat scene is drawn at. Drawing the splats is the dearest "
           "part of a headset frame, and PoseStudio lowers the size on the fly whenever a frame runs late and "
           "raises it back toward this as the GPU finds room: at 1.00 the capture is as sharp as the GPU can "
           "afford at the moment, softening as you walk up to it; 0.50 is half the eye's size in each direction, "
           "a quarter of the work. Lower it if the headset image still stutters with a capture loaded; Reduced "
           "effects gives the splats the most room. Applies when VR Mode next starts."));
    storeAtGestureEnd(splatBox, &VrSettings::setSplatScale);
}
