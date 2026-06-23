/**
 * @file generalpreferencespanel.cpp
 * @brief Implements the "General" preferences page.
 */

#include "generalpreferencespanel.h"

GeneralPreferencesPanel::GeneralPreferencesPanel(QWidget* parent)
    : PreferencesPanel(QStringLiteral("General"), parent) {
    addPlaceholder(QStringLiteral("General settings will appear here."));
}
