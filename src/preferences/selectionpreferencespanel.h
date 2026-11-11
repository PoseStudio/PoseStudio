/**
 * @file selectionpreferencespanel.h
 * @brief "Selection" page of the Preferences dialog: the color and the opacity of the outline
 *        around the selected object, and of the highlight on the part a selected joint moves.
 */

#ifndef SELECTIONPREFERENCESPANEL_H
#define SELECTIONPREFERENCESPANEL_H

#include "preferencespanel.h"

#include <QColor>
#include <QPointer>

class QCheckBox;
class QColorDialog;
class QPushButton;
namespace pose {
class DragNumberBox;
}

/**
 * @class SelectionPreferencesPanel
 * @brief How the viewport marks what is selected — two marks, set apart on one page.
 *
 * Each mark (the OBJECT OUTLINE, the JOINT HIGHLIGHT) has a Color row — a swatch that opens the
 * app's color dialog — and an Opacity row (0–100 %), each with a button that returns it to its
 * default. Every change is announced at once (outlineChanged / jointHighlightChanged — the
 * dialog forwards them to the viewport, which redraws with the new values while the dialog is
 * still open: a color is judged on the figure, as it is picked) and stored through
 * SelectionSettings: a color when its pick is accepted (Cancel puts back and re-announces the
 * color the dialog opened with), an opacity when its scrub or type-in ends.
 */
class SelectionPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit SelectionPreferencesPanel(QWidget* parent = nullptr);

public slots:
    /// A color set by number (0xRRGGBB) and stored — what an accepted pick in the color dialog
    /// does. The scripted test's way in (`ui call selection.panel 0 setOutlineRgb <n>`): a
    /// color dialog cannot be driven from a script.
    void setOutlineRgb(int rgb);
    void setJointRgb(int rgb);

signals:
    /// The outline around the selected object as it now stands: its color (as the screen shows
    /// it) and its opacity, 0..1.
    void outlineChanged(const QColor& color, double opacity);
    /// The same for the highlight on the part a selected joint moves.
    void jointHighlightChanged(const QColor& color, double opacity);

private:
    enum Mark { Outline = 0, Joint = 1, MarkCount = 2 };

    struct MarkRows {
        QCheckBox*           enabled = nullptr; // the mark's switch: off = not drawn, color and opacity kept
        QPushButton*         swatch = nullptr;
        pose::DragNumberBox* opacity = nullptr;
        QColor               color;          // as shown (the stored one, or a pick in progress)
        int                  fieldHeight = 24; // the opacity field's height, which the swatch takes
        bool                 scrubbing = false; // an opacity scrub / type-in is open: stored when it ends
    };

    /// One mark's block: a sub-heading, its Color and Opacity rows, the copy under them.
    void buildMark(Mark mark, const QString& title, const QString& idPrefix, const QString& description);
    /// Shows @p color on the mark's swatch and announces it; @p store also writes the preference.
    void setColor(Mark mark, const QColor& color, bool store);
    void storeOpacity(Mark mark);
    void announce(Mark mark);
    /// Opens the color dialog on the mark (one dialog at a time; a second click raises it).
    void pickColor(Mark mark);

    MarkRows               m_marks[MarkCount];
    QPointer<QColorDialog> m_colorDialog;
    Mark                   m_picking = Outline;
    QColor                 m_colorAtOpen;
};

#endif // SELECTIONPREFERENCESPANEL_H
