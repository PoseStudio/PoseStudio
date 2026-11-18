/**
 * @file factoryresetpreferencespanel.h
 * @brief "Factory Reset" page of the Preferences dialog.
 */

#ifndef FACTORYRESETPREFERENCESPANEL_H
#define FACTORYRESETPREFERENCESPANEL_H

#include "preferencespanel.h"

/// Destructive page: once the confirmation word is typed, its button asks for the database to
/// be wiped and the app relaunched (factoryResetRequested — the shell does it).
class FactoryResetPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit FactoryResetPreferencesPanel(QWidget* parent = nullptr);

signals:
    /// The user confirmed the reset (typed the word, clicked the button).
    void factoryResetRequested();
};

#endif // FACTORYRESETPREFERENCESPANEL_H
