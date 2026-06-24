/**
 * @file vulkanwindow.cpp
 * @brief Implementation of the Vulkan-hosting QWindow. See vulkanwindow.h.
 */

#include "vulkanwindow.h"

#include "rendering/vulkancommon.h"
#include "rendering/vulkancontext.h"
#include "rendering/vulkanrenderer.h"
#include "scene/objloader.h"

#include <QDebug>
#include <QImage>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <QtGui/qevent.h> // QPlatformSurfaceEvent lives here (no separate qpa header in binary Qt)

#include <cmath>
#include <exception>

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
        m_initialized = true;

        // Drain any imports requested before the renderer existed (e.g. user hit Import while
        // the viewport was still hidden). A bad file only drops that one model, not the window.
        for (const QString& pending : m_pendingModels) {
            loadAndAddModel(pending);
        }
        m_pendingModels.clear();
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
    loadAndAddModel(path);
}

void VulkanWindow::loadAndAddModel(const QString& path) {
    try {
        ModelData data = loadObj(path.toStdString()); // geometry + resolved texture paths
        // Decode each diffuse texture here (Qt layer) so the renderer stays codec-free.
        for (MeshData& mesh : data.meshes) {
            if (mesh.diffuseTexturePath.empty()) {
                continue;
            }
            QImage image(QString::fromStdString(mesh.diffuseTexturePath));
            if (image.isNull()) {
                qWarning() << "[viewport] Texture not found/decodable:"
                           << QString::fromStdString(mesh.diffuseTexturePath)
                           << "- using base color instead";
                continue; // mesh falls back to baseColor via the white fallback texture
            }
            image = image.convertToFormat(QImage::Format_RGBA8888);
            mesh.diffuseWidth = static_cast<uint32_t>(image.width());
            mesh.diffuseHeight = static_cast<uint32_t>(image.height());
            const uchar* bits = image.constBits();
            mesh.diffusePixels.assign(bits, bits + image.sizeInBytes());
        }
        m_renderer->addModel(data);
        requestUpdate();
    } catch (const std::exception& e) {
        qWarning() << "[viewport] OBJ import failed:" << path << "-" << e.what();
    }
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
}

void VulkanWindow::mouseReleaseEvent(QMouseEvent* event) {
    m_activeDragButtons &= ~event->button();
}

void VulkanWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!m_renderer) {
        return;
    }
    const QPointF delta = event->position() - m_lastMousePos;
    m_lastMousePos = event->position();

    // Only act on buttons that are BOTH held now AND were pressed inside this window. Using
    // event->buttons() alone would let a leaked move (e.g. as the modal Import dialog closes
    // over us with a button still logically down) snap the camera from a stale last-position.
    const Qt::MouseButtons active = m_activeDragButtons & event->buttons();

    Camera& camera = m_renderer->camera();
    if (active & Qt::LeftButton) {
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

} // namespace pose
