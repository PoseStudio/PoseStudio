/**
 * @file jointmodebadge.h
 * @brief The viewport strip's "the mouse is turning a joint" badge.
 *
 * Shown under the strip's buttons for as long as a joint's mouse mode is on (B, S or T with a
 * joint selected: VulkanWindow::beginJointModal) — the mouse's meaning is MODAL then (moving it
 * bends, tilts or twists the joint; a click keeps the result, Esc or a right-click drops it), so
 * the mode must be visible. A rotation-arrow glyph and the dial's name as the Transform tab
 * labels it, in the accent color, on the same surface as the strip's buttons. Painted, not an
 * icon file.
 */

#ifndef JOINTMODEBADGE_H
#define JOINTMODEBADGE_H

#include <QWidget>

class QPaintEvent;

namespace pose {

/**
 * @class JointModeBadge
 * @brief A fixed-size painted badge naming the dial the mouse currently turns.
 */
class JointModeBadge : public QWidget {
    Q_OBJECT
public:
    /// @p height matches the strip's field height so the badge rows align with the buttons.
    explicit JointModeBadge(QWidget* parent, int height);

    /// @p kind 0/1/2 = Bend / Side-Side / Twist (a JointDialKind); anything else paints the
    /// empty surface.
    void setMode(int kind);

    /// The dial's name as the Transform tab labels it ("" for anything but 0/1/2).
    static QString modeName(int kind);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int m_kind = -1;
};

} // namespace pose

#endif // JOINTMODEBADGE_H
