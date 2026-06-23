/**
 * @file inputpreferencespanel.h
 * @brief "Input" page of the Preferences dialog.
 */

#ifndef INPUTPREFERENCESPANEL_H
#define INPUTPREFERENCESPANEL_H

#include "preferencespanel.h"

/// Keyboard & input settings. Placeholder for now.
class InputPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit InputPreferencesPanel(QWidget* parent = nullptr);
};

#endif // INPUTPREFERENCESPANEL_H
