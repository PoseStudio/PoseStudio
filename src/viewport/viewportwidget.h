/**
 * @file viewportwidget.h
 * @brief The 3D viewport as a plain QWidget — the ONLY thing the rest of the app touches.
 *
 * Owns the application's QVulkanInstance and embeds a VulkanWindow via
 * QWidget::createWindowContainer(), so callers (e.g. main.cpp) can drop it into a
 * layout like any other widget and stay completely unaware of Vulkan. If the Vulkan
 * instance can't be created (no driver / no GPU), it degrades to an inline message
 * rather than taking the whole app down.
 */

#ifndef VIEWPORTWIDGET_H
#define VIEWPORTWIDGET_H

#include <QWidget>

#include <memory>

class QVulkanInstance;

namespace pose {

class VulkanWindow;

/**
 * @class ViewportWidget
 * @brief Self-contained 3D viewport widget backed by Vulkan.
 */
class ViewportWidget : public QWidget {
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget* parent = nullptr);
    ~ViewportWidget() override; // out-of-line: completes unique_ptr<QVulkanInstance>

    /// Imports an OBJ file into the 3D scene. No-op if the viewport degraded to the
    /// Vulkan-unavailable message (no window to load into).
    void importObj(const QString& path);

private:
    std::unique_ptr<QVulkanInstance> m_instance;          // owns the VkInstance
    VulkanWindow*                    m_window = nullptr;    // owned by m_container
    QWidget*                         m_container = nullptr; // the createWindowContainer wrapper
};

} // namespace pose

#endif // VIEWPORTWIDGET_H
