/**
 * @file viewportwidget.cpp
 * @brief Implementation of the viewport widget facade. See viewportwidget.h.
 */

#include "viewportwidget.h"

#include "vulkanwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMoveEvent>
#include <QPoint>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSize>
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

    createShaderOverlay(); // the floating shader-mode dropdown in the top-right corner
}

void ViewportWidget::importObj(const QString& path) {
    if (m_window) {
        m_window->importObj(path);
    }
}

void ViewportWidget::importFigure(const QString& path) {
    if (m_window) {
        m_window->importFigure(path);
    }
}

bool ViewportWidget::hasPosableFigure() const {
    return m_window && m_window->hasPosableFigure();
}

bool ViewportWidget::savePose(const QString& path) {
    return m_window && m_window->savePose(path);
}

bool ViewportWidget::loadPose(const QString& path) {
    return m_window && m_window->loadPose(path);
}

void ViewportWidget::setShadeMode(int mode) {
    if (m_window) {
        m_window->setShadeMode(mode);
    }
}

void ViewportWidget::resetView() {
    if (m_window) {
        m_window->resetView();
    }
}

void ViewportWidget::setEnvironment(const QString& hdrPath) {
    if (m_window) {
        m_window->setEnvironmentFile(hdrPath);
    }
}

void ViewportWidget::setLightingSettings(const LightingSettings& settings) {
    if (m_window) {
        m_window->setLightingSettings(settings);
    }
}

QStringList ViewportWidget::shaderModeNames() {
    // Order IS the shade-mode index the shader reads (cam.params.x); keep in sync with mesh.frag.
    return {
        QStringLiteral("Rendered"),               // 0
        QStringLiteral("PBR (Physically-Based)"), // 1
        QStringLiteral("Matcap: Studio"),         // 2
        QStringLiteral("Matcap: Skin"),           // 3
        QStringLiteral("Matcap: Metal"),          // 4
        QStringLiteral("Toon / Cel"),             // 5
        QStringLiteral("Clay"),                   // 6
        QStringLiteral("Lighting Only"),          // 7
        QStringLiteral("Flat Shaded"),            // 8
        QStringLiteral("Normals"),                // 9
        QStringLiteral("Albedo (Unlit)"),         // 10
        QStringLiteral("UV Checker"),             // 11
    };
}

void ViewportWidget::createShaderOverlay() {
    // A *top-level* frameless window owned by this widget — a child widget would be composited behind
    // the native viewport (see the SplashOverlay note). WA_ShowWithoutActivating so revealing it
    // doesn't steal focus from the app; the combo still takes clicks normally.
    m_overlay = new QWidget(this, Qt::Window | Qt::FramelessWindowHint);
    m_overlay->setObjectName(QStringLiteral("ViewportShaderOverlay"));
    m_overlay->setAttribute(Qt::WA_TranslucentBackground, true);
    m_overlay->setAttribute(Qt::WA_ShowWithoutActivating, true);

    auto* lay = new QHBoxLayout(m_overlay);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    // The selector is a push-button that opens a *real QMenu*, rather than a QComboBox. This is the one
    // way to get the picker's dropdown to look and behave EXACTLY like the File/Edit/Help menus — colours,
    // hover, spacing, and top-down drop — because it *is* a QMenu and so inherits the global QMenu QSS
    // (see _menumanager.qss). A styled QComboBox popup renders its own item states inconsistently.
    m_shaderButton = new QPushButton(m_overlay);
    m_shaderButton->setObjectName(QStringLiteral("ShaderModeButton"));
    m_shaderButton->setToolTip(tr("Viewport shading mode"));
    // No cursor override on the overlay controls: Windows apps keep the standard arrow over buttons
    // (the hand cursor reads as a hyperlink there), so these follow the platform convention.
    // The button (the closed selector) matches the menu-bar surface + hover; the QMenu it opens is themed
    // globally. text-align:left so the mode name sits left of the drop arrow, like a combo field.
    m_shaderButton->setStyleSheet(QStringLiteral(
        "#ShaderModeButton {"
        "  background-color: #252627;"
        "  color: #e8e8ea;"
        "  border: 1px solid #555555;"
        "  border-radius: 4px;"
        "  padding: 5px 10px;"
        "  min-width: 168px;"
        "  font-size: 12px;"
        "  text-align: left;"
        "}"
        "#ShaderModeButton:hover { background-color: #314D7A; color: #ffffff; }"
        "#ShaderModeButton::menu-indicator { subcontrol-position: right center;"
        "  subcontrol-origin: padding; right: 8px; }"));

    auto* menu = new QMenu(m_shaderButton);
    auto* group = new QActionGroup(menu);
    group->setExclusive(true);
    const QStringList names = shaderModeNames();
    // QPushButton with text-align:left ignores padding-left, so the field label gets a small leading
    // indent from a couple of spaces to keep the mode name off the left border (the menu items, which
    // honour padding normally, stay unindented).
    const auto fieldLabel = [](const QString& name) { return QStringLiteral("  ") + name; };
    // PBR/IBL is the default shade mode (index 1) — see mesh.frag and Scene's m_shadeMode.
    constexpr int kDefaultShadeMode = 1;
    for (int i = 0; i < names.size(); ++i) {
        QAction* action = menu->addAction(names.at(i));
        action->setCheckable(true);
        action->setChecked(i == kDefaultShadeMode);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, i, name = names.at(i), fieldLabel] {
            setShadeMode(i);
            m_shaderButton->setText(fieldLabel(name));
        });
    }
    m_shaderButton->setMenu(menu); // QPushButton drops the menu straight down from the button
    m_shaderButton->setText(fieldLabel(names.at(kDefaultShadeMode)));
    lay->addWidget(m_shaderButton);

    // "Home": snap the camera back to the default perspective framing. Same surface/hover as the
    // shader field so the two read as one control strip; squared off to the field's own height.
    m_homeButton = new QPushButton(m_overlay);
    m_homeButton->setObjectName(QStringLiteral("ViewportHomeButton"));
    m_homeButton->setToolTip(tr("Reset to the default view"));
    m_homeButton->setIcon(QIcon(QStringLiteral(":/resources/icons/home.png")));
    m_homeButton->setIconSize(QSize(16, 16));
    m_homeButton->setStyleSheet(QStringLiteral(
        "#ViewportHomeButton {"
        "  background-color: #252627;"
        "  border: 1px solid #555555;"
        "  border-radius: 4px;"
        "}"
        "#ViewportHomeButton:hover { background-color: #314D7A; }"));
    // Match the field's height exactly (rather than guessing a size), so the strip aligns whatever
    // the font metrics work out to.
    const int fieldHeight = m_shaderButton->sizeHint().height();
    m_homeButton->setFixedSize(fieldHeight, fieldHeight);
    connect(m_homeButton, &QPushButton::clicked, this, &ViewportWidget::resetView);
    lay->addWidget(m_homeButton);

    m_overlay->adjustSize();
}

void ViewportWidget::syncOverlayPosition() {
    if (!m_overlay || !m_container || !m_container->isVisible()) {
        return;
    }
    m_overlay->adjustSize();
    constexpr int margin = 12;
    const QPoint topRight = m_container->mapToGlobal(QPoint(m_container->width(), 0));
    m_overlay->move(topRight.x() - m_overlay->width() - margin, topRight.y() + margin);
}

void ViewportWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!m_overlay) {
        return;
    }
    // Follow the top-level window's moves/resizes (a child moveEvent doesn't fire when the whole app
    // window is dragged). Re-target the filter if we've been reparented into a different window.
    if (QWidget* w = window(); w && w != m_filteredWindow) {
        if (m_filteredWindow) {
            m_filteredWindow->removeEventFilter(this);
        }
        w->installEventFilter(this);
        m_filteredWindow = w;
    }
    m_overlay->show();
    m_overlay->raise();
    syncOverlayPosition();
}

void ViewportWidget::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_overlay) {
        m_overlay->hide();
    }
}

void ViewportWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncOverlayPosition();
}

void ViewportWidget::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    syncOverlayPosition();
}

bool ViewportWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_filteredWindow) {
        const QEvent::Type t = event->type();
        if (t == QEvent::Move || t == QEvent::Resize || t == QEvent::WindowStateChange) {
            syncOverlayPosition();
        }
    }
    return QWidget::eventFilter(watched, event);
}

ViewportWidget::~ViewportWidget() {
    // The floating overlay is an owned top-level window with no Vulkan resources; drop it first so it
    // can't briefly outlive the viewport it tracks. (Qt would also delete it as a child, but explicit
    // is clearer next to the ordered Vulkan teardown below.)
    delete m_overlay;
    m_overlay = nullptr;
    m_shaderButton = nullptr;
    m_homeButton = nullptr;

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
