/**
 * @file vulkanwindow.cpp
 * @brief Implementation of the Vulkan-hosting QWindow. See vulkanwindow.h.
 */

#include "vulkanwindow.h"

#include "environmentsource.h"
#include "figureimportservice.h"
#include "librarypaths.h"
#include "modelimportservice.h"
#include "rendering/vulkancommon.h"
#include "rendering/vulkancontext.h"
#include "rendering/vulkanrenderer.h"

#include <QAction>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMenu>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <QtConcurrent>
#include <QtGui/qevent.h> // QPlatformSurfaceEvent lives here (no separate qpa header in binary Qt)

#include <glm/glm.hpp>

#include <cmath>
#include <memory>
#include <optional>

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
        m_renderer->setLightingSettings(m_lighting); // apply any dials set before first expose

        // Image-based lighting: bake a real HDR panorama over the renderer's built-in procedural studio
        // (the environment the user picked before first expose, else the default). The bake runs off the
        // UI thread; the first frames render the procedural studio (from the Scene ctor) until it lands.
        beginEnvironmentBake(m_environmentPath.isEmpty() ? defaultEnvironmentPath() : m_environmentPath);
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

QString VulkanWindow::defaultEnvironmentPath() const {
    // An explicit <appDir>/environment.hdr (or .exr) override wins; otherwise the stock/first
    // panorama from the user library's hdri/ tree via LibraryPaths::defaultHdri() — the same
    // recursive scan backing the Environment panel's menu, so a collection categorized into
    // subfolders resolves identically here. An empty return leaves the procedural studio
    // environment (beginEnvironmentBake no-ops on it).
    for (const char* name : {"environment.hdr", "environment.exr"}) {
        const QString override =
            QCoreApplication::applicationDirPath() + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(override)) {
            return override;
        }
    }
    return LibraryPaths::defaultHdri();
}

void VulkanWindow::beginEnvironmentBake(const QString& hdrPath) {
    if (!m_renderer || hdrPath.isEmpty() || !QFileInfo::exists(hdrPath)) {
        return; // no renderer yet, or nothing to load — the procedural default stays
    }
    // Decode + bake the HDRI off the UI thread: the CPU prefilter is hundreds of ms and would freeze the
    // app on every switch. The viewport keeps rendering the current environment until the result lands.
    // A per-request id discards a slower earlier bake whose selection a newer one has since superseded.
    const quint64 requestId = ++m_environmentRequestId;
    auto* watcher = new QFutureWatcher<std::shared_ptr<BakedEnvironment>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, requestId]() {
        const std::shared_ptr<BakedEnvironment> baked = watcher->result();
        watcher->deleteLater();
        // The GPU upload runs here on the GUI thread; apply only the newest request that still has a
        // live renderer and a successful bake.
        if (requestId == m_environmentRequestId && m_renderer && baked) {
            m_renderer->applyBakedEnvironment(*baked);
            requestUpdate();
        }
    });
    // The task captures only the path (by value); it touches no window/renderer state, so it stays safe
    // even if the window is torn down before it finishes (the watcher, a child of `this`, won't fire).
    watcher->setFuture(QtConcurrent::run([hdrPath]() -> std::shared_ptr<BakedEnvironment> {
        std::optional<EnvironmentImage> env = loadEnvironmentImage(hdrPath.toStdString());
        if (!env) {
            qWarning() << "[viewport] Failed to decode environment panorama:" << hdrPath;
            return nullptr;
        }
        return std::make_shared<BakedEnvironment>(bakeEnvironment(std::move(*env)));
    }));
}

void VulkanWindow::setEnvironmentFile(const QString& hdrPath) {
    m_environmentPath = hdrPath; // remembered so it survives a device loss / applies before first expose
    beginEnvironmentBake(hdrPath); // no-op until the renderer exists (init kicks off the first bake)
}

void VulkanWindow::setLightingSettings(const LightingSettings& settings) {
    m_lighting = settings; // remembered so it applies before first expose / after a device loss
    if (m_renderer) {
        m_renderer->setLightingSettings(settings);
        requestUpdate();
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

void VulkanWindow::resetView() {
    if (m_renderer) {
        m_renderer->camera().reset();
        requestUpdate(); // rendering is on demand — nothing redraws unless we ask
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
    // Event-driven rendering: a frame is drawn only when something requested one (input, imports,
    // pose edits, shade-mode/lighting changes, expose/resize — every state change already calls
    // requestUpdate()). The old continuous requestUpdate() here redrew at full swap rate even when
    // idle, keeping the GPU busy and (with FIFO/vsync present) throttling the whole GUI thread.
    // drawFrame() returns true only when the swapchain was just rebuilt (or the frame skipped
    // mid-rebuild) and one follow-up frame is needed to reflect the new size.
    if (m_renderer->drawFrame()) {
        requestUpdate();
    }
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
                UndoEntry entry;
                entry.kind = UndoEntry::Kind::Pose;
                entry.pose = m_preEditPose;
                m_undoStack.push_back(std::move(entry));
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
    // Fallback path: the Edit-menu actions carry the app-wide Ctrl+Z/Ctrl+Y shortcuts, but keep
    // handling the raw keys here too in case a platform delivers them to the native window
    // without the shortcut map consuming them first.
    if (m_renderer && event->modifiers().testFlag(Qt::ControlModifier)) {
        if (event->key() == Qt::Key_Z) {
            undo();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Y) {
            redo();
            event->accept();
            return;
        }
    }
    QWindow::keyPressEvent(event);
}

void VulkanWindow::registerLightingUndo(const LightingSettings& preEdit) {
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::Lighting;
    entry.lighting = preEdit;
    m_undoStack.push_back(std::move(entry));
    m_redoStack.clear(); // a fresh edit invalidates the redo branch, same as a pose edit
}

void VulkanWindow::undo() {
    if (!m_renderer || m_undoStack.empty()) {
        return;
    }
    UndoEntry entry = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    UndoEntry redo;
    redo.kind = entry.kind;
    if (entry.kind == UndoEntry::Kind::Pose) {
        redo.pose = m_renderer->capturePose(); // current pose becomes redoable
        m_renderer->applyPose(entry.pose);
        m_renderer->finalizePose(); // re-runs correctives, like any pose change
    } else {
        redo.lighting = m_lighting; // current dials become redoable
        setLightingSettings(entry.lighting);
        emit lightingRestored(entry.lighting); // the Environment panel syncs its widgets
    }
    m_redoStack.push_back(std::move(redo));
    requestUpdate();
}

void VulkanWindow::redo() {
    if (!m_renderer || m_redoStack.empty()) {
        return;
    }
    UndoEntry entry = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    UndoEntry undone;
    undone.kind = entry.kind;
    if (entry.kind == UndoEntry::Kind::Pose) {
        undone.pose = m_renderer->capturePose();
        m_renderer->applyPose(entry.pose);
        m_renderer->finalizePose();
    } else {
        undone.lighting = m_lighting;
        setLightingSettings(entry.lighting);
        emit lightingRestored(entry.lighting);
    }
    m_undoStack.push_back(std::move(undone));
    requestUpdate();
}

} // namespace pose
