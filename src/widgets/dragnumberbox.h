/**
 * @file dragnumberbox.h
 * @brief The app-standard numeric input: a "drag number box" (scrub field).
 *
 * One compact control that replaces the slider + spin box pair for bounded numeric
 * parameters (the DCC-app convention — Blender/Substance-style):
 *  - Background: a left-to-right fill proportional to (value - min) / (max - min),
 *    so the box doubles as a progress-bar readout of where the value sits in range.
 *  - Foreground: the current value, centered.
 *  - Press + horizontal drag scrubs the value (a full-width sweep covers the whole
 *    range). The cursor shows a left-right arrow whenever it hovers the box, so the
 *    scrubbability is discoverable before pressing.
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

#include <QColor>
#include <QWidget>

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

    void setRange(double min, double max);
    /// The value delta one "notch" of precision represents. Drag positions snap to it.
    void setSingleStep(double step);
    /// Fractional digits shown in the label and the type-in editor.
    void setDecimals(int decimals);

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
    /// setValue() emits neither.
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
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void beginEdit();  // click (no drag): swap in the inline line editor
    void commitEdit(); // Enter / focus-out: parse, apply, restore the painted look
    void cancelEdit(); // Esc: discard the typed text
    QString formattedValue() const;

    double m_min = 0.0;
    double m_max = 1.0;
    double m_step = 0.01;
    int    m_decimals = 2;
    double m_value = 0.0;

    // Scrub state. The drag is anchored: value = pressValue + (dx / width) * range, so
    // overshooting a bound and dragging back retraces symmetrically.
    bool    m_pressed = false;  // left button is down on us
    bool    m_dragging = false; // moved past the threshold: this press is a scrub, not a click
    QPointF m_pressPos;
    double  m_pressValue = 0.0;

    bool       m_hovered = false;
    bool       m_editing = false;
    QLineEdit* m_editor = nullptr; // created lazily on first edit

    // Painted-look properties (QSS-overridable); defaults match the app palette
    // (see _environment.qss's palette comment).
    QColor m_backgroundColor = QColor(0x2a, 0x2b, 0x2d);
    QColor m_fillColor = QColor(0x31, 0x4D, 0x7A); // the app's muted accent (menu-hover blue)
    QColor m_borderColor = QColor(0x4a, 0x4b, 0x4d);
    QColor m_hoverBorderColor = QColor(0x5b, 0x87, 0xcc); // the app's bright accent — matches the HDRI dropdown's hover border
    QColor m_textColor = QColor(0xe0, 0xe0, 0xe0);
};

} // namespace pose

#endif // DRAGNUMBERBOX_H
