/**
 * @file vulkanwindow_environment.cpp
 * @brief VulkanWindow's lighting environment: the default HDRI, the off-thread IBL bake, and the
 *        live lighting dials.
 *
 * One of the seven translation units of VulkanWindow — see vulkanwindow.h for the map. This is
 * the only QtConcurrent user in the viewport: decoding + prefiltering a panorama costs hundreds
 * of milliseconds of CPU, so it runs on a worker and the result is uploaded on the GUI thread,
 * guarded by a request id (a slower earlier bake must never clobber a newer pick) and by a catch
 * boundary (a worker exception is rethrown by QFuture::result() INSIDE the finished slot, and an
 * exception escaping a slot is fatal to a Qt app).
 */

#include "vulkanwindow.h"

#include "environmentsource.h"
#include "librarypaths.h"
#include "rendering/vulkancommon.h"
#include "rendering/vulkanrenderer.h"
#include "scene/environment.h"
#include "scene/scene.h"

#include <QDebug>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace pose {

QString VulkanWindow::defaultEnvironmentPath() const {
    // The stock/first panorama from the user library's hdri/ tree — the same recursive scan
    // backing the Environment panel's menu, so a collection categorized into subfolders resolves
    // identically here. (The <appDir>/environment.{hdr,exr} developer override lives in
    // LibraryPaths::defaultHdri() too, so every consumer of "the default" agrees.) An empty
    // return leaves the procedural studio environment (beginEnvironmentBake no-ops on it).
    return LibraryPaths::defaultHdri();
}

void VulkanWindow::beginEnvironmentBake(const QString& hdrPath, bool autoAimKey) {
    if (!m_renderer || hdrPath.isEmpty() || !QFileInfo::exists(hdrPath)) {
        return; // no renderer yet, or nothing to load — the procedural default stays
    }
    // Decode + bake the HDRI off the UI thread: the CPU prefilter is hundreds of ms and would freeze the
    // app on every switch. The viewport keeps rendering the current environment until the result lands.
    // A per-request id discards a slower earlier bake whose selection a newer one has since superseded.
    const quint64 requestId = ++m_environmentRequestId;
    auto* watcher = new QFutureWatcher<std::shared_ptr<BakedEnvironment>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, requestId, autoAimKey, hdrPath]() {
        watcher->deleteLater();
        std::shared_ptr<BakedEnvironment> baked;
        try {
            baked = watcher->result();
        } catch (const std::exception& e) {
            // The worker threw (QtConcurrent rethrows it here as QUnhandledException): a corrupt
            // file whose header declares an absurd size ends in bad_alloc. A bad panorama drops
            // only itself — the current environment stays.
            qWarning() << "[viewport] Environment bake failed:" << hdrPath << "-" << e.what();
            return;
        } catch (...) {
            qWarning() << "[viewport] Environment bake failed:" << hdrPath;
            return;
        }
        // The GPU upload runs here on the GUI thread; apply only the newest request that still has a
        // live renderer and a successful bake.
        if (requestId != m_environmentRequestId || !m_renderer || !baked) {
            return;
        }
        try {
            m_renderer->applyBakedEnvironment(*baked);
        } catch (const VulkanError& e) {
            // The upload allocates GPU objects, so its VK_CHECKs can throw inside this queued
            // slot — the same no-boundary hole renderFrame() closes. Disable the viewport
            // rather than aborting the app.
            failDevice("Environment upload", e);
            return;
        }
        m_appliedEnvironmentPath = hdrPath;
        m_appliedEnvironmentScale = baked->luminanceScale;
        // Auto-aim the key light at the environment's dominant light (its "sun"): in the PBR
        // mode the figure is visibly lit by the HDRI, so a key/shadow pointing anywhere else
        // reads as broken — the ground shadow must fall away from where the light comes from.
        // Only for USER-initiated HDRI switches (the startup bake keeps the authored default
        // dials — a fresh launch must show the LightingSettings defaults) and meaningfully
        // directional environments (an overcast sky has no sun); the azimuth/elevation dials
        // stay fully live for manual re-aiming afterwards, and the next HDRI switch re-aims
        // again. Not an undo entry — HDRI selection itself isn't one.
        if (autoAimKey && baked->dominantStrength >= 0.1f) {
            constexpr float kRadToDeg = 57.29577951f;
            const float elevation =
                std::asin(std::clamp(baked->dominantDir.y, -1.0f, 1.0f)) * kRadToDeg;
            LightingSettings aimed = m_deferred.lighting;
            aimed.keyAzimuthDeg = std::atan2(baked->dominantDir.x, baked->dominantDir.z) * kRadToDeg;
            // Keep the key above the horizon (a below-horizon "sun" — window-lit interiors —
            // can't cast a ground shadow) and below ~60°: a near-zenith light drops the shadow
            // straight under the figure where it reads as a smudge, not a shadow. 60° keeps a
            // legible cast direction while staying close to the environment's real sun.
            aimed.keyElevationDeg = std::clamp(elevation, 15.0f, 60.0f);
            setLightingSettings(aimed);
            emit lightingRestored(aimed); // the Environment panel syncs its az/el rows
            qDebug().nospace() << "[viewport] key light auto-aimed to environment sun (azimuth "
                               << aimed.keyAzimuthDeg << ", elevation " << aimed.keyElevationDeg
                               << ", directionality " << baked->dominantStrength << ")";
        }
        requestUpdate();
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
    m_deferred.environmentPath = hdrPath; // remembered so it survives a device loss / applies before first expose
    beginEnvironmentBake(hdrPath, /*autoAimKey=*/true); // no-op until the renderer exists (init kicks off the first bake)
    markProjectDirty(); // the environment is part of the document
}

void VulkanWindow::setLightingSettings(const LightingSettings& settings) {
    m_deferred.lighting = settings; // remembered so it applies before first expose / after a device loss
    withRenderer([&settings](VulkanRenderer& r) { r.scene().setLightingSettings(settings); });
}

} // namespace pose
