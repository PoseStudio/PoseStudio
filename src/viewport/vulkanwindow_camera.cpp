// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vulkanwindow_camera.cpp
 * @brief VulkanWindow's camera: the named views, the Blender-convention view hotkeys, and the
 *        world→screen projection.
 *
 * One of the translation units of VulkanWindow — see vulkanwindow.h for the map. The
 * camera itself (scene/camera.h) is pure GLM; this file is the bookkeeping around it — which
 * NAMED view the camera is in (so the View picker, the View menu and the keys agree), and the
 * in-viewport fallback for the app-wide view shortcuts.
 */

#include "vulkanwindow.h"

#include "rendering/vulkanrenderer.h"

#include "constants.h"
#include "preferencesmanager.h"

#include <QKeyEvent>

namespace pose {

void VulkanWindow::noteView(ViewPreset view) {
    if (m_viewPreset != view) {
        m_viewPreset = view;
        emit viewPresetChanged(view);
    }
}

void VulkanWindow::noteCameraChange() {
    if (PreferencesManager::instance().getValue(Constants::PREF_CAMERA_CHANGES_MARK_DIRTY,
                                              false).toBool()) {
        markProjectDirty(); // the framing is part of the document
    }
}

void VulkanWindow::resetView() {
    withRenderer([this](VulkanRenderer& r) {
        r.camera().reset();
        noteView(ViewPreset::Home);
    });
    noteCameraChange();
}

void VulkanWindow::setAxisView(AxisView view) {
    withRenderer([this, view](VulkanRenderer& r) {
        r.camera().setAxisView(view);
        switch (view) {
        case AxisView::Front:  noteView(ViewPreset::Front);  break;
        case AxisView::Back:   noteView(ViewPreset::Back);   break;
        case AxisView::Right:  noteView(ViewPreset::Right);  break;
        case AxisView::Left:   noteView(ViewPreset::Left);   break;
        case AxisView::Top:    noteView(ViewPreset::Top);    break;
        case AxisView::Bottom: noteView(ViewPreset::Bottom); break;
        }
    });
    noteCameraChange();
}

void VulkanWindow::flipView() {
    withRenderer([this](VulkanRenderer& r) {
        r.camera().flip();
        // The opposite side of a named side view is its counterpart; a flipped top view is still
        // the top (turned), anything else is no longer a named view.
        switch (m_viewPreset) {
        case ViewPreset::Front: noteView(ViewPreset::Back);  break;
        case ViewPreset::Back:  noteView(ViewPreset::Front); break;
        case ViewPreset::Left:  noteView(ViewPreset::Right); break;
        case ViewPreset::Right: noteView(ViewPreset::Left);  break;
        case ViewPreset::Top:
        case ViewPreset::Bottom: break;
        default: noteView(ViewPreset::Free); break;
        }
    });
    noteCameraChange();
}

void VulkanWindow::frameSelected() {
    if (m_renderer && m_renderer->frameSelected()) {
        requestUpdate();
        noteCameraChange();
    }
}

bool VulkanWindow::handleViewHotkey(const QKeyEvent* event) {
    // Camera views, Blender's numpad convention — on the numpad AND the number row (Blender's
    // "emulate numpad"): 1/3/7 = front/right/top, Ctrl = the opposite side, 9 = flip 180°,
    // 5 = the Home view, "." = frame selected. The View menu's QAction shortcuts carry these
    // APP-WIDE; this is the in-viewport fallback (like Ctrl+Z) for a platform that hands the
    // native window the key before the shortcut map sees it.
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool plain = mods == Qt::NoModifier;
    const bool ctrl = mods == Qt::ControlModifier;
    if (!plain && !ctrl) {
        return false;
    }
    // With NumLock OFF the pad sends its navigation keys instead of digits: fold those back
    // onto the digits so the pad works either way (the keypad modifier tells them apart from
    // the real End/Home/PageUp/PageDown/Delete keys).
    int key = event->key();
    if (event->modifiers().testFlag(Qt::KeypadModifier)) {
        switch (key) {
        case Qt::Key_End:      key = Qt::Key_1; break;
        case Qt::Key_PageDown: key = Qt::Key_3; break;
        case Qt::Key_Home:     key = Qt::Key_7; break;
        case Qt::Key_PageUp:   key = Qt::Key_9; break;
        case Qt::Key_Clear:    key = Qt::Key_5; break; // numpad 5 with NumLock off
        case Qt::Key_Delete:                    // the pad's "." with NumLock off
        case Qt::Key_Comma:    key = Qt::Key_Period; break; // decimal-comma layouts
        default: break;
        }
    }
    switch (key) {
    case Qt::Key_1: setAxisView(ctrl ? AxisView::Back : AxisView::Front); return true;
    case Qt::Key_3: setAxisView(ctrl ? AxisView::Left : AxisView::Right); return true;
    case Qt::Key_7: setAxisView(ctrl ? AxisView::Bottom : AxisView::Top); return true;
    case Qt::Key_9:
        if (plain) { flipView(); return true; }
        return false;
    case Qt::Key_5:
        if (plain) { resetView(); return true; } // = the Home button
        return false;
    case Qt::Key_Period:
        if (plain) { frameSelected(); return true; }
        return false;
    default:
        return false;
    }
}

bool VulkanWindow::projectToScreen(const glm::vec3& world, QPointF& out) const {
    const glm::vec4 clip = m_renderer->camera().viewProjection() * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-4f) {
        return false;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    out = QPointF((ndc.x * 0.5f + 0.5f) * width(), (ndc.y * 0.5f + 0.5f) * height());
    return true;
}

} // namespace pose
