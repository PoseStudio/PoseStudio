/**
 * @file interfacepreferencespanel.cpp
 * @brief Implements the "Interface" preferences page.
 */

#include "interfacepreferencespanel.h"

InterfacePreferencesPanel::InterfacePreferencesPanel(QWidget* parent)
    : PreferencesPanel(QStringLiteral("Interface"), parent) {
    addPlaceholder(QStringLiteral("Interface and appearance settings will appear here."));
}
