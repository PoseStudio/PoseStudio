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
    scaleBox->setDecimals(2);
    scaleBox->setValue(VrSettings::renderScale());
    scaleBox->setFixedWidth(120);
    form->addRow(tr("Render scale"), scaleBox);
    contentLayout()->addLayout(form);
    addDescription(
        tr("The per-eye render resolution as a fraction of the headset's recommended size. 1.00 is the "
           "headset's own; lower it if the headset image stutters, raise it on a strong GPU for a sharper "
           "picture. Applies when VR Mode next starts."));
    connect(scaleBox, &pose::DragNumberBox::valueChanged, this, [](double value) {
        VrSettings::setRenderScale(value);
    });

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
}
