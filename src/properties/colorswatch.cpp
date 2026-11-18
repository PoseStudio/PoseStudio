/**
 * @file colorswatch.cpp
 * @brief The properties tabs' shared color row parts. See colorswatch.h.
 */

#include "colorswatch.h"

#include <QColorDialog>
#include <QPushButton>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pose::colorswatch {

namespace {

/// Linear → display (sRGB) and back, per channel.
float linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
float srgbToLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

QColor displayColor(const glm::vec3& linear) {
    return QColor::fromRgbF(linearToSrgb(linear.x), linearToSrgb(linear.y), linearToSrgb(linear.z));
}

glm::vec3 linearColor(const QColor& srgb) {
    return glm::vec3(srgbToLinear(static_cast<float>(srgb.redF())), srgbToLinear(static_cast<float>(srgb.greenF())),
                     srgbToLinear(static_cast<float>(srgb.blueF())));
}

QPushButton* makeSwatch() {
    auto* swatch = new QPushButton;
    swatch->setObjectName(QStringLiteral("MaterialColorButton"));
    swatch->setCursor(Qt::PointingHandCursor);
    return swatch;
}

void setSwatchColor(QPushButton* swatch, const QColor& c, bool enabled) {
    const bool dark = c.lightnessF() < 0.5;
    swatch->setText(enabled ? c.name(QColor::HexRgb).toUpper() : QString());
    const QString sheet =
        QStringLiteral("QPushButton#MaterialColorButton { background-color: %1; color: %2; border: 1px solid #4a4b4d; "
                       "border-radius: 4px; min-height: 20px; padding: 2px 8px; }"
                       "QPushButton#MaterialColorButton:hover { border-color: #5b87cc; }"
                       "QPushButton#MaterialColorButton:disabled { background-color: #2a2b2d; color: #6a6b6e; }")
            .arg(c.name(QColor::HexRgb), dark ? QStringLiteral("#e0e0e0") : QStringLiteral("#1b1c1e"));
    if (swatch->styleSheet() != sheet) {
        swatch->setStyleSheet(sheet);
    }
}

void ColorPick::raise() {
    if (m_dialog) {
        m_dialog->raise();
        m_dialog->activateWindow();
    }
}

void ColorPick::open(QWidget* owner, const QString& title, const QColor& initial, std::function<void(const QColor&)> live,
                     std::function<void(Outcome)> done) {
    // The application's own themed dialog (the native one ignores the dark theme).
    auto* dialog = new QColorDialog(initial, owner);
    dialog->setOption(QColorDialog::DontUseNativeDialog, true);
    dialog->setWindowTitle(title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_dialog = dialog;
    m_abandoned = false;
    QObject::connect(dialog, &QColorDialog::currentColorChanged, owner, [live = std::move(live)](const QColor& color) { live(color); });
    QObject::connect(dialog, &QColorDialog::finished, owner, [this, done = std::move(done)](int result) {
        const bool abandoned = m_abandoned;
        m_abandoned = false;
        m_dialog = nullptr; // (before the caller hears of it: see open()'s contract)
        done(abandoned ? Outcome::Abandoned : result == QDialog::Accepted ? Outcome::Accepted : Outcome::Cancelled);
    });
    dialog->open();
}

void ColorPick::abandon() {
    if (m_dialog) {
        m_abandoned = true;
        m_dialog->close();
    }
}

} // namespace pose::colorswatch
