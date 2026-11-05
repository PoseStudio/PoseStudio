/**
 * @file transformpanel.h
 * @brief The "Transform" properties tab: the selected joint's rotation as three dials.
 *
 * A self-contained docker panel (its own module, like EnvironmentPanel): with a joint selected on
 * the posable figure it shows that joint's name and three DragNumberBox rows —
 *
 *   Bend       the joint's natural direction (a knee's fold, a spine joint's forward bend)
 *   Side-Side  its secondary direction
 *   Twist      the turn about its own length
 *
 * — each on the same scale: 0 is the rest pose, 100 is the joint's limit in that direction, and a
 * negative value bends it the other way in the same proportion until the other limit stops it (a
 * knee reads -7..100). Which Euler channel each dial turns, its scale and its range are the
 * armature's business (Armature::jointDial); the panel only mirrors what the viewport reports
 * (ViewportWidget::jointTransform) and sends the values the user sets. A row whose motion the
 * joint does not have (a locked channel — an elbow has no side bend) is disabled.
 *
 * Live both ways: a dial scrub poses the figure as it moves (an FK rotation through the joint
 * limits and the collision stop — the row shows where the pose actually rests), and the rows
 * follow the figure whenever the pose or the selection changes by any other means — an IK drag,
 * the viewport's B / S / T mouse mode (which turns these same dials), undo, a pose load
 * (ViewportWidget::jointTransformChanged).
 *
 * Undo: every completed gesture (a scrub, a type-in, a row's restore button, Reset All) is
 * ONE entry on the viewport's unified undo stack, bracketed begin/endJointTransformEdit.
 */

#ifndef TRANSFORMPANEL_H
#define TRANSFORMPANEL_H

#include <QWidget>

#include <array>

class QLabel;
class QPushButton;
class QToolButton;

namespace pose {

class DragNumberBox;
class ViewportWidget;

class TransformPanel : public QWidget {
    Q_OBJECT

public:
    /// @param viewport The viewport whose selected joint this panel shows and poses. Not owned.
    explicit TransformPanel(ViewportWidget* viewport, QWidget* parent = nullptr);

private:
    void buildUi();
    /// Mirrors the viewport's selected-joint state into the heading and the rows (signals
    /// blocked: nothing here re-poses the figure).
    void refresh();
    /// Sets every dial the joint has back to 0 — the joint's rest pose, its limb's twist included
    /// — as one undoable edit.
    void resetRotation();

    /// One dial row: its label (dimmed with the row), the scrub field, the restore button.
    struct Row {
        QLabel*        label = nullptr;
        DragNumberBox* box = nullptr;
        QToolButton*   restore = nullptr;
    };

    ViewportWidget*    m_viewport = nullptr;
    QLabel*            m_jointName = nullptr; // the selected joint's name, or "No joint selected"
    QLabel*            m_hint = nullptr;      // how to get a joint selected (shown while none is)
    std::array<Row, 3> m_rows;                // indexed by JointDialKind
    std::array<Row, 7> m_figureRows;          // the Figure group: x, y, z (cm), turn, tip, tilt (deg), scale (%, a static model's)
    QPushButton*       m_reset = nullptr;
};

} // namespace pose

#endif // TRANSFORMPANEL_H
