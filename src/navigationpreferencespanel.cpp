/**
 * @file navigationpreferencespanel.cpp
 * @brief Implements the "Navigation" preferences page.
 */

#include "navigationpreferencespanel.h"

NavigationPreferencesPanel::NavigationPreferencesPanel(QWidget* parent)
    : PreferencesPanel(QStringLiteral("Navigation"), parent) {
    addPlaceholder(QStringLiteral("Viewport navigation settings will appear here."));
}
