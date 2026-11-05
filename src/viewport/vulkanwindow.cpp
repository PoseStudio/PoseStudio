/**
 * @file vulkanwindow.cpp
 * @brief VulkanWindow lifecycle: construction, Vulkan init / teardown / device loss, the scene
 *        state remembered on the renderer's behalf, the import queue, and the event-driven frame.
 *
 * One of the seven translation units of VulkanWindow — see vulkanwindow.h for the map. This one
 * owns everything that exists BEFORE and AFTER the renderer: the timers are created here, the
 * renderer is built on first expose and released when the platform surface goes away or the
 * device is lost (failDevice is the single device-loss path, so every failure resets the
 * interaction state the same way), and the settings / imports requested while no renderer
 * exists are replayed once it does.
 */

#include "vulkanwindow.h"

#include "figureimportservice.h"
#include "importformats.h"
#include "riggedimportservice.h"
#include "splatimportservice.h"
#include "ikdiagnostics.h"
#include "ikscript.h"
#include "modelimportservice.h"
#include "openxrsystem.h" // the headset's device requirements (vr/)
#include "rendering/vulkancommon.h"
#include "rendering/vulkancontext.h"
#include "rendering/vulkanrenderer.h"
#include "scene/scene.h"
#include "vrsettings.h"

#include <QDebug>
#include <QFileInfo>
#include <QSize>
#include <QTimer>
#include <QVulkanInstance>
#include <QtGui/qevent.h> // QPlatformSurfaceEvent lives here (no separate qpa header in binary Qt)

#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>

namespace pose {

namespace {
// The renderer speaks Vulkan extents; the window keeps its app-facing header Vulkan-free by
// reporting a QSize and converting at the one seam.
VkExtent2D toExtent(const QSize& size) {
    return VkExtent2D{static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height())};
}
} // namespace

VulkanWindow::VulkanWindow(QVulkanInstance* instance, uint32_t apiVersion, QString shaderDir,
                           QWindow* parent)
    : QWindow(parent), m_instance(instance), m_apiVersion(apiVersion),
      m_shaderDir(std::move(shaderDir)) {
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(m_instance);

    // FBIK drag ticker: the IK solve is rate-limited per call, so while the drag button is held
    // the last target is re-issued at ~60 Hz — the pose keeps catching up (and settles) even when
    // the mouse stops moving and mouse-move events stop with it. It is the SOLE solve driver.
    m_ikTimer = new QTimer(this);
    m_ikTimer->setInterval(16);
    if (qEnvironmentVariableIsSet("POSESTUDIO_IK_PRECISE_TIMER")) {
        m_ikTimer->setTimerType(Qt::PreciseTimer); // diagnostics A/B against the coarse default
    }
    connect(m_ikTimer, &QTimer::timeout, this, &VulkanWindow::onIkTick);

    // The ground button's fall (see groundFigure): free fall from rest, evaluated against real
    // elapsed time so the landing takes the same 0.45s per metre whatever the frame cadence.
    m_fall.timer = new QTimer(this);
    m_fall.timer->setInterval(16);
    connect(m_fall.timer, &QTimer::timeout, this, &VulkanWindow::onFallTick);

    m_diag = IkDiagnostics::fromEnvironment(); // null unless POSESTUDIO_IK_PERF / _BENCH is set
    m_script = IkScript::fromEnvironment();    // null unless POSESTUDIO_IK_SCRIPT is set

    // The document state's transitions surface as documentChanged (signal-to-signal forward; the
    // facade re-emits it for the window-title update).
    connect(&m_project, &ProjectState::changed, this, &VulkanWindow::documentChanged);
}

VulkanWindow::~VulkanWindow() {
    releaseVulkan();
}

Scene& VulkanWindow::scene() const {
    return m_renderer->scene();
}

QSize VulkanWindow::pixelExtent() const {
    const qreal dpr = devicePixelRatio();
    return QSize(static_cast<int>(std::lround(width() * dpr)),
                 static_cast<int>(std::lround(height() * dpr)));
}

void VulkanWindow::syncRendererExtent() {
    if (m_renderer) {
        m_renderer->notifyResize(toExtent(pixelExtent()));
        m_renderer->setUiScale(static_cast<float>(devicePixelRatio())); // may change with the screen
        requestUpdate();
    }
}

void VulkanWindow::DeferredSceneState::applyTo(VulkanRenderer& renderer) const {
    Scene& scene = renderer.scene();
    scene.setShadeMode(shadeMode);       // a mode chosen before first expose
    scene.setShowSkeleton(showSkeleton); // a skeleton toggle chosen before first expose
    scene.setLightingSettings(lighting); // any dials set before first expose
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
        // The headset runtime's device requirements (set at startup, see setOpenXrSystem): its
        // physical device and extensions. A device that can't meet them is still a fine desktop
        // device — VR Mode is then simply unavailable, with the reason on the Preferences page.
        VulkanContextOptions options;
        if (m_xr) {
            options.preferredDevice = m_xr->vulkanPhysicalDevice(m_instance->vkInstance());
            options.extraDeviceExtensions = m_xr->vulkanDeviceExtensions();
        }
        m_context = std::make_unique<VulkanContext>(m_instance->vkInstance(), surface, m_apiVersion, options);
        if (m_xr) {
            if (m_context->vrDeviceReady()) {
                VrSettings::setReady(true);
                VrSettings::setStatus(tr("Ready: %1 (runtime: %2). View → VR Mode puts the scene in the headset.")
                                          .arg(QString::fromStdString(m_xr->systemName()),
                                               QString::fromStdString(m_xr->runtimeName())));
                qInfo() << "[vr] headset ready:" << QString::fromStdString(m_xr->systemName());
            } else {
                VrSettings::setReady(false);
                VrSettings::setStatus(tr("The headset (%1) is driven by a GPU this viewport cannot render through, "
                                         "or that GPU lacks an extension the runtime needs — see the log.")
                                          .arg(QString::fromStdString(m_xr->systemName())));
                qWarning() << "[vr] the Vulkan device does not meet the headset runtime's requirements; VR Mode unavailable";
                m_xr = nullptr;
            }
        }
        m_renderer = std::make_unique<VulkanRenderer>(*m_context, toExtent(pixelExtent()),
                                                      m_shaderDir.toStdString());
        m_deferred.applyTo(*m_renderer);
        m_renderer->setUiScale(static_cast<float>(devicePixelRatio())); // outline width in logical px
        noteView(ViewPreset::Home); // a fresh camera starts at its default framing (a rebuilt
                                    // renderer after a device loss must not keep a stale view)

        // Image-based lighting: bake a real HDR panorama over the renderer's built-in procedural
        // studio — the environment the user picked before first expose, else the default. A
        // remembered file that no longer exists must not pre-empt the default. The bake runs off
        // the UI thread; the first frames render the procedural studio (from the Scene ctor)
        // until it lands.
        QString environment = m_deferred.environmentPath;
        if (environment.isEmpty() || !QFileInfo::exists(environment)) {
            environment = defaultEnvironmentPath();
        }
        beginEnvironmentBake(environment, /*autoAimKey=*/false); // startup: the authored default dials win
        m_initialized = true;

        // Drain any imports requested before the renderer existed, in request order (e.g. the
        // user hit Import while the viewport was still hidden). A bad file only drops that one
        // model, not the window. NOTE: on Windows the first expose is delivered synchronously
        // inside QMainWindow::show(), so main.cpp's command-line imports (issued after show())
        // usually find the renderer already built and run on the INTERACTIVE progress-dialog
        // path — this drain is the fallback for platforms that expose later, and for a viewport
        // that starts hidden.
        std::vector<PendingImport> queued;
        queued.swap(m_deferred.imports);
        for (const PendingImport& pending : queued) {
            if (runImport(pending, /*showProgress=*/false)) {
                requestUpdate();
            }
        }
        if (m_diag && m_diag->benchRequested()) {
            std::fprintf(stderr, "[ikbench] drained %zu queued figure(s)\n", queued.size());
            std::fflush(stderr);
        }
        if (!m_deferred.pendingPose.isEmpty()) {
            scene().loadPose(m_deferred.pendingPose.toStdString()); // onto the just-drained figure
            m_deferred.pendingPose.clear();
            requestUpdate();
        }
        if (m_diag && m_diag->benchRequested()) {
            QTimer::singleShot(1500, this, &VulkanWindow::startBench); // after the first frames
        }
        if (m_script) {
            QTimer::singleShot(1500, this, &VulkanWindow::startScript);
        }
        // POSESTUDIO_VR_AUTOSTART=1: straight into VR Mode once the first frames are up — the
        // developer lever for exercising the session with no menu clicks (and for a launch that
        // goes directly to the headset). With no headset it only LOGS: the menu path's "cannot
        // start" dialog is modal, and raised from inside the command-line import's progress loop
        // it would hold the import (and a scripted run) until someone dismissed it.
        if (qEnvironmentVariableIsSet("POSESTUDIO_VR_AUTOSTART")) {
            QTimer::singleShot(1500, this, [this]() {
                if (!m_xr) {
                    qInfo() << "[vr] autostart: no headset —" << VrSettings::status();
                } else if (!setVrMode(true)) {
                    qInfo() << "[vr] autostart: VR Mode did not start";
                }
            });
        }
    } catch (const VulkanError& e) {
        // Leave the surface intact (Qt owns it) but mark the device as failed so we don't retry
        // every expose. The container just shows the clear-colour-less window.
        failDevice("Viewport initialisation", e);
    }
}

void VulkanWindow::failDevice(const char* stage, const VulkanError& error) {
    // Every device-loss site (init, the environment upload's queued slot, a frame's VK_CHECK
    // after a driver reset/TDR) ends here: the interaction state machines must reset with the
    // renderer they acted on, or a drag/settle/fall/joint mouse mode outlives its figure and the
    // mode's badge stays lit over a dead viewport.
    releaseVulkan();
    m_deviceFailed = true;
    qCritical() << "[Vulkan]" << stage << "failed; viewport disabled:" << error.what();
    notifyJointTransform(); // the figure went with the renderer: the Transform tab empties
}

void VulkanWindow::releaseVulkan() {
    // VR Mode first: its session holds swapchain images on the device, and its wait thread
    // must be gone before the renderer it renders through.
    stopVr(QString());
    // A pending IK drag or release settle dies with the renderer (nothing left to settle or
    // commit) — the DRAG flags must clear too, or a rebuilt renderer inherits a phantom drag
    // whose eventual release would settle-and-commit against a stale pre-teardown pose. The
    // button mask and FK flag clear with them: a later press must start clean.
    m_ik.settling = false;
    m_ik.dragging = false;
    m_ik.hasTarget = false;
    m_ik.poseChanged = false;
    m_leftClickCandidate = false;
    m_activeDragButtons = Qt::NoButton;
    m_transformEdit = false; // (an open dial edit dies with the figure it turned)
    m_modalSwallow = Qt::NoButton;
    if (m_modalKind >= 0) {
        m_modalKind = -1; // the joint the mouse turned is going away with the renderer
        emit jointModalChanged(-1);
    }
    if (m_fall.timer) {
        m_fall.timer->stop(); // the figure is going away with the renderer
        m_fall.height = 0.0f;
        m_fall.dropped = 0.0f;
        m_fall.bouncing = false; // (... and its landing bounce with it: nothing left to restore)
        m_fall.figure = -1;
    }
    if (m_ikTimer) {
        m_ikTimer->stop();
    }
    // Order matters: the renderer waits for the device to idle in its destructor, so it
    // must go before the context (which owns the device).
    m_renderer.reset();
    m_context.reset();
    m_initialized = false;
}

bool VulkanWindow::runImport(const PendingImport& import, bool showProgress) {
    // All the parse/decode/upload orchestration (and its progress UI) lives in the Qt-facing
    // import services, keeping both the importers and the renderer core Qt-free. The skeleton
    // overlay stays hidden after a figure import; a drag on a joint still grabs it (full-body
    // IK — picking is independent of the overlay).
    bool ok = false;
    switch (import.kind) {
    case PendingImport::Kind::Obj:
        ok = ModelImportService::importInto(*m_renderer, import.path, showProgress);
        break;
    case PendingImport::Kind::Figure:
        // (By extension: a native figure file, or a rigged character in an interchange file —
        // FBX, glTF / GLB, USD — conformed to the rig standard on the way in, see
        // riggedimportservice.h.)
        ok = ImportFormats::isRiggedFile(import.path)
                 ? RiggedImportService::importInto(*m_renderer, import.path, showProgress)
                 : FigureImportService::importInto(*m_renderer, import.path, showProgress);
        break;
    case PendingImport::Kind::Splat:
        ok = SplatImportService::importInto(*m_renderer, import.path, showProgress);
        break;
    }
    if (ok) {
        markProjectDirty(); // a new model is a document change
    }
    return ok;
}

void VulkanWindow::importOrQueue(PendingImport import) {
    if (m_deviceFailed) {
        qWarning() << "[viewport] Ignoring import; Vulkan is unavailable:" << import.path;
        return;
    }
    if (!m_renderer) {
        m_deferred.imports.push_back(std::move(import)); // loaded once initializeVulkan() builds the renderer
        return;
    }
    // The new model becomes the selection (and, for a figure, the active posing target): a
    // release settle still ticking, a fall, or an open dial edit would retarget to it and commit
    // the previous figure's snapshot against the new figure's index — close them first.
    closeOpenPoseEdits();
    if (runImport(import, /*showProgress=*/true)) {
        requestUpdate();
    }
}

void VulkanWindow::importObj(const QString& path) {
    importOrQueue({PendingImport::Kind::Obj, path});
}

void VulkanWindow::importFigure(const QString& path) {
    importOrQueue({PendingImport::Kind::Figure, path});
}

void VulkanWindow::importSplat(const QString& path) {
    importOrQueue({PendingImport::Kind::Splat, path});
}

bool VulkanWindow::hasPosableFigure() const {
    return m_renderer && scene().hasPosableFigure();
}

void VulkanWindow::setShadeMode(int mode) {
    if (m_announcedShadeMode == mode) {
        return; // only a real change is announced (the strip mirrors the signal)
    }
    m_deferred.shadeMode = mode; // remembered so a mode chosen before first expose still applies
    withRenderer([mode](VulkanRenderer& r) { r.scene().setShadeMode(mode); });
    m_announcedShadeMode = mode;
    emit shadeModeChanged(mode);
}

void VulkanWindow::setShowSkeleton(bool on) {
    m_deferred.showSkeleton = on; // remembered so a toggle before first expose still applies
                                  // (ViewportWidget de-duplicates: it must know whether to signal)
    withRenderer([on](VulkanRenderer& r) { r.scene().setShowSkeleton(on); });
}

bool VulkanWindow::showSkeleton() const {
    return m_deferred.showSkeleton;
}

void VulkanWindow::exposeEvent(QExposeEvent*) {
    if (isExposed()) {
        initializeVulkan();
        syncRendererExtent(); // expose events report the current size too
    }
}

void VulkanWindow::resizeEvent(QResizeEvent*) {
    syncRendererExtent();
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
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
        // A scripted test drives the gestures itself (POSESTUDIO_IK_SCRIPT) and runs on a desktop
        // in use: real input that happens to reach the window is not part of it. A wheel notch
        // from the user's own scrolling, the cursor merely passing over the window, dollied the
        // camera under a scripted drag — the drag's target jumped with the view's scale (a 14cm
        // "pop" no rerun reproduced) and every later shot of the script was framed differently.
        if (m_script) {
            e->accept();
            return true;
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
    notifyJointTransform(); // the Transform tab follows whatever this frame is about to show
    try {
        const bool perfFrame = m_diag && (m_ik.dragging || m_ik.settling);
        const qint64 f0 = perfFrame ? m_diag->frameBegin() : 0;
        if (m_renderer->drawFrame()) {
            requestUpdate();
        }
        if (perfFrame) {
            m_diag->frameEnd(f0);
        }
        if (m_script) {
            scriptFrameRendered(); // a `shot` waiting for its frame
        }
    } catch (const VulkanError& e) {
        // Runtime VK_CHECK failure (e.g. VK_ERROR_DEVICE_LOST after a driver reset/TDR): tear the
        // renderer down and stop scheduling frames, rather than letting the exception escape
        // Qt's event loop and abort the whole app.
        failDevice("Rendering", e);
    }
}

} // namespace pose
