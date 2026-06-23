/**
 * @file interfacepreferencespanel.h
 * @brief "Interface" page of the Preferences dialog.
 */

#ifndef INTERFACEPREFERENCESPANEL_H
#define INTERFACEPREFERENCESPANEL_H

#include "preferencespanel.h"

/// Interface & appearance settings. Placeholder for now.
class InterfacePreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit InterfacePreferencesPanel(QWidget* parent = nullptr);
};

#endif // INTERFACEPREFERENCESPANEL_H
