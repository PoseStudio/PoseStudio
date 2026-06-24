/**
 * @file viewportwidget.cpp
 * @brief Implementation of the viewport widget facade. See viewportwidget.h.
 */

#include "viewportwidget.h"

#include "vulkanwindow.h"

#include <QCoreApplication>
#include <QDebug>
#include <QLabel>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QVulkanInstance>

#include <vulkan/vulkan.h>

namespace pose {

namespace {
// Single source of truth for the Vulkan version the app targets. 1.1 is supported by
// effectively every current driver and is enough for VMA's dedicated-allocation path.
// Bump here (and nowhere else) when the renderer starts relying on newer core features.
constexpr uint32_t kVulkanApiVersion = VK_API_VERSION_1_1;
} // namespace

ViewportWidget::ViewportWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_instance = std::make_unique<QVulkanInstance>();
    m_instance->setApiVersion(QVersionNumber(1, 1));
#ifndef NDEBUG
    // Standard validation layer in debug builds; QVulkanInstance routes its messages to
    // qDebug() automatically. Released builds skip it for performance.
    m_instance->setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
#endif

    if (!m_instance->create()) {
        auto* message = new QLabel(
            tr("3D viewport unavailable.\n\n"
               "Could not create a Vulkan instance — check that your GPU supports Vulkan "
               "and that your graphics drivers are up to date."),
            this);
        message->setAlignment(Qt::AlignCenter);
        message->setWordWrap(true);
        layout->addWidget(message);
        qCritical() << "[Vulkan] QVulkanInstance::create() failed, error code"
                    << m_instance->errorCode();
        m_instance.reset();
        return;
    }

    // Compiled shaders are mirrored next to the executable by the build (see CMakeLists).
    const QString shaderDir = QCoreApplication::applicationDirPath() + QStringLiteral("/shaders");

    m_window = new VulkanWindow(m_instance.get(), kVulkanApiVersion, shaderDir);
    m_container = QWidget::createWindowContainer(m_window, this);
    m_container->setFocusPolicy(Qt::StrongFocus); // so the viewport can receive wheel/keys
    layout->addWidget(m_container);
}

void ViewportWidget::importObj(const QString& path) {
    if (m_window) {
        m_window->importObj(path);
    }
}

ViewportWidget::~ViewportWidget() {
    // The QVulkanInstance (m_instance) owns the VkInstance, and the VulkanWindow's
    // device/renderer were created from it. Qt would otherwise destroy the window via the
    // base QWidget destructor — i.e. AFTER m_instance is gone — leaving the renderer to
    // call vkDestroyDevice on a dead instance. Destroy the container (hence the window,
    // hence all Vulkan objects) here, explicitly, while m_instance is still alive.
    delete m_container;
    m_container = nullptr;
    m_window = nullptr; // was owned by m_container; now dangling — clear it
}

} // namespace pose
