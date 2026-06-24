/**
 * @file inputpreferencespanel.cpp
 * @brief Implements the "Input" preferences page.
 */

#include "inputpreferencespanel.h"

InputPreferencesPanel::InputPreferencesPanel(QWidget* parent)
    : PreferencesPanel(QStringLiteral("Input"), parent) {
    addPlaceholder(QStringLiteral("Keyboard and input settings will appear here."));
}
