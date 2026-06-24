/**
 * @file systempreferencespanel.h
 * @brief "System" page of the Preferences dialog.
 */

#ifndef SYSTEMPREFERENCESPANEL_H
#define SYSTEMPREFERENCESPANEL_H

#include "preferencespanel.h"

/// System, performance & storage settings. Placeholder for now.
class SystemPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit SystemPreferencesPanel(QWidget* parent = nullptr);
};

#endif // SYSTEMPREFERENCESPANEL_H
