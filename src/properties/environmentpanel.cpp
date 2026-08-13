/**
 * @file environmentpanel.cpp
 * @brief Implementation of EnvironmentPanel. See environmentpanel.h.
 */

#include "environmentpanel.h"

#include "viewportwidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace pose {

namespace {
// Builds a titled QGroupBox with a QFormLayout inside, appended to @p parent; returns the form.
QFormLayout* addGroup(QVBoxLayout* parent, const QString& title) {
    auto* box = new QGroupBox(title);
    box->setObjectName(QStringLiteral("EnvironmentGroup"));
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    parent->addWidget(box);
    return form;
}
} // namespace

EnvironmentPanel::EnvironmentPanel(ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("EnvironmentPanel"));
    buildUi();
}

QDoubleSpinBox* EnvironmentPanel::addSliderRow(QFormLayout* form, const QString& label, double min,
                                               double max, double step, double value) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto* slider = new QSlider(Qt::Horizontal, row);
    auto* spin = new QDoubleSpinBox(row);
    const int steps = std::max(1, static_cast<int>(std::lround((max - min) / step)));
    slider->setRange(0, steps);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(step >= 1.0 ? 0 : 2);
    spin->setValue(value);
    spin->setFixedWidth(76);
    slider->setValue(static_cast<int>(std::lround((value - min) / step)));

    // Two-way sync. Slider drag -> set the spin (which fires its valueChanged, updating the field);
    // spin edit -> move the slider with its own signal blocked so it can't loop back.
    QObject::connect(slider, &QSlider::valueChanged, spin, [=](int v) {
        const double d = min + v * step;
        if (std::abs(spin->value() - d) >= step * 0.5) {
            spin->setValue(d);
        }
    });
    QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), slider, [=](double d) {
        const int v = static_cast<int>(std::lround((d - min) / step));
        if (slider->value() != v) {
            QSignalBlocker block(slider);
            slider->setValue(v);
        }
    });

    h->addWidget(slider, 1);
    h->addWidget(spin, 0);
    form->addRow(label, row);
    return spin;
}

void EnvironmentPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("EnvironmentContent"));
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(12, 12, 12, 12);
    col->setSpacing(12);

    // --- Environment ---
    QFormLayout* env = addGroup(col, tr("Environment"));
    m_envCombo = new QComboBox;
    populateEnvironments(m_envCombo);
    env->addRow(tr("HDRI"), m_envCombo);
    m_rotation = addSliderRow(env, tr("Rotation°"), 0.0, 360.0, 1.0, m_settings.environmentRotationDeg);

    // --- Exposure & Tone ---
    QFormLayout* tone = addGroup(col, tr("Exposure & Tone"));
    m_exposure = addSliderRow(tone, tr("Exposure"), 0.0, 3.0, 0.01, m_settings.exposure);
    m_tonemap = new QCheckBox(tr("ACES filmic tonemap"));
    m_tonemap->setChecked(m_settings.tonemap);
    tone->addRow(QString(), m_tonemap);

    // --- Image-Based Lighting ---
    QFormLayout* ibl = addGroup(col, tr("Image-Based Lighting"));
    m_diffuse = addSliderRow(ibl, tr("Diffuse"), 0.0, 3.0, 0.01, m_settings.diffuseIntensity);
    m_specular = addSliderRow(ibl, tr("Specular"), 0.0, 2.0, 0.01, m_settings.specularIntensity);
    m_ambientFill = addSliderRow(ibl, tr("Ambient fill"), 0.0, 1.0, 0.01, m_settings.ambientFill);

    // --- Key Light ---
    QFormLayout* key = addGroup(col, tr("Key Light"));
    m_keyIntensity = addSliderRow(key, tr("Intensity"), 0.0, 5.0, 0.05, m_settings.keyIntensity);

    // --- Reset ---
    auto* reset = new QPushButton(tr("Reset to defaults"));
    reset->setObjectName(QStringLiteral("EnvironmentResetButton"));
    col->addWidget(reset, 0, Qt::AlignLeft);

    col->addStretch(1);
    scroll->setWidget(content);

    // --- Wiring: every control updates m_settings and pushes it live to the viewport ---
    connect(m_rotation, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double d) {
        m_settings.environmentRotationDeg = static_cast<float>(d);
        pushSettings();
    });
    connect(m_exposure, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double d) {
        m_settings.exposure = static_cast<float>(d);
        pushSettings();
    });
    connect(m_tonemap, &QCheckBox::toggled, this, [this](bool on) {
        m_settings.tonemap = on;
        pushSettings();
    });
    connect(m_diffuse, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double d) {
        m_settings.diffuseIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_specular, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double d) {
        m_settings.specularIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_ambientFill, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double d) {
        m_settings.ambientFill = static_cast<float>(d);
        pushSettings();
    });
    connect(m_keyIntensity, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double d) {
        m_settings.keyIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(reset, &QPushButton::clicked, this, &EnvironmentPanel::resetToDefaults);
}

void EnvironmentPanel::populateEnvironments(QComboBox* combo) {
    const QString hdriDir = QCoreApplication::applicationDirPath() + QStringLiteral("/hdri");
    const QStringList files =
        QDir(hdriDir).entryList(QStringList{QStringLiteral("*.hdr")}, QDir::Files, QDir::Name);
    for (const QString& file : files) {
        combo->addItem(QFileInfo(file).completeBaseName(), hdriDir + QLatin1Char('/') + file);
    }
    // Select the default HDRI (matches the viewport's startup default) without firing a redundant
    // re-load; the viewport already has it loaded.
    int idx = combo->findText(QStringLiteral("Golden Gate Hills"));
    if (idx < 0) {
        idx = 0;
    }
    {
        QSignalBlocker block(combo);
        combo->setCurrentIndex(idx);
    }
    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, combo](int i) {
        if (m_viewport && i >= 0) {
            m_viewport->setEnvironment(combo->itemData(i).toString());
        }
    });
}

void EnvironmentPanel::pushSettings() {
    if (m_viewport) {
        m_viewport->setLightingSettings(m_settings);
    }
}

void EnvironmentPanel::resetToDefaults() {
    const LightingSettings defaults;
    // Setting the widgets (unblocked) fires each control's handler, which rebuilds m_settings from the
    // defaults, syncs the sliders, and pushes — so the viewport lands back on the default look.
    m_rotation->setValue(defaults.environmentRotationDeg);
    m_exposure->setValue(defaults.exposure);
    m_tonemap->setChecked(defaults.tonemap);
    m_diffuse->setValue(defaults.diffuseIntensity);
    m_specular->setValue(defaults.specularIntensity);
    m_ambientFill->setValue(defaults.ambientFill);
    m_keyIntensity->setValue(defaults.keyIntensity);
    pushSettings(); // covers the case where every widget was already at its default (no signals fired)
}

} // namespace pose
