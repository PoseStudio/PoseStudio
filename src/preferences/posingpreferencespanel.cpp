/**
 * @file posingpreferencespanel.cpp
 * @brief Implements the "Posing" preferences page.
 */

#include "posingpreferencespanel.h"

#include "constants.h"
#include "dragnumberbox.h"
#include "preferencesmanager.h"
#include "softbodysettings.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QVBoxLayout>

namespace {

bool storedBreastMotion() {
    const QString v = PreferencesManager::instance().getValue(Constants::PREF_SOFTBODY_BREASTS).toString();
    return v.isEmpty() ? true : v != QStringLiteral("0"); // (absent = on)
}

bool storedButtockMotion() {
    const QString v = PreferencesManager::instance().getValue(Constants::PREF_SOFTBODY_BUTTOCKS).toString();
    return v.isEmpty() ? true : v != QStringLiteral("0"); // (absent = on)
}

int storedBreastStrengthPercent() {
    bool      ok = false;
    const int v = PreferencesManager::instance().getValue(Constants::PREF_SOFTBODY_BREAST_STRENGTH).toString().toInt(&ok);
    return ok ? qBound(0, v, 200) : 100;
}

} // namespace

void PosingPreferencesPanel::applyStoredSettings() {
    pose::softbody::setBreastMotion(storedBreastMotion(), static_cast<float>(storedBreastStrengthPercent()) / 100.0f);
    pose::softbody::setButtockMotion(storedButtockMotion());
}

PosingPreferencesPanel::PosingPreferencesPanel(QWidget* parent)
    : PreferencesPanel(tr("Posing"), parent) {

    // --- Soft breasts ---
    auto* breastToggle = new QCheckBox(tr("Soft breasts follow the pose"), this);
    breastToggle->setChecked(storedBreastMotion());
    contentLayout()->addWidget(breastToggle);
    addDescription(
        tr("A figure's breasts hang and swing with the chest's orientation against gravity — bending "
           "forward, lying down, hanging from a hand — and bounce a little while you drag and when "
           "she lands from the Ground button, by the size the figure actually has: a flat chest does "
           "not move. Nothing of it is written into the pose or a pose file."));
    connect(breastToggle, &QCheckBox::toggled, this, [this](bool on) {
        PreferencesManager::instance().setValue(Constants::PREF_SOFTBODY_BREASTS, on ? QStringLiteral("1") : QStringLiteral("0"));
        applyStoredSettings();
        emit softBodyChanged();
    });

    // --- Soft buttocks ---
    auto* buttockToggle = new QCheckBox(tr("Soft buttocks jiggle with movement"), this);
    buttockToggle->setChecked(storedButtockMotion());
    contentLayout()->addWidget(buttockToggle);
    addDescription(
        tr("A figure's buttocks bounce a little with the hips' movement — a quick drag, a landing from "
           "the Ground button — at about half the breasts' amount, and never hang: a pose by itself "
           "leaves them as sculpted. Nothing of it is written into the pose or a pose file."));
    connect(buttockToggle, &QCheckBox::toggled, this, [this](bool on) {
        PreferencesManager::instance().setValue(Constants::PREF_SOFTBODY_BUTTOCKS, on ? QStringLiteral("1") : QStringLiteral("0"));
        applyStoredSettings();
        emit softBodyChanged();
    });

    // --- Strength ---
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 8, 0, 0);
    form->setHorizontalSpacing(12);
    auto* strengthBox = new pose::DragNumberBox(this);
    strengthBox->setRange(0.0, 200.0);
    strengthBox->setSingleStep(5.0);
    strengthBox->setDecimals(0);
    strengthBox->setValue(storedBreastStrengthPercent());
    strengthBox->setFixedWidth(120);
    form->addRow(tr("Strength (%)"), strengthBox);
    contentLayout()->addLayout(form);
    addDescription(tr("How far the soft body moves — the breasts and the buttocks alike: 100 is the calibrated amount, 0 holds them as the figure was sculpted."));
    connect(strengthBox, &pose::DragNumberBox::valueChanged, this, [this](double value) {
        PreferencesManager::instance().setValue(Constants::PREF_SOFTBODY_BREAST_STRENGTH, QString::number(static_cast<int>(value + 0.5)));
        applyStoredSettings();
        emit softBodyChanged();
    });
}
