/**
 * @file viewportwidget.cpp
 * @brief Implementation of the viewport widget facade. See viewportwidget.h.
 */

#include "viewportwidget.h"

#include "jointmodalinput.h"
#include "openxrsystem.h" // the headset's Vulkan requirements, negotiated before the instance (vr/)
#include "scene/jointtransform.h"
#include "scene/materialparams.h"
#include "scene/shademode.h"
#include "viewportstrip.h"
#include "vrsettings.h"
#include "vulkanwindow.h"

#include <QAction>
#include <QApplication>
#include <QByteArrayList>
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QTextEdit>

#include "dragnumberbox.h"
#include <QMouseEvent>
#include <QWindow>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QVulkanInstance>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <string>

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
    uint32_t apiVersion = kVulkanApiVersion;

    // VR (opt-in, Preferences → VR): the headset runtime dictates instance extensions, a device
    // and an API version range BEFORE the instance exists, so it is consulted here, first. Every
    // failure is a normal outcome reported to the Preferences page; the viewport is the same
    // without it. Off by default because connecting to a runtime at every launch would start
    // SteamVR for people who never wear a headset.
    if (VrSettings::isEnabled()) {
        std::string reason;
        m_xr = OpenXrSystem::create(reason);
        if (!m_xr) {
            VrSettings::setStatus(QString::fromStdString(reason));
            qInfo() << "[vr] unavailable:" << QString::fromStdString(reason);
        } else if (m_xr->maxVulkanApiVersion() < kVulkanApiVersion) {
            VrSettings::setStatus(tr("The OpenXR runtime (%1) supports only Vulkan %2.%3; this viewport needs 1.1.")
                                      .arg(QString::fromStdString(m_xr->runtimeName()))
                                      .arg(VK_API_VERSION_MAJOR(m_xr->maxVulkanApiVersion()))
                                      .arg(VK_API_VERSION_MINOR(m_xr->maxVulkanApiVersion())));
            m_xr.reset();
        } else {
            apiVersion = std::max(kVulkanApiVersion, m_xr->minVulkanApiVersion());
            QByteArrayList extensions;
            for (const std::string& name : m_xr->vulkanInstanceExtensions()) {
                extensions << QByteArray::fromStdString(name);
            }
            m_instance->setExtensions(extensions); // Qt adds its own surface extensions on top
            VrSettings::setStatus(tr("Headset found: %1 (runtime: %2) — waiting for the viewport.")
                                      .arg(QString::fromStdString(m_xr->systemName()),
                                           QString::fromStdString(m_xr->runtimeName())));
        }
    } else {
        VrSettings::setStatus(tr("VR support is off. Turn it on and restart PoseStudio to use VR Mode."));
    }
    VrSettings::setReady(false); // the window reports readiness once the device exists

    m_instance->setApiVersion(QVersionNumber(static_cast<int>(VK_API_VERSION_MAJOR(apiVersion)),
                                             static_cast<int>(VK_API_VERSION_MINOR(apiVersion))));
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

    // The instance exists: did it take every extension the headset runtime asked for? Qt drops
    // the ones the driver lacks silently, and a runtime short of one cannot drive the device.
    if (m_xr) {
        const QByteArrayList enabled = m_instance->extensions();
        for (const std::string& name : m_xr->vulkanInstanceExtensions()) {
            if (!enabled.contains(QByteArray::fromStdString(name))) {
                VrSettings::setStatus(tr("This GPU driver lacks a Vulkan instance extension the headset runtime "
                                         "needs (%1).")
                                          .arg(QString::fromStdString(name)));
                qWarning() << "[vr] instance extension unavailable:" << QString::fromStdString(name);
                m_xr.reset();
                break;
            }
        }
    }

    m_window = new VulkanWindow(m_instance.get(), apiVersion, shaderDir);
    m_window->setOpenXrSystem(m_xr.get()); // before the first expose builds the device
    connect(m_window, &VulkanWindow::vrModeChanged, this, &ViewportWidget::vrModeChanged);
    connect(m_window, &VulkanWindow::vrError, this, &ViewportWidget::vrError);
    // Signal-to-signal forward: undo/redo restoring a lighting state surfaces on the facade,
    // where the Environment panel listens.
    connect(m_window, &VulkanWindow::lightingRestored, this, &ViewportWidget::lightingRestored);
    // ... and likewise the selected joint's transform changing, for the Transform tab.
    connect(m_window, &VulkanWindow::jointTransformChanged, this, &ViewportWidget::jointTransformChanged);
    // ... and the Materials tab's view of the selected object changing.
    connect(m_window, &VulkanWindow::materialViewChanged, this, &ViewportWidget::materialViewChanged);
    // The scripted test's `uishot`: the main window's widgets as a picture (the panels beside the
    // viewport; the native viewport itself is not in a widget grab — `shot` reads that off the GPU),
    // and the floating strip, a window of its own, beside it as <name>_strip.png (its badge).
    connect(m_window, &VulkanWindow::uiShotRequested, this, [this](const QString& path) {
        if (QWidget* top = window()) {
            top->grab().save(path);
        }
        if (m_strip) {
            QString stripPath = path;
            stripPath.replace(QStringLiteral(".png"), QStringLiteral("_strip.png"));
            m_strip->grab().save(stripPath);
        }
    });
    // THE JOINT MOUSE MODE's input wherever it lands (see jointmodalinput.h): on with the mode,
    // off when it ends.
    m_modalInput = new JointModalInput(m_window, this);
    connect(m_window, &VulkanWindow::jointModalChanged, this,
            [this](int kind) { m_modalInput->setActive(kind >= 0); });
    // The scripted test's `app`: the mode's input as it reaches the application with the viewport
    // NOT focused. The keys are the Edit menu's actions (found by their shortcut, and only if
    // enabled, as the shortcut map would); Esc, Enter, the clicks and the moves are events sent
    // to the main window, through the application's event filters like any other.
    connect(m_window, &VulkanWindow::appInputRequested, this,
            [this](const QString& what, const QPointF& delta, int steps) {
        QWidget* top = window();
        QWindow* handle = top ? top->windowHandle() : nullptr;
        if (!handle || !m_window) {
            return;
        }
        const auto sendKey = [handle](int key) {
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            QCoreApplication::sendEvent(handle, &press);
        };
        const auto sendMouse = [this, handle](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons held) {
            QMouseEvent e(type, handle->mapFromGlobal(m_scriptCursor), m_scriptCursor, button, held, Qt::NoModifier);
            QCoreApplication::sendEvent(handle, &e);
        };
        if (what == QLatin1String("b") || what == QLatin1String("s") || what == QLatin1String("t")) {
            const QKeySequence key(what == QLatin1String("b") ? Qt::Key_B : what == QLatin1String("s") ? Qt::Key_S : Qt::Key_T);
            m_scriptCursor = m_window->mapToGlobal(QPointF(m_window->width() * 0.5, m_window->height() * 0.5));
            const QList<QAction*> actions = top->findChildren<QAction*>();
            for (QAction* action : actions) {
                if (action->shortcut() == key && action->isEnabled()) {
                    action->trigger();
                    break;
                }
            }
        } else if (what == QLatin1String("esc")) {
            sendKey(Qt::Key_Escape);
        } else if (what == QLatin1String("enter")) {
            sendKey(Qt::Key_Return);
        } else if (what == QLatin1String("click") || what == QLatin1String("rclick")) {
            const Qt::MouseButton button = what == QLatin1String("click") ? Qt::LeftButton : Qt::RightButton;
            sendMouse(QEvent::MouseButtonPress, button, button);
            sendMouse(QEvent::MouseButtonRelease, button, Qt::NoButton);
        } else if (what == QLatin1String("move")) {
            for (int k = 0; k < steps; ++k) {
                m_scriptCursor += delta / steps;
                sendMouse(QEvent::MouseMove, Qt::NoButton, Qt::NoButton);
            }
        }
    });
    // Same forward for the document state: the main window's title listens on the facade.
    connect(m_window, &VulkanWindow::documentChanged, this, &ViewportWidget::documentChanged);
    connect(m_window, &VulkanWindow::environmentRestored, this, &ViewportWidget::environmentRestored);
    m_container = QWidget::createWindowContainer(m_window, this);
    m_container->setFocusPolicy(Qt::StrongFocus); // so the viewport can receive wheel/keys
    layout->addWidget(m_container);

    createStrip(); // the floating control strip in the top-right corner

    // Application-wide, for the one key that must work wherever the focus is: Esc leaves VR Mode
    // (eventFilter). The top-level window's own filter (anchorStrip) is installed in showEvent.
    QCoreApplication::instance()->installEventFilter(this);
}

void ViewportWidget::importObj(const QString& path) {
    withWindow([&](VulkanWindow& w) { w.importObj(path); });
}

void ViewportWidget::importFigure(const QString& path) {
    withWindow([&](VulkanWindow& w) { w.importFigure(path); });
}

void ViewportWidget::importSplat(const QString& path) {
    withWindow([&](VulkanWindow& w) { w.importSplat(path); });
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

bool ViewportWidget::saveProjectFile(const QString& path) {
    return m_window && m_window->saveProjectFile(path);
}

int ViewportWidget::loadProjectFile(const QString& path,
                                    const std::map<std::string, std::string>& recovered,
                                    std::vector<std::string>& missing, std::string& error) {
    if (!m_window) {
        error = "no viewport (Vulkan unavailable)";
        return -1;
    }
    return m_window->loadProjectFile(path, recovered, missing, error);
}

QString ViewportWidget::projectPath() const {
    return m_window ? m_window->projectPath() : QString();
}

void ViewportWidget::setProjectPath(const QString& path) {
    withWindow([path](VulkanWindow& w) { w.setProjectPath(path); });
}

bool ViewportWidget::isProjectDirty() const {
    return m_window && m_window->isProjectDirty();
}

void ViewportWidget::setProjectClean() {
    withWindow([](VulkanWindow& w) { w.setProjectClean(); });
}

void ViewportWidget::markProjectDirty() {
    withWindow([](VulkanWindow& w) { w.markProjectDirty(); });
}

void ViewportWidget::setShadeMode(int mode) {
    withWindow([mode](VulkanWindow& w) { w.setShadeMode(mode); });
}

void ViewportWidget::deleteSelectedObject() {
    withWindow([](VulkanWindow& w) { w.deleteSelectedObject(); });
}

void ViewportWidget::resetToEmptyScene() {
    withWindow([](VulkanWindow& w) { w.resetToEmptyScene(); });
}

void ViewportWidget::resetSelectedJoint() {
    withWindow([](VulkanWindow& w) { w.resetSelectedJoint(); });
}

void ViewportWidget::resetSelectedLimb() {
    withWindow([](VulkanWindow& w) { w.resetSelectedLimb(); });
}

void ViewportWidget::resetPose() {
    withWindow([](VulkanWindow& w) { w.resetPose(); });
}

void ViewportWidget::mirrorPose() {
    withWindow([](VulkanWindow& w) { w.mirrorPose(); });
}

void ViewportWidget::mirrorSelectedLimb() {
    withWindow([](VulkanWindow& w) { w.mirrorSelectedLimb(); });
}

JointTransform ViewportWidget::jointTransform() const {
    return m_window ? m_window->jointTransform() : JointTransform{};
}

bool ViewportWidget::beginJointTransformEdit() {
    return m_window && m_window->beginJointTransformEdit();
}

void ViewportWidget::setFigurePosition(int axis, double cm) {
    withWindow([axis, cm](VulkanWindow& w) { w.setFigurePosition(axis, cm); });
}

void ViewportWidget::setFigureRotation(int axis, double degrees) {
    withWindow([axis, degrees](VulkanWindow& w) { w.setFigureRotation(axis, degrees); });
}

void ViewportWidget::setFigureScale(double pct) {
    withWindow([pct](VulkanWindow& w) { w.setFigureScale(pct); });
}

MaterialView ViewportWidget::materialView() const {
    return m_window ? m_window->materialView() : MaterialView{};
}

void ViewportWidget::setMaterialZone(int zone) {
    withWindow([zone](VulkanWindow& w) { w.setMaterialZone(zone); });
}

bool ViewportWidget::beginMaterialEdit() {
    return m_window && m_window->beginMaterialEdit();
}

void ViewportWidget::setSelectedMaterial(const MaterialParams& params, bool linked) {
    withWindow([&params, linked](VulkanWindow& w) { w.setSelectedMaterial(params, linked); });
}

void ViewportWidget::endMaterialEdit() {
    withWindow([](VulkanWindow& w) { w.endMaterialEdit(); });
}

void ViewportWidget::resetSelectedMaterial(bool linked, bool allZones) {
    withWindow([linked, allZones](VulkanWindow& w) { w.resetSelectedMaterial(linked, allZones); });
}

bool ViewportWidget::setMaterialTexture(MaterialMap map, const QString& source) {
    return m_window && m_window->setMaterialTexture(map, source);
}

std::shared_ptr<const DecodedImage> ViewportWidget::materialSlotImage(MaterialMap map, int* channel) const {
    return m_window ? m_window->materialSlotImage(map, channel) : nullptr;
}

void ViewportWidget::setJointTransformDial(int dial, double value, bool absolute) {
    withWindow([dial, value, absolute](VulkanWindow& w) { w.setJointTransformDial(dial, value, absolute); });
}

void ViewportWidget::endJointTransformEdit() {
    withWindow([](VulkanWindow& w) { w.endJointTransformEdit(); });
}

void ViewportWidget::toggleJointModal(int kind) {
    withWindow([kind](VulkanWindow& w) { w.toggleJointModal(kind); });
}

void ViewportWidget::setShowSkeleton(bool on) {
    // The window de-duplicates the state itself; only a real change is announced, so the
    // strip ↔ View-menu sync (both driven by the signal) stays one round-trip.
    if (!m_window || m_window->showSkeleton() == on) {
        return;
    }
    m_window->setShowSkeleton(on);
    emit skeletonVisibilityChanged(on);
}

bool ViewportWidget::showSkeleton() const {
    return m_window && m_window->showSkeleton();
}

void ViewportWidget::resetView() {
    withWindow([](VulkanWindow& w) { w.resetView(); });
}

void ViewportWidget::setAxisView(AxisView view) {
    withWindow([view](VulkanWindow& w) { w.setAxisView(view); });
}

void ViewportWidget::flipView() {
    withWindow([](VulkanWindow& w) { w.flipView(); });
}

void ViewportWidget::frameSelected() {
    withWindow([](VulkanWindow& w) { w.frameSelected(); });
}

void ViewportWidget::groundFigure() {
    withWindow([](VulkanWindow& w) { w.groundFigure(); });
}

void ViewportWidget::applySoftBodySettings() {
    withWindow([](VulkanWindow& w) { w.applySoftBodySettings(); });
}

void ViewportWidget::setEnvironment(const QString& hdrPath) {
    withWindow([&](VulkanWindow& w) { w.setEnvironmentFile(hdrPath); });
}

void ViewportWidget::setLightingSettings(const LightingSettings& settings) {
    withWindow([&](VulkanWindow& w) { w.setLightingSettings(settings); });
}

void ViewportWidget::undo() {
    withWindow([](VulkanWindow& w) { w.undo(); });
}

void ViewportWidget::redo() {
    withWindow([](VulkanWindow& w) { w.redo(); });
}

void ViewportWidget::registerLightingUndo(const LightingSettings& preEdit) {
    withWindow([&](VulkanWindow& w) { w.registerLightingUndo(preEdit); });
}

bool ViewportWidget::setVrMode(bool on) {
    return m_window && m_window->setVrMode(on);
}

bool ViewportWidget::vrModeActive() const {
    return m_window && m_window->vrModeActive();
}

void ViewportWidget::recenterVr() {
    withWindow([](VulkanWindow& w) { w.recenterVr(); });
}

QStringList ViewportWidget::shaderModeNames() {
    // Picker order = the table's order (scene/shademode.h); the index handed to setShadeMode is
    // a row of that table, and the row maps itself onto mesh.frag's mode + the draw variants.
    QStringList names;
    for (const ShadeMode& mode : kShadeModes) {
        names << QString::fromUtf8(mode.name);
    }
    return names;
}

void ViewportWidget::createStrip() {
    m_strip = new ViewportStrip(this);

    // Strip -> window, always through the facade methods so the menus, the keys and the strip
    // take one path (and the skeleton toggle announces itself once, via the signal below).
    connect(m_strip, &ViewportStrip::shadeModeSelected, this, &ViewportWidget::setShadeMode);
    connect(m_strip, &ViewportStrip::viewSelected, this, [this](ViewPreset preset) {
        switch (preset) {
        case ViewPreset::Top:    setAxisView(AxisView::Top);    break;
        case ViewPreset::Bottom: setAxisView(AxisView::Bottom); break;
        case ViewPreset::Front:  setAxisView(AxisView::Front);  break;
        case ViewPreset::Back:   setAxisView(AxisView::Back);   break;
        case ViewPreset::Left:   setAxisView(AxisView::Left);   break;
        case ViewPreset::Right:  setAxisView(AxisView::Right);  break;
        case ViewPreset::Home:
        case ViewPreset::Free:   resetView();                   break;
        }
    });
    connect(m_strip, &ViewportStrip::homeClicked, this, &ViewportWidget::resetView);
    connect(m_strip, &ViewportStrip::groundClicked, this, &ViewportWidget::groundFigure);
    connect(m_strip, &ViewportStrip::skeletonToggled, this, &ViewportWidget::setShowSkeleton);

    // Window -> strip: the view caption follows the camera (keys, View menu, orbit), the shader
    // picker follows the shade mode, the badge follows the joint mouse mode, and the Skeleton
    // button follows the overlay state whoever changed it (the signal is the single source of truth).
    connect(m_window, &VulkanWindow::viewPresetChanged, m_strip, &ViewportStrip::setViewPreset);
    connect(m_window, &VulkanWindow::shadeModeChanged, m_strip, &ViewportStrip::setShadeMode);
    connect(m_window, &VulkanWindow::jointModalChanged, m_strip, &ViewportStrip::setJointModeBadge);
    connect(m_window, &VulkanWindow::vrModeChanged, m_strip, &ViewportStrip::setVrActive);
    connect(m_strip, &ViewportStrip::vrModeToggled, this, &ViewportWidget::vrModeRequested);
    m_strip->setVrActive(m_window->vrModeActive());
    connect(this, &ViewportWidget::skeletonVisibilityChanged, m_strip,
            &ViewportStrip::setSkeletonChecked);
    m_strip->setViewPreset(m_window->currentView());
    m_strip->setSkeletonChecked(m_window->showSkeleton());
}

void ViewportWidget::anchorStrip() {
    if (m_strip && m_container) {
        m_strip->anchorTo(m_container);
    }
}

void ViewportWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!m_strip) {
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
    m_strip->show();
    m_strip->raise();
    anchorStrip();
}

void ViewportWidget::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_strip) {
        m_strip->hide();
    }
}

void ViewportWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    anchorStrip();
}

void ViewportWidget::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    anchorStrip();
}

bool ViewportWidget::eventFilter(QObject* watched, QEvent* event) {
    // ESCAPE, anywhere in the application, leaves VR Mode (installed on the application for
    // that; the event is read at the WINDOW it arrives at, once per press). Not while the joint
    // mouse mode is on — its Esc puts the joint back, and JointModalInput consumes it — and not
    // while a modal dialog has the keyboard (its own Esc closes it).
    if (event->type() == QEvent::KeyPress && watched->isWindowType() && m_window && m_window->vrModeActive() &&
        m_window->jointModal() < 0 && QApplication::activeModalWidget() == nullptr) {
        const auto* key = static_cast<const QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && !key->isAutoRepeat()) {
            m_window->setVrMode(false);
            return true;
        }
    }
    // ESCAPE with nothing else claiming it — no joint mouse mode, no VR Mode, no text field being
    // edited (its own Esc cancels the edit) — DESELECTS whatever the scene has selected: the
    // model and its joint. Read at the main window and the viewport's own window only, so the
    // manual's Esc (its own top-level window) still closes it; a modal dialog never lets a key
    // reach either (the scripted import's progress dialog lingers as the "active modal widget"
    // without the keyboard, which is why that test is not made here).
    if (event->type() == QEvent::KeyPress && m_window && m_window->jointModal() < 0 && !m_window->vrModeActive() &&
        (watched == m_window || (m_filteredWindow != nullptr && watched == m_filteredWindow->windowHandle()))) {
        const auto* key = static_cast<const QKeyEvent*>(event);
        // An edit in progress keeps its Esc: a scrub field's inline editor (a QLineEdit under a
        // DragNumberBox), an item view's inline RENAME editor (the Asset Manager's collections: a
        // QLineEdit inside the tree, whose Esc cancels the rename), a spin box, a text edit — not
        // the Asset Manager's search field, which merely holds the focus until the viewport is clicked.
        QWidget*   focus = QApplication::focusWidget();
        const auto inItemView = [](QWidget* w) {
            for (QWidget* p = w != nullptr ? w->parentWidget() : nullptr; p != nullptr; p = p->parentWidget()) {
                if (qobject_cast<QAbstractItemView*>(p) != nullptr) {
                    return true;
                }
            }
            return false;
        };
        const bool editing = (qobject_cast<QLineEdit*>(focus) != nullptr && focus->parentWidget() != nullptr &&
                              (qobject_cast<DragNumberBox*>(focus->parentWidget()) != nullptr || inItemView(focus))) ||
                             qobject_cast<QAbstractSpinBox*>(focus) != nullptr || qobject_cast<QTextEdit*>(focus) != nullptr;
        if (key->key() == Qt::Key_Escape && !key->isAutoRepeat() && !editing && m_window->hasSelection()) {
            m_window->clearSelection();
            return true;
        }
    }
    if (watched == m_filteredWindow) {
        const QEvent::Type t = event->type();
        if (t == QEvent::Move || t == QEvent::Resize || t == QEvent::WindowStateChange) {
            anchorStrip();
        } else if (t == QEvent::WindowActivate) {
            // The main window was activated (focus, un-minimize, user click): re-raise the strip
            // so it can't sit BEHIND the main window. On X11 a top-level strip's stacking
            // relative to its owner is the WM's call, so we re-assert it whenever the owner is
            // raised. WindowActivate, not ActivationChange (which is also sent on deactivation):
            // the strip must not be raised over a modal dialog or the splash while they hold the
            // focus.
            if (m_strip && m_strip->isVisible()) {
                m_strip->raise();
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

ViewportWidget::~ViewportWidget() {
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->removeEventFilter(this); // the Esc-leaves-VR filter
    }
    // The QVulkanInstance (m_instance) owns the VkInstance, and the VulkanWindow's
    // device/renderer were created from it. Qt would otherwise destroy the window via the
    // base QWidget destructor — i.e. AFTER m_instance is gone — leaving the renderer to
    // call vkDestroyDevice on a dead instance. Destroy the container (hence the window,
    // hence all Vulkan objects) here, explicitly, while m_instance is still alive.
    //
    // The container goes BEFORE the strip: the window's teardown (releaseVulkan) ends an open
    // joint mouse mode by emitting jointModalChanged(-1), which lands on the strip's badge — with
    // the strip already deleted that was a use-after-free. The strip is also unhooked first so
    // that emit can't re-anchor it against a container mid-destruction.
    if (m_window && m_strip) {
        disconnect(m_window, nullptr, m_strip, nullptr);
    }
    // The joint mouse mode's application-wide input goes first too, unhooked from the window: it
    // holds the window's pointer, and the same teardown emit would reach it through this widget.
    if (m_window) {
        disconnect(m_window, nullptr, this, nullptr);
    }
    delete m_modalInput;
    m_modalInput = nullptr;
    delete m_container;
    m_container = nullptr;
    m_window = nullptr; // was owned by m_container; now dangling — clear it

    // The strip is an owned top-level window with no Vulkan resources (Qt would also delete it
    // as a child, but explicit keeps every pointer here accounted for).
    delete m_strip;
    m_strip = nullptr;
}

} // namespace pose
