/**
 * @file softbodypanel.cpp
 * @brief Implements the "Soft Body Physics" properties tab (see softbodypanel.h).
 */

#include "softbodypanel.h"

#include "constants.h"
#include "dragnumberbox.h"
#include "preferencesmanager.h"
#include "propertyrows.h"
#include "softbodysettings.h"
#include "viewportwidget.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace pose {

namespace {

/// A stored on/off preference; absent = on (the app ships with the soft body on).
bool storedOn(const char* key) {
    const QString v = PreferencesManager::instance().getValue(key).toString();
    return v.isEmpty() ? true : v != QStringLiteral("0");
}

/// A stored 0-200 % dial; absent = 100 (the calibrated amount).
int storedPercent(const char* key) {
    bool      ok = false;
    const int v = PreferencesManager::instance().getValue(key).toString().toInt(&ok);
    return ok ? qBound(0, v, 200) : 100;
}

QString onOff(bool on) {
    return on ? QStringLiteral("1") : QStringLiteral("0");
}

QString percent(double value) {
    return QString::number(static_cast<int>(value + 0.5));
}

DragNumberBox* makePercentBox(const QString& tip) {
    auto* box = new DragNumberBox;
    box->setRange(0.0, 200.0);
    box->setSingleStep(5.0);
    box->setFineStep(1.0); // Shift: whole percent (the dials are stored as whole numbers)
    box->setDecimals(0);
    box->setOrigin(100.0); // the calibrated amount: the bar fills from it, either way
    box->setToolTip(tip);
    return box;
}

} // namespace

void SoftBodyPanel::applyStoredSettings() {
    softbody::setEnabled(storedOn(Constants::PREF_SOFTBODY_ENABLED));
    softbody::setBreastMotion(storedOn(Constants::PREF_SOFTBODY_BREASTS),
                              static_cast<float>(storedPercent(Constants::PREF_SOFTBODY_BREAST_STRENGTH)) / 100.0f,
                              static_cast<float>(storedPercent(Constants::PREF_SOFTBODY_BREAST_BOUNCE)) / 100.0f);
    softbody::setButtockMotion(storedOn(Constants::PREF_SOFTBODY_BUTTOCKS),
                               static_cast<float>(storedPercent(Constants::PREF_SOFTBODY_BUTTOCK_STRENGTH)) / 100.0f);
}

SoftBodyPanel::SoftBodyPanel(ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("SoftBodyPanel"));
    buildUi();
    refresh();
}

void SoftBodyPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("SoftBodyContent"));
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(12, 12, 12, 12);
    col->setSpacing(12);

    // --- The master switch ---
    QFormLayout* master = propertyrows::addGroup(col, tr("Soft Body Physics"));
    m_enabled = new QCheckBox(tr("Enable soft body physics"));
    m_enabled->setToolTip(tr("Turns the soft body on or off for every figure in the scene. Off, the breasts and the "
                             "buttocks stay as the figure was sculpted."));
    master->addRow(m_enabled);
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) { store(Constants::PREF_SOFTBODY_ENABLED, onOff(on)); });

    // --- Breasts ---
    QFormLayout* breasts = propertyrows::addGroup(col, tr("Breasts"));
    m_breasts = new QCheckBox(tr("Breasts follow the pose and bounce"));
    m_breasts->setToolTip(tr("The breasts hang and swing with the chest's orientation against gravity — bending forward, "
                             "lying down, hanging from a hand — and bounce when the chest moves quickly or she lands, "
                             "by the size the figure has: a flat chest does not move."));
    breasts->addRow(m_breasts);
    connect(m_breasts, &QCheckBox::toggled, this, [this](bool on) { store(Constants::PREF_SOFTBODY_BREASTS, onOff(on)); });

    m_breastSoftness = makePercentBox(tr("How far the breasts move — the hang in a pose and the bounce alike. 100 is the "
                                         "calibrated amount for the figure's own bust, 0 holds them as sculpted, 200 "
                                         "doubles it."));
    connect(m_breastSoftness, &DragNumberBox::valueChanged, this,
            [this](double v) { store(Constants::PREF_SOFTBODY_BREAST_STRENGTH, percent(v)); });
    auto* softnessRestore = propertyrows::makeRestoreButton();
    softnessRestore->setToolTip(tr("Back to 100: the calibrated amount"));
    connect(softnessRestore, &QToolButton::clicked, this, [this]() { m_breastSoftness->setValue(100.0); });
    breasts->addRow(tr("Softness (%)"), propertyrows::rowWithRestore(m_breastSoftness, softnessRestore));

    m_breastBounce = makePercentBox(tr("How much a quick move or a landing sets the breasts bouncing, on top of the "
                                       "softness. 100 is the calibrated amount, 0 leaves only the hang of the pose, 200 "
                                       "doubles the bounce."));
    connect(m_breastBounce, &DragNumberBox::valueChanged, this,
            [this](double v) { store(Constants::PREF_SOFTBODY_BREAST_BOUNCE, percent(v)); });
    auto* bounceRestore = propertyrows::makeRestoreButton();
    bounceRestore->setToolTip(tr("Back to 100: the calibrated amount"));
    connect(bounceRestore, &QToolButton::clicked, this, [this]() { m_breastBounce->setValue(100.0); });
    breasts->addRow(tr("Bounce (%)"), propertyrows::rowWithRestore(m_breastBounce, bounceRestore));

    // --- Buttocks ---
    QFormLayout* buttocks = propertyrows::addGroup(col, tr("Buttocks"));
    m_buttocks = new QCheckBox(tr("Buttocks jiggle with movement"));
    m_buttocks->setToolTip(tr("The buttocks jiggle with the hips' movement — a quick drag, a landing — and never hang: a "
                              "pose by itself leaves them as sculpted."));
    buttocks->addRow(m_buttocks);
    connect(m_buttocks, &QCheckBox::toggled, this, [this](bool on) { store(Constants::PREF_SOFTBODY_BUTTOCKS, onOff(on)); });

    m_buttockSoftness = makePercentBox(tr("How far the buttocks jiggle. 100 is the calibrated amount, 0 holds them still, "
                                          "200 doubles it."));
    connect(m_buttockSoftness, &DragNumberBox::valueChanged, this,
            [this](double v) { store(Constants::PREF_SOFTBODY_BUTTOCK_STRENGTH, percent(v)); });
    auto* buttockRestore = propertyrows::makeRestoreButton();
    buttockRestore->setToolTip(tr("Back to 100: the calibrated amount"));
    connect(buttockRestore, &QToolButton::clicked, this, [this]() { m_buttockSoftness->setValue(100.0); });
    buttocks->addRow(tr("Softness (%)"), propertyrows::rowWithRestore(m_buttockSoftness, buttockRestore));

    auto* hint = new QLabel(tr("Every change applies to the figures in the scene at once and is remembered between "
                               "sessions. Nothing here is written into the pose or a pose file."));
    hint->setObjectName(QStringLiteral("SoftBodyHint"));
    hint->setWordWrap(true);
    col->addWidget(hint);

    // --- Restore ---
    m_reset = new QPushButton(tr("Restore Defaults"));
    m_reset->setObjectName(QStringLiteral("PropertyResetButton"));
    m_reset->setToolTip(tr("Everything back to the calibrated amounts, all on"));
    col->addWidget(m_reset, 0, Qt::AlignLeft);
    connect(m_reset, &QPushButton::clicked, this, &SoftBodyPanel::restoreDefaults);

    col->addStretch(1);
    scroll->setWidget(content);
}

void SoftBodyPanel::refresh() {
    const QSignalBlocker b1(m_enabled), b2(m_breasts), b3(m_breastSoftness), b4(m_breastBounce), b5(m_buttocks),
        b6(m_buttockSoftness);
    m_enabled->setChecked(storedOn(Constants::PREF_SOFTBODY_ENABLED));
    m_breasts->setChecked(storedOn(Constants::PREF_SOFTBODY_BREASTS));
    m_breastSoftness->setValue(storedPercent(Constants::PREF_SOFTBODY_BREAST_STRENGTH));
    m_breastBounce->setValue(storedPercent(Constants::PREF_SOFTBODY_BREAST_BOUNCE));
    m_buttocks->setChecked(storedOn(Constants::PREF_SOFTBODY_BUTTOCKS));
    m_buttockSoftness->setValue(storedPercent(Constants::PREF_SOFTBODY_BUTTOCK_STRENGTH));
    // The parts' rows read as off while the master is: what the figure does is the master's say.
    const bool on = m_enabled->isChecked();
    m_breasts->setEnabled(on);
    m_breastSoftness->setEnabled(on && m_breasts->isChecked());
    m_breastBounce->setEnabled(on && m_breasts->isChecked());
    m_buttocks->setEnabled(on);
    m_buttockSoftness->setEnabled(on && m_buttocks->isChecked());
}

void SoftBodyPanel::store(const char* key, const QString& value) {
    PreferencesManager::instance().setValue(key, value);
    applyStoredSettings();
    refresh(); // (the rows' enabled state follows the switches)
    if (m_viewport) {
        m_viewport->applySoftBodySettings();
    }
}

void SoftBodyPanel::restoreDefaults() {
    PreferencesManager& prefs = PreferencesManager::instance();
    for (const char* key : {Constants::PREF_SOFTBODY_ENABLED, Constants::PREF_SOFTBODY_BREASTS, Constants::PREF_SOFTBODY_BUTTOCKS}) {
        prefs.setValue(key, QStringLiteral("1"));
    }
    for (const char* key : {Constants::PREF_SOFTBODY_BREAST_STRENGTH, Constants::PREF_SOFTBODY_BREAST_BOUNCE,
                            Constants::PREF_SOFTBODY_BUTTOCK_STRENGTH}) {
        prefs.setValue(key, QStringLiteral("100"));
    }
    applyStoredSettings();
    refresh();
    if (m_viewport) {
        m_viewport->applySoftBodySettings();
    }
}

} // namespace pose
