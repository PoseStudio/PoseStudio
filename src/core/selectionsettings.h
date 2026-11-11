/**
 * @file selectionsettings.h
 * @brief The stored half of Preferences → Selection: the color and the opacity of the outline
 *        around the selected object and of the highlight on the part a selected joint moves.
 *
 * Two marks, each with a color (as the screen shows it) and an opacity in percent. The page
 * (preferences/selectionpreferencespanel) writes them, main.cpp hands the stored values to the
 * viewport at startup, and the viewport draws with them (ViewportWidget::setSelectionOutline /
 * setJointHighlight → rendering/selectionstyle.h). Absent or unreadable = the default: the
 * app's accent blue for both, the outline in full, the highlight at 75 %.
 *
 * Qt GUI thread only (it reads PreferencesManager).
 */

#ifndef SELECTIONSETTINGS_H
#define SELECTIONSETTINGS_H

#include <QColor>

class SelectionSettings {
public:
    /// The default color of both marks: the app's accent (Constants::COLOR_ACCENT).
    static QColor defaultColor();
    /// The default opacities, in percent. (The joint highlight's is the 0.75 the viewport was
    /// written with — kJointHighlightDefaultOpacity, held equal by a static_assert in
    /// viewportwidget.cpp.)
    static constexpr double kDefaultOutlineOpacity = 100.0;
    static constexpr double kDefaultJointOpacity = 75.0;

    /// The outline around the selected object.
    static QColor outlineColor();
    static void   setOutlineColor(const QColor& color);
    static double outlineOpacity(); ///< percent, 0..100
    static void   setOutlineOpacity(double percent);

    /// The highlight on the part of the body a selected joint moves.
    static QColor jointColor();
    static void   setJointColor(const QColor& color);
    static double jointOpacity(); ///< percent, 0..100
    static void   setJointOpacity(double percent);

    /// Each mark's switch (absent = on). Off, the mark is not drawn; its color and opacity
    /// stay as they are for when it is switched back on.
    static bool outlineEnabled();
    static void setOutlineEnabled(bool enabled);
    static bool jointEnabled();
    static void setJointEnabled(bool enabled);

    /// What the viewport draws with, 0..1: the opacity, or 0 while the mark is switched off.
    static double outlineDrawnOpacity() { return outlineEnabled() ? outlineOpacity() / 100.0 : 0.0; }
    static double jointDrawnOpacity() { return jointEnabled() ? jointOpacity() / 100.0 : 0.0; }

    /// Both marks back at their defaults (and on).
    static void restoreDefaults();
};

#endif // SELECTIONSETTINGS_H
