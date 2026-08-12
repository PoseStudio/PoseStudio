/**
 * @file vulkanwindow.cpp
 * @brief Implementation of the Vulkan-hosting QWindow. See vulkanwindow.h.
 */

#include "vulkanwindow.h"

#include "figureimportservice.h"
#include "modelimportservice.h"
#include "rendering/vulkancommon.h"
#include "rendering/vulkancontext.h"
#include "rendering/vulkanrenderer.h"

#include <QAction>
#include <QDebug>
#include <QMenu>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <QtGui/qevent.h> // QPlatformSurfaceEvent lives here (no separate qpa header in binary Qt)

#include <glm/glm.hpp>

#include <cmath>

namespace pose {

namespace {
// Input tuning. Kept local for now; promote to Constants / a preference once the
// navigation-preferences panel grows a "viewport" section.
constexpr float kOrbitRadiansPerPixel = 0.008f;
constexpr float kPanPerPixel = 0.0015f;
constexpr float kDollyPerWheelStep = 0.12f; // per 120-unit wheel notch
} // namespace

VulkanWindow::VulkanWindow(QVulkanInstance* instance, uint32_t apiVersion, QString shaderDir,
                           QWindow* parent)
    : QWindow(parent), m_instance(instance), m_apiVersion(apiVersion),
      m_shaderDir(std::move(shaderDir)) {
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(m_instance);
}

VulkanWindow::~VulkanWindow() {
    releaseVulkan();
}

VkExtent2D VulkanWindow::pixelExtent() const {
    const qreal dpr = devicePixelRatio();
    return VkExtent2D{
        static_cast<uint32_t>(std::lround(width() * dpr)),
        static_cast<uint32_t>(std::lround(height() * dpr)),
    };
}

void VulkanWindow::initializeVulkan() {
    if (m_initialized || m_deviceFailed) {
        return;
    }
    try {
        VkSurfaceKHR surface = m_instance->surfaceForWindow(this);
        if (surface == VK_NULL_HANDLE) {
            throw VulkanError("QVulkanInstance::surfaceForWindow returned a null surface.");
        }
        m_context = std::make_unique<VulkanContext>(m_instance->vkInstance(), surface, m_apiVersion);
        m_renderer = std::make_unique<VulkanRenderer>(*m_context, pixelExtent(),
                                                      m_shaderDir.toStdString());
        m_renderer->setShadeMode(m_shadeMode); // apply a mode chosen before first expose
        m_initialized = true;

        // Drain any imports requested before the renderer existed (e.g. user hit Import while
        // the viewport was still hidden). A bad file only drops that one model, not the window.
        for (const QString& pending : m_pendingModels) {
            // No progress dialog here: we're inside the expose/init path, where a modal
            // dialog's processEvents() could re-enter exposeEvent()/initializeVulkan().
            if (ModelImportService::importInto(*m_renderer, pending, /*showProgress=*/false)) {
                requestUpdate();
            }
        }
        m_pendingModels.clear();
        for (const QString& pending : m_pendingFigures) {
            if (FigureImportService::importInto(*m_renderer, pending, /*showProgress=*/false)) {
                // The skeleton overlay stays hidden; a click on a joint still selects it (picking is
                // independent of the overlay) and brings up the rotate gizmo.
                requestUpdate();
            }
        }
        m_pendingFigures.clear();
        if (!m_pendingPose.isEmpty()) {
            m_renderer->loadPose(m_pendingPose.toStdString()); // onto the just-drained figure
            m_pendingPose.clear();
            requestUpdate();
        }
    } catch (const VulkanError& e) {
        // Leave the surface intact (Qt owns it) but mark the device as failed so we
        // don't retry every expose. The container just shows the clear-colour-less window.
        m_renderer.reset();
        m_context.reset();
        m_deviceFailed = true;
        qCritical() << "[Vulkan] Viewport initialisation failed:" << e.what();
    }
}

void VulkanWindow::releaseVulkan() {
    // Order matters: the renderer waits for the device to idle in its destructor, so it
    // must go before the context (which owns the device).
    m_renderer.reset();
    m_context.reset();
    m_initialized = false;
}

void VulkanWindow::importObj(const QString& path) {
    if (m_deviceFailed) {
        qWarning() << "[viewport] Ignoring OBJ import; Vulkan is unavailable:" << path;
        return;
    }
    if (!m_renderer) {
        m_pendingModels.push_back(path); // loaded once initializeVulkan() builds the renderer
        return;
    }
    // All the parse/decode/upload orchestration (and its progress UI) lives in the Qt-facing
    // ModelImportService, keeping both the importers and the renderer core Qt-free.
    if (ModelImportService::importInto(*m_renderer, path, /*showProgress=*/true)) {
        requestUpdate();
    }
}

void VulkanWindow::importFigure(const QString& path) {
    if (m_deviceFailed) {
        qWarning() << "[viewport] Ignoring figure import; Vulkan is unavailable:" << path;
        return;
    }
    if (!m_renderer) {
        m_pendingFigures.push_back(path); // loaded once initializeVulkan() builds the renderer
        return;
    }
    if (FigureImportService::importInto(*m_renderer, path, /*showProgress=*/true)) {
        // The skeleton overlay stays hidden; a click on a joint still selects it and shows the gizmo.
        requestUpdate();
    }
}

bool VulkanWindow::hasPosableFigure() const {
    return m_renderer && m_renderer->hasPosableFigure();
}

void VulkanWindow::setShadeMode(int mode) {
    m_shadeMode = mode; // remembered so a mode chosen before first expose still applies
    if (m_renderer) {
        m_renderer->setShadeMode(mode);
        requestUpdate();
    }
}

bool VulkanWindow::savePose(const QString& path) {
    return m_renderer && m_renderer->savePose(path.toStdString());
}

bool VulkanWindow::loadPose(const QString& path) {
    if (!m_renderer) {
        m_pendingPose = path; // applied after the queued figure is drained (open-with a .pose)
        return true;
    }
    if (!m_renderer->loadPose(path.toStdString())) {
        return false;
    }
    requestUpdate(); // repaint with the restored pose
    return true;
}

void VulkanWindow::exposeEvent(QExposeEvent*) {
    if (isExposed()) {
        initializeVulkan();
        if (m_renderer) {
            m_renderer->notifyResize(pixelExtent());
            requestUpdate();
        }
    }
}

void VulkanWindow::resizeEvent(QResizeEvent*) {
    if (m_renderer) {
        m_renderer->notifyResize(pixelExtent());
        requestUpdate();
    }
}

bool VulkanWindow::event(QEvent* e) {
    switch (e->type()) {
    case QEvent::UpdateRequest:
        renderFrame();
        return true;
    case QEvent::PlatformSurface:
        // The surface is about to disappear (window closing/reparenting). Tear the
        // Vulkan objects down now, while the surface they reference is still valid.
        if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            releaseVulkan();
        }
        break;
    default:
        break;
    }
    return QWindow::event(e);
}

void VulkanWindow::renderFrame() {
    if (!m_renderer || !isExposed()) {
        return;
    }
    m_renderer->drawFrame();
    // Continuous redraw for now. Once the viewport is event-driven, gate this on "the
    // scene/camera actually changed".
    requestUpdate();
}

void VulkanWindow::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->position();
    m_activeDragButtons |= event->button(); // a drag with this button started in the viewport

    // Left-press priority: (1) a gizmo ring of the selected joint -> axis-constrained rotate;
    // (2) a joint -> select it + free-rotate this drag; (3) empty space -> orbit the camera.
    if (event->button() == Qt::LeftButton && m_renderer) {
        const float x = static_cast<float>(event->position().x());
        const float y = static_cast<float>(event->position().y());
        const float w = static_cast<float>(width());
        const float h = static_cast<float>(height());
        m_gizmoAxis = m_renderer->hasSelectedBone() ? m_renderer->gizmoAxisAt(x, y, w, h) : -1;
        if (m_gizmoAxis >= 0) {
            m_posingBone = false;
        } else {
            m_posingBone = (m_renderer->selectBoneAt(x, y, w, h) >= 0);
        }
        if (m_gizmoAxis >= 0 || m_posingBone) {
            m_preEditPose = m_renderer->capturePose(); // snapshot for undo (committed on release)
        }
        requestUpdate(); // reflect the new selection highlight / gizmo
    }
}

void VulkanWindow::mouseReleaseEvent(QMouseEvent* event) {
    m_activeDragButtons &= ~event->button();
    if (event->button() == Qt::LeftButton) {
        if ((m_posingBone || m_gizmoAxis >= 0) && m_renderer) {
            // Pose edit settled: apply the pose correctives (deferred during the drag because they
            // re-upload geometry) and commit an undo entry if the pose actually changed.
            m_renderer->finalizePose();
            if (m_renderer->capturePose() != m_preEditPose) {
                m_undoStack.push_back(m_preEditPose);
                m_redoStack.clear();
            }
            requestUpdate();
        }
        m_posingBone = false;
        m_gizmoAxis = -1;
    }
    // Right-click (on release, the desktop convention) opens the object context menu. The right
    // button drives no camera motion, so there's nothing to disambiguate from a drag here.
    if (event->button() == Qt::RightButton) {
        showObjectContextMenu(event->position(), event->globalPosition().toPoint());
    }
}

void VulkanWindow::showObjectContextMenu(const QPointF& localPos, const QPoint& globalPos) {
    if (!m_renderer) {
        return;
    }
    const Ray ray = m_renderer->camera().screenPointToRay(
        static_cast<float>(localPos.x()), static_cast<float>(localPos.y()),
        static_cast<float>(width()), static_cast<float>(height()));
    const int picked = m_renderer->pickModel(ray);
    if (picked < 0) {
        return; // empty space — no menu (for now)
    }

    QMenu menu;
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));
    if (menu.exec(globalPos) == deleteAction) {
        m_renderer->deleteModel(static_cast<std::size_t>(picked));
        requestUpdate();
    }
}

void VulkanWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!m_renderer) {
        return;
    }
    const QPointF prev = m_lastMousePos;
    const QPointF delta = event->position() - m_lastMousePos;
    m_lastMousePos = event->position();

    // Only act on buttons that are BOTH held now AND were pressed inside this window. Using
    // event->buttons() alone would let a leaked move (e.g. as the modal Import dialog closes
    // over us with a button still logically down) snap the camera from a stale last-position.
    const Qt::MouseButtons active = m_activeDragButtons & event->buttons();

    Camera& camera = m_renderer->camera();
    if ((active & Qt::LeftButton) && m_gizmoAxis >= 0) {
        // Gizmo: rotate the selected joint about the grabbed ring's axis by the swept screen angle.
        m_renderer->rotateGizmo(m_gizmoAxis, static_cast<float>(prev.x()), static_cast<float>(prev.y()),
                                static_cast<float>(event->position().x()),
                                static_cast<float>(event->position().y()), static_cast<float>(width()),
                                static_cast<float>(height()));
    } else if ((active & Qt::LeftButton) && m_posingBone) {
        // Free posing: drag rotates the selected joint — horizontal about its Y axis, vertical about X.
        constexpr float kDegPerPixel = 0.4f;
        m_renderer->nudgeSelectedBone(glm::vec3(static_cast<float>(delta.y()) * kDegPerPixel,
                                                static_cast<float>(delta.x()) * kDegPerPixel, 0.0f));
    } else if (active & Qt::LeftButton) {
        // Drag right -> orbit right; drag up -> tilt up. Negated to feel like grabbing the scene.
        camera.orbit(-static_cast<float>(delta.x()) * kOrbitRadiansPerPixel,
                     -static_cast<float>(delta.y()) * kOrbitRadiansPerPixel);
    } else if (active & Qt::MiddleButton) {
        camera.pan(static_cast<float>(delta.x()) * kPanPerPixel,
                   static_cast<float>(delta.y()) * kPanPerPixel);
    }
    requestUpdate();
}

void VulkanWindow::wheelEvent(QWheelEvent* event) {
    if (!m_renderer) {
        return;
    }
    const float steps = static_cast<float>(event->angleDelta().y()) / 120.0f;
    m_renderer->camera().dolly(steps * kDollyPerWheelStep);
    requestUpdate();
}

void VulkanWindow::keyPressEvent(QKeyEvent* event) {
    if (m_renderer && event->modifiers().testFlag(Qt::ControlModifier)) {
        if (event->key() == Qt::Key_Z) {
            undoPose();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Y) {
            redoPose();
            event->accept();
            return;
        }
    }
    QWindow::keyPressEvent(event);
}

void VulkanWindow::undoPose() {
    if (!m_renderer || m_undoStack.empty()) {
        return;
    }
    m_redoStack.push_back(m_renderer->capturePose()); // current pose becomes redoable
    m_renderer->applyPose(m_undoStack.back());
    m_undoStack.pop_back();
    m_renderer->finalizePose();
    requestUpdate();
}

void VulkanWindow::redoPose() {
    if (!m_renderer || m_redoStack.empty()) {
        return;
    }
    m_undoStack.push_back(m_renderer->capturePose());
    m_renderer->applyPose(m_redoStack.back());
    m_redoStack.pop_back();
    m_renderer->finalizePose();
    requestUpdate();
}

} // namespace pose
