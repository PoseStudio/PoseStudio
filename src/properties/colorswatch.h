// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file colorswatch.h
 * @brief The properties tabs' COLOR ROW, shared: the swatch button whose background is the color,
 *        the linear <-> display conversion a color is shown through, and the color dialog that
 *        sets a color live while it is open.
 *
 * The Materials tab's color rows, an overlay card's Base Color and the Lights tab's Color are one
 * control: a button painted in the color with its hex value as the caption, opening the
 * application's own (dark-themed) color dialog. One definition of its look and of the dialog's
 * rules, so the tabs cannot drift apart in either.
 *
 * THE DIALOG IS WINDOW-MODAL (QDialog::open): while it is up the main window — the side panel and
 * the viewport with it — takes no input; the color is judged on the scene BEHIND the dialog,
 * which follows every change (`live`). What it edits can still change under it without the mouse
 * (a script's selection, a VR controller's click): the owner then ABANDONS the pick — the dialog
 * closes, the color stays as picked so far, and nothing is written to whatever is shown now.
 */

#ifndef COLORSWATCH_H
#define COLORSWATCH_H

#include <QColor>
#include <QPointer>
#include <QString>

#include <glm/vec3.hpp>

#include <functional>

class QColorDialog;
class QPushButton;
class QWidget;

namespace pose::colorswatch {

/// A LINEAR color as the screen shows it (sRGB), and a picked display color back to linear. The
/// swatch shows what the screen shows.
QColor    displayColor(const glm::vec3& linear);
glm::vec3 linearColor(const QColor& srgb);

/// The swatch button (#MaterialColorButton) of a color row. The caller wires clicked and gives
/// it its tooltip; setSwatchColor paints it.
QPushButton* makeSwatch();

/// The swatch's look: its background IS @p color, its caption the color's hex value in a
/// contrasting ink (no caption, and the disabled look, when not @p enabled). A swatch that
/// already shows exactly this is left alone — setting a style sheet re-parses it and re-polishes
/// the button, and a tab's refresh comes here for every swatch at every step of a scrub.
void setSwatchColor(QPushButton* swatch, const QColor& color, bool enabled);

/// ONE color dialog at a time for its owner: opened on a color, announcing every color the user
/// moves through, and saying how it ended.
class ColorPick {
public:
    enum class Outcome {
        Accepted,  ///< OK: the color last announced stands
        Cancelled, ///< Cancel, Esc or the dialog's corner: the caller puts the color at open back
        Abandoned, ///< abandon(): the color stays as picked so far, nothing is put back
    };

    bool isOpen() const { return !m_dialog.isNull(); }
    /// Brings the open dialog to the front (a second click on the swatch).
    void raise();
    /// Opens the dialog on @p initial as a child of @p owner. @p live is called with every color
    /// the user moves through; @p done once, when the dialog has closed — isOpen() is false by
    /// then, so whatever the caller refreshes from it finds no pick in progress. Not to be called
    /// while a dialog is open.
    void open(QWidget* owner, const QString& title, const QColor& initial, std::function<void(const QColor&)> live,
              std::function<void(Outcome)> done);
    /// Closes an open dialog as Outcome::Abandoned: what it was opened for is no longer what the
    /// owner shows.
    void abandon();

private:
    QPointer<QColorDialog> m_dialog;
    bool                   m_abandoned = false; ///< the close in progress is abandon()'s
};

} // namespace pose::colorswatch

#endif // COLORSWATCH_H
