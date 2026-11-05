/**
 * @file posingpreferencespanel.h
 * @brief The "Posing" preferences page: the soft-body switches of the posing engine — whether
 *        the breasts follow the pose (armaturebreasts.cpp) and how strongly. Writes the stored
 *        preferences and the engine's process-wide settings (softbody::setBreastMotion), and
 *        tells the dialog (softBodyChanged) so the viewport re-poses the figures at once.
 */

#ifndef POSINGPREFERENCESPANEL_H
#define POSINGPREFERENCESPANEL_H

#include "preferencespanel.h"

class PosingPreferencesPanel : public PreferencesPanel {
    Q_OBJECT
public:
    explicit PosingPreferencesPanel(QWidget* parent = nullptr);

    /// Reads the stored preferences into the engine's settings — at startup, before any figure
    /// is imported (main.cpp), and by the panel after every change.
    static void applyStoredSettings();

signals:
    /// A soft-body setting changed: the figures in the scene should be re-posed with it.
    void softBodyChanged();
};

#endif // POSINGPREFERENCESPANEL_H
