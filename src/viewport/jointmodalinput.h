// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file jointmodalinput.h
 * @brief The joint mouse mode's input, wherever in the application it lands.
 *
 * The mode (B / S / T: VulkanWindow::toggleJointModal) is MODAL: while it is on, the mouse turns a
 * joint and a click or a key ends it. The viewport sees only the input that arrives at its own
 * native window — so begun with the pointer over a side panel (the user had just scrubbed a dial
 * there), the mode was on and nothing turned until the pointer came back over the viewport, a
 * click on the panel went to the panel, and Esc went to whichever widget had the focus.
 *
 * This is an application-wide event filter that is installed only while the mode is on. It reads
 * the events as they arrive at a WINDOW (the main window, the floating strip: before any widget
 * is handed its copy) and gives the mode's share to the viewport: every mouse move turns the
 * joint, a left click or Enter keeps the result, a right click or Esc puts the joint back — and
 * the click is the mode's alone: it never reaches the widget under it. The viewport's own window
 * is left to its own handlers. While the mode is on the pointer is a four-way arrow everywhere
 * in the application (an override cursor).
 */

#ifndef JOINTMODALINPUT_H
#define JOINTMODALINPUT_H

#include <QObject>

namespace pose {

class VulkanWindow;

class JointModalInput : public QObject {
public:
    JointModalInput(VulkanWindow* window, QObject* parent);
    ~JointModalInput() override;

    /// The mode turned on (true) or ended (false): VulkanWindow::jointModalChanged. The filter
    /// stays installed past the end until the release of a click it consumed has gone by.
    void setActive(bool on);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void install();
    void uninstall();

    VulkanWindow*    m_window = nullptr;
    bool             m_active = false;
    bool             m_installed = false;
    Qt::MouseButtons m_swallow = Qt::NoButton; // presses the mode consumed: their releases are its too
};

} // namespace pose

#endif // JOINTMODALINPUT_H
