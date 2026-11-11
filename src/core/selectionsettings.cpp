/**
 * @file selectionsettings.cpp
 * @brief Implementation of SelectionSettings. See selectionsettings.h.
 */

#include "selectionsettings.h"

#include "constants.h"
#include "preferencesmanager.h"

#include <algorithm>

namespace {

/// A stored "#rrggbb", or the default when the row is absent or does not read as a color.
QColor storedColor(const char* key) {
    const QString name = PreferencesManager::instance().getValue(QString::fromLatin1(key)).toString();
    const QColor  color = QColor::fromString(name);
    return name.isEmpty() || !color.isValid() ? SelectionSettings::defaultColor() : QColor(color.rgb());
}

void storeColor(const char* key, const QColor& color) {
    if (color.isValid()) {
        PreferencesManager::instance().setValue(QString::fromLatin1(key), color.name(QColor::HexRgb));
    }
}

/// A stored percentage, or @p fallback when the row is absent or does not read as a number.
double storedPercent(const char* key, double fallback) {
    bool         ok = false;
    const double stored = PreferencesManager::instance().getValue(QString::fromLatin1(key), fallback).toDouble(&ok);
    return ok ? std::clamp(stored, 0.0, 100.0) : fallback;
}

void storePercent(const char* key, double percent) {
    PreferencesManager::instance().setValue(QString::fromLatin1(key),
                                           QString::number(std::clamp(percent, 0.0, 100.0), 'f', 1)); // (to the tenth a fine scrub sets)
}

} // namespace

QColor SelectionSettings::defaultColor() {
    return QColor::fromString(QLatin1String(Constants::COLOR_ACCENT));
}

QColor SelectionSettings::outlineColor() {
    return storedColor(Constants::PREF_SELECTION_OUTLINE_COLOR);
}

void SelectionSettings::setOutlineColor(const QColor& color) {
    storeColor(Constants::PREF_SELECTION_OUTLINE_COLOR, color);
}

double SelectionSettings::outlineOpacity() {
    return storedPercent(Constants::PREF_SELECTION_OUTLINE_OPACITY, kDefaultOutlineOpacity);
}

void SelectionSettings::setOutlineOpacity(double percent) {
    storePercent(Constants::PREF_SELECTION_OUTLINE_OPACITY, percent);
}

QColor SelectionSettings::jointColor() {
    return storedColor(Constants::PREF_JOINT_HIGHLIGHT_COLOR);
}

void SelectionSettings::setJointColor(const QColor& color) {
    storeColor(Constants::PREF_JOINT_HIGHLIGHT_COLOR, color);
}

double SelectionSettings::jointOpacity() {
    return storedPercent(Constants::PREF_JOINT_HIGHLIGHT_OPACITY, kDefaultJointOpacity);
}

void SelectionSettings::setJointOpacity(double percent) {
    storePercent(Constants::PREF_JOINT_HIGHLIGHT_OPACITY, percent);
}

bool SelectionSettings::outlineEnabled() {
    return PreferencesManager::instance().getValue(QString::fromLatin1(Constants::PREF_SELECTION_OUTLINE_ENABLED)).toString() !=
           QLatin1String("0");
}

void SelectionSettings::setOutlineEnabled(bool enabled) {
    PreferencesManager::instance().setValue(QString::fromLatin1(Constants::PREF_SELECTION_OUTLINE_ENABLED),
                                           enabled ? QStringLiteral("1") : QStringLiteral("0"));
}

bool SelectionSettings::jointEnabled() {
    return PreferencesManager::instance().getValue(QString::fromLatin1(Constants::PREF_JOINT_HIGHLIGHT_ENABLED)).toString() !=
           QLatin1String("0");
}

void SelectionSettings::setJointEnabled(bool enabled) {
    PreferencesManager::instance().setValue(QString::fromLatin1(Constants::PREF_JOINT_HIGHLIGHT_ENABLED),
                                           enabled ? QStringLiteral("1") : QStringLiteral("0"));
}

void SelectionSettings::restoreDefaults() {
    setOutlineEnabled(true);
    setJointEnabled(true);
    setOutlineColor(defaultColor());
    setOutlineOpacity(kDefaultOutlineOpacity);
    setJointColor(defaultColor());
    setJointOpacity(kDefaultJointOpacity);
}
