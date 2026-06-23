/**
 * @file systempreferencespanel.cpp
 * @brief Implements the "System" preferences page.
 */

#include "systempreferencespanel.h"

SystemPreferencesPanel::SystemPreferencesPanel(QWidget* parent)
    : PreferencesPanel(QStringLiteral("System"), parent) {
    addPlaceholder(QStringLiteral("System, performance, and storage settings will appear here."));
}
