/**
 * @file softbodypanel.h
 * @brief The "Soft Body Physics" properties tab: the switches and dials of the posing engine's
 *        soft body — the breasts (armaturebreasts.cpp) and the buttocks (armaturebuttocks.cpp).
 *
 * A self-contained docker panel (its own module, like EnvironmentPanel and TransformPanel), built
 * from the shared row builders (propertyrows.h):
 *
 *   Enable soft body physics   the master switch: off, every figure keeps both as sculpted
 *   Breasts                    their own switch; Softness (%) scales everything they do — the
 *                              hang in a pose and the bounce alike — and Bounce (%) only how much
 *                              a quick move or a landing sets them bouncing, on top of it
 *   Buttocks                   their own switch and a Softness (%) of their own
 *   Restore Defaults           everything back to the calibrated amounts, all on
 *
 * The values are PREFERENCES (PREF_SOFTBODY_*): remembered between sessions, read into the
 * engine's process-wide settings (softbody::setBreastMotion …) by applyStoredSettings — at
 * startup before any figure is imported (main.cpp), and by the panel after every change, which
 * then has the viewport re-pose the figures at once (ViewportWidget::applySoftBodySettings).
 * Nothing here is a pose edit: no undo entry, nothing in a pose file. Until 2026-11-07 these
 * switches were a Preferences → Posing page (PosingPreferencesPanel); the user asked for a tab
 * with controls in it ("a toggle on/off, and a slider to control the Softness of the breasts and
 * one for the buttocks"), and one place is better than two.
 */

#ifndef SOFTBODYPANEL_H
#define SOFTBODYPANEL_H

#include <QWidget>

class QCheckBox;
class QPushButton;

namespace pose {

class DragNumberBox;
class ViewportWidget;

class SoftBodyPanel : public QWidget {
    Q_OBJECT

public:
    /// @param viewport The viewport whose figures re-pose when a setting changes. Not owned.
    explicit SoftBodyPanel(ViewportWidget* viewport, QWidget* parent = nullptr);

    /// Reads the stored preferences into the engine's settings — at startup, before any figure
    /// is imported (main.cpp), and by the panel after every change.
    static void applyStoredSettings();

private:
    void buildUi();
    /// Mirrors the stored preferences into the widgets (signals blocked: nothing re-poses).
    void refresh();
    /// Writes one preference, applies the stored settings to the engine and re-poses the figures.
    void store(const char* key, const QString& value);
    /// Every preference back to its default (written as such: on, 100), applied and mirrored.
    void restoreDefaults();

    ViewportWidget* m_viewport = nullptr;
    QCheckBox*      m_enabled = nullptr;
    QCheckBox*      m_breasts = nullptr;
    DragNumberBox*  m_breastSoftness = nullptr;
    DragNumberBox*  m_breastBounce = nullptr;
    QCheckBox*      m_buttocks = nullptr;
    DragNumberBox*  m_buttockSoftness = nullptr;
    QPushButton*    m_reset = nullptr;
};

} // namespace pose

#endif // SOFTBODYPANEL_H
