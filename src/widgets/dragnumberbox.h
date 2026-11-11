/**
 * @file dragnumberbox.h
 * @brief The app-standard numeric input: a "drag number box" (scrub field).
 *
 * One compact control that replaces the slider + spin box pair for bounded numeric
 * parameters (the DCC-app convention — Blender/Substance-style):
 *  - Background: a left-to-right fill proportional to (value - min) / (max - min),
 *    so the box doubles as a progress-bar readout of where the value sits in range.
 *    A box given an ORIGIN inside its range (setOrigin: a dial whose rest value is 0
 *    between a negative and a positive limit) fills from the origin to the value
 *    instead — empty at rest, growing either way — with a hairline marking the origin.
 *  - Foreground: the current value, centered.
 *  - Press + horizontal drag scrubs the value (a full-width sweep covers the whole
 *    range — or, for a box given a SCRUB SPAN, that much of it: a placement whose range
 *    is wide and whose useful moves are small). The cursor shows a left-right arrow
 *    whenever it hovers the box, so the scrubbability is discoverable before pressing.
 *  - SHIFT + drag is the FINE scrub: a tenth of the travel and a tenth of the step — a
 *    dial that scrubs in whole numbers is set to a tenth with Shift held. Shift may go
 *    down or up in the middle of a scrub; the scrub carries on from where it is at the
 *    other pace. A value off the regular grid (set fine, typed, or simply what the scene
 *    has) is shown to the fine step's digits — 17.3 — and a value on it as ever — 17.
 *  - A plain click (press + release with no drag) opens inline type-in editing.
 *
 * Use this for any bounded scalar parameter row (properties panels, future material
 * dials) instead of hand-rolling QSlider/QDoubleSpinBox combos. The API mirrors the
 * QDoubleSpinBox essentials (setRange / setSingleStep / setDecimals / setValue /
 * valueChanged) so swapping one in is mechanical.
 *
 * Colors are Q_PROPERTYs (the AssetTreeView pattern) so QSS can re-theme the painted
 * look per-container (qproperty-fillColor: ...); the defaults match the app palette,
 * so no QSS is required.
 */

#ifndef DRAGNUMBERBOX_H
#define DRAGNUMBERBOX_H

#include "constants.h"

#include <QColor>
#include <QWidget>

class QFocusEvent;
class QHideEvent;
class QLineEdit;

namespace pose {

/**
 * @class DragNumberBox
 * @brief Scrubbable, click-to-edit numeric field with a range-fill background.
 */
class DragNumberBox : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QColor backgroundColor MEMBER m_backgroundColor)
    Q_PROPERTY(QColor fillColor MEMBER m_fillColor)
    Q_PROPERTY(QColor borderColor MEMBER m_borderColor)
    Q_PROPERTY(QColor hoverBorderColor MEMBER m_hoverBorderColor)
    Q_PROPERTY(QColor textColor MEMBER m_textColor)

public:
    explicit DragNumberBox(QWidget* parent = nullptr);

    /// Sets [min, max] and re-clamps the current value into it — so this CAN emit valueChanged
    /// (when the old value falls outside the new range). Set the range before wiring listeners
    /// if that matters.
    void setRange(double min, double max);
    /// The value delta one "notch" of precision represents. Drag positions snap to it.
    void setSingleStep(double step);
    /// Fractional digits shown in the label and the type-in editor.
    void setDecimals(int decimals);
    /// The value the FILL starts from and the scrub's step grid is anchored at — the minimum
    /// until one is set. For a range that runs both ways from a rest value (a dial from -7 to
    /// 100 about 0): the bar is empty at the origin and fills toward the value on either side,
    /// and a scrub lands on origin + n * step, so the origin itself is always reachable (anchored
    /// at a fractional minimum the grid would step over it).
    void setOrigin(double origin);
    /// How much of the value a full-width sweep of the box scrubs — the whole range until one
    /// is set. For a value whose range is wide but whose useful moves are small (a placement:
    /// anywhere on a map, set to a fraction of a percent): the scrub is as fine as the span
    /// says, and a longer move is several sweeps, or typed. 0 or less = the whole range again.
    void setScrubSpan(double span);
    /// The step of the FINE scrub (Shift held) — a tenth of the single step until one is set.
    /// 0 = no fine scrub: a whole COUNT (subdivision levels, repeats), where Shift scrubs as
    /// usual and the value is shown to the box's own decimals alone.
    void setFineStep(double step);

    double value() const { return m_value; }
    double minimum() const { return m_min; }
    double maximum() const { return m_max; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    /// Clamps to the range; emits valueChanged only if the value actually changed.
    void setValue(double value);

signals:
    void valueChanged(double value);

    /// Gesture brackets, for undo systems: editingStarted fires when a scrub crosses the drag
    /// threshold or the type-in editor opens; editingFinished when that scrub releases or the
    /// editor closes (commit and cancel alike). Always a balanced pair around any user-driven
    /// change — snapshot state on started, commit one undo entry on finished. Programmatic
    /// setValue() emits neither. The pair stays balanced even when the scrub's mouse release
    /// never arrives (a popup, the menu bar or another window took the input mid-scrub — see
    /// abandonScrub), so a listener's bracket depth can never stick open.
    void editingStarted();
    void editingFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void beginEdit();  // click (no drag): swap in the inline line editor
    void commitEdit(); // Enter / focus-out: parse, apply, restore the painted look
    void cancelEdit(); // Esc: discard the typed text
    void abandonScrub(); // the release is lost: end the press/scrub as if it had arrived
    QString formattedValue() const;
    double fineStep() const;   // the Shift scrub's step in effect (0 = none; see setFineStep)
    int    shownDecimals() const; // the most digits a value is shown to: the box's own, or the fine step's

    double m_min = 0.0;
    double m_max = 1.0;
    double m_step = 0.01;
    int    m_decimals = 2;
    double m_value = 0.0;
    bool   m_hasOrigin = false; // see setOrigin: until set, the fill and the step grid start at m_min
    double m_origin = 0.0;
    double m_scrubSpan = 0.0;   // see setScrubSpan: 0 = a full-width sweep covers the whole range
    double m_fineStep = -1.0;   // see setFineStep: < 0 = a tenth of m_step, 0 = no fine scrub

    // Scrub state. The drag is anchored: value = pressValue + (dx / width) * range (or the scrub
    // span), so overshooting a bound and dragging back retraces symmetrically.
    bool    m_pressed = false;  // left button is down on us
    bool    m_dragging = false; // moved past the threshold: this press is a scrub, not a click
    QPointF m_pressPos;
    double  m_pressValue = 0.0;
    bool    m_fineScrub = false; // the scrub's last move had Shift held (a change re-anchors it)

    bool       m_hovered = false;
    bool       m_editing = false;
    QLineEdit* m_editor = nullptr; // created lazily on first edit

    // Painted-look properties (QSS-overridable); defaults match the app palette
    // (see _environment.qss's palette comment). The inline editor's stylesheet is built from
    // these too (beginEdit), so a re-theme covers both looks.
    QColor m_backgroundColor = QColor(0x2a, 0x2b, 0x2d);
    QColor m_fillColor = QColor(0x31, 0x4D, 0x7A); // the app's muted accent (menu-hover blue)
    QColor m_borderColor = QColor(0x4a, 0x4b, 0x4d);
    QColor m_hoverBorderColor = QColor(Constants::COLOR_ACCENT); // THE app accent — matches the HDRI dropdown's hover border
    QColor m_textColor = QColor(0xe0, 0xe0, 0xe0);
};

} // namespace pose

#endif // DRAGNUMBERBOX_H
