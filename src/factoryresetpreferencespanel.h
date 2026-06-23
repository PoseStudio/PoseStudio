/**
 * @file factoryresetpreferencespanel.h
 * @brief "Factory Reset" page of the Preferences dialog.
 */

#ifndef FACTORYRESETPREFERENCESPANEL_H
#define FACTORYRESETPREFERENCESPANEL_H

#include "preferencespanel.h"

/// Destructive page that wipes the database and relaunches the app once confirmed.
class FactoryResetPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit FactoryResetPreferencesPanel(QWidget* parent = nullptr);
};

#endif // FACTORYRESETPREFERENCESPANEL_H
