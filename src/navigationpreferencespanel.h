/**
 * @file navigationpreferencespanel.h
 * @brief "Navigation" page of the Preferences dialog.
 */

#ifndef NAVIGATIONPREFERENCESPANEL_H
#define NAVIGATIONPREFERENCESPANEL_H

#include "preferencespanel.h"

/// Viewport navigation settings. Placeholder for now.
class NavigationPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit NavigationPreferencesPanel(QWidget* parent = nullptr);
};

#endif // NAVIGATIONPREFERENCESPANEL_H
