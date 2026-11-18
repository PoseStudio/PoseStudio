/**
 * @file vulkanwindow_project.cpp
 * @brief VulkanWindow's side of the DOCUMENT: File → New's scene reset, the `.pss` project file
 *        saved and opened (scene/projectfile.h), and the document's identity (path + unsaved flag).
 *
 * One of the translation units of VulkanWindow — see vulkanwindow.h for the map. A project file
 * REFERENCES every model by its import path and stores numbers: so saving is a capture of the
 * live scene into a ProjectDocument, and opening is parse → link pre-flight → resetToEmptyScene →
 * re-import each source and restore its transform, pose, pins and material edits → the
 * environment, the light objects and the camera. Nothing of the scene is touched until the whole
 * document has parsed and every source has been found (or skipped by the caller).
 *
 * The reader and writer of the file itself are Qt-free (scene/projectfile.{h,cpp}); the shell's
 * dialogs — the unsaved-changes prompt, the missing-file recovery — are MenuManager's.
 */

#include "vulkanwindow.h"

#include "importformats.h"
#include "rendering/vulkanrenderer.h"
#include "scene/armature.h"
#include "scene/model.h"
#include "scene/scene.h"

#include <QDebug>
#include <QFileInfo>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

// decompose() lives in the experimental gtx set — this TU opts in (it only feeds the .pss
// save path, so the flag's blast radius is one translation unit).
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pose {

void VulkanWindow::resetToEmptyScene() {
    if (!m_renderer) {
        return; // no renderer yet: a fresh launch already IS the empty scene
    }
    // 1. Close open pose edits FIRST — a settle or a fall would otherwise keep ticking past
    //    the reset and retarget at already-deleted models (the deleteModel bug).
    closeOpenPoseEdits();
    m_lightDrag = LightDrag{}; // (a marker's drag does not outlive the lights it indexed)
    // 2. Every model + selection + active figure (the device-wait guard lives in the renderer,
    //    as for deleteModel).
    m_renderer->clearModels();
    // 3. Camera → home framing (camera.reset + noteView(Home), the Home View / numpad 5 path).
    resetView();
    // 4. Deferred state → startup defaults: shade mode, skeleton, lighting dials, environment
    //    path, the import queue and the pending pose all drop.
    const bool skeletonWasShown = m_deferred.showSkeleton;
    m_deferred = DeferredSceneState{};
    // 5. The startup HDRI (default panorama, or the procedural studio) — off-thread, and with
    //    autoAimKey=false so the AUTHORED default dials win over a key light aimed at whatever
    //    environment was up before the New (design D2).
    beginEnvironmentBake(defaultEnvironmentPath(), /*autoAimKey=*/false);
    // 6. Push the deferred defaults into the scene's UBOs (shade mode / skeleton / lighting).
    m_deferred.applyTo(*m_renderer);
    //    … and say so where the shade mode was another (the strip's picker mirrors the signal;
    //    the Rendered mode's engine stops with its row).
    if (m_announcedShadeMode != m_deferred.shadeMode) {
        m_announcedShadeMode = m_deferred.shadeMode;
        emit shadeModeChanged(m_deferred.shadeMode);
    }
    //    … and likewise for the skeleton overlay (the View menu's entry and the strip's button
    //    mirror it: after an Open they went on showing it as on, with the overlay gone).
    if (skeletonWasShown != m_deferred.showSkeleton) {
        emit showSkeletonChanged(m_deferred.showSkeleton);
    }
    syncTracedMode();
    // 7. History cleared LAST — nothing inside New registers an entry (the pre-edit bracket is
    //    closed at step 1), and Ctrl+Z must not undo a New.
    m_undoStack.clear();
    m_redoStack.clear();
    // 8. Document identity: a new scene has no .pss path and nothing unsaved — Save must offer
    //    Save As (not silently overwrite the file that was open before), and the close prompt
    //    stays quiet. Last, so the camera reset above can't re-dirty it.
    setProjectPath(QString());
    setProjectClean();
    requestUpdate();
}

bool VulkanWindow::captureProjectDocument(ProjectDocument& out) const {
    if (!m_renderer) {
        return false;
    }
    out = ProjectDocument{};

    Scene& scn = scene();
    for (std::size_t i = 0; i < scn.modelCount(); ++i) {
        const Model* model = scn.modelAt(i);
        if (!model) {
            continue;
        }
        ProjectFigure fig;
        fig.source = model->sourcePath();

        // Split the snapshot rows: rotations -> pose, @trans: -> root translation, @pin: -> pins.
        glm::vec3 rootTranslation(0.0f);
        std::string rootBone;
        for (const auto& [name, value] : model->capturePose()) {
            if (name.compare(0, sizeof(kPosePinPrefix) - 1, kPosePinPrefix) == 0) {
                fig.pins.push_back(name.substr(sizeof(kPosePinPrefix) - 1));
            } else if (name.compare(0, sizeof(kPoseTranslationPrefix) - 1,
                           kPoseTranslationPrefix) == 0) {
                rootBone = name.substr(sizeof(kPoseTranslationPrefix) - 1);
                rootTranslation = value;
            } else {
                fig.pose.emplace_back(name, value);
            }
        }
        if (!rootBone.empty()) {
            fig.rootBone = std::move(rootBone);
            fig.rootTranslation = rootTranslation;
        }
        // The Materials tab's edits: the zones whose material differs from the imported one — the
        // values and the texture slots that were replaced or cleared.
        for (const Model::MaterialOverride& o : model->materialOverrides()) {
            ProjectMaterial pm;
            pm.zone = o.zone;
            pm.params = o.params;
            for (const auto& [slot, source] : o.textures) {
                pm.textures.push_back({slot, source});
            }
            fig.materials.push_back(std::move(pm));
        }

        // Decompose the model matrix into TRS (the document stores it channel-wise).
        glm::quat rotation;
        glm::vec3 skew;
        glm::vec4 perspective;
        if (glm::decompose(model->transform(), fig.scale, rotation, fig.translation, skew, perspective)) {
            fig.rotation = glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w);
        }
        out.figures.push_back(std::move(fig));
    }

    out.environment.hdri = m_deferred.environmentPath.toStdString();
    out.environment.settings = m_deferred.lighting;
    out.lights = m_deferred.lights;

    const Camera& cam = m_renderer->camera();
    out.camera.target = cam.target();
    out.camera.yaw = cam.yaw();
    out.camera.pitch = cam.pitch();
    out.camera.distance = cam.distance();
    out.camera.ortho = cam.orthographic();
    return true;
}

bool VulkanWindow::saveProjectFile(const QString& path) {
    closeOpenPoseEdits(); // the LANDED pose, not a transient mid-settle / mid-fall frame (as savePose)
    ProjectDocument doc;
    if (!captureProjectDocument(doc)) {
        return false;
    }
    return writeProjectFile(path.toStdString(), doc);
}

int VulkanWindow::loadProjectFile(const QString& path,
                                  const std::map<std::string, std::string>& recovered,
                                  std::vector<std::string>& missing, std::string& error) {
    if (!m_renderer) {
        // (For the log: a caller that can wait for the renderer asks rendererReady() first, as
        // the startup open of a project does.)
        error = "no renderer yet";
        return -1;
    }
    if (dragInFlight()) {
        error = tr("A drag is in flight: release the mouse first.").toStdString();
        return -1;
    }

    // 1. VALIDATE FIRST — the scene is untouched until the whole document has parsed.
    ProjectDocument doc;
    if (!readProjectFile(path.toStdString(), doc, error)) {
        return -1;
    }

    // 2. LINK RECOVERY PRE-FLIGHT — every figure's source must exist on disk (after applying the
    //    caller's re-pointed paths; a source the caller mapped to "" is SKIPPED: left out of the
    //    load, as the recovery dialog's Skip promises). Anything still missing is reported and the
    //    scene is left exactly as it was.
    auto resolve = [&recovered](const ProjectFigure& fig) -> std::string {
        const auto it = recovered.find(fig.source);
        return it != recovered.end() ? it->second : fig.source;
    };
    for (const ProjectFigure& fig : doc.figures) {
        const std::string src = resolve(fig);
        if (!src.empty() && !QFileInfo::exists(QString::fromStdString(src))) {
            missing.push_back(fig.source);
        }
    }
    if (!missing.empty()) {
        return 1;
    }

    // 3. RESET — a fresh scene to import into (New's body: models, camera home, deferred state,
    //    startup HDRI, history cleared).
    resetToEmptyScene();

    // 4. IMPORT + RESTORE in document order (the order the user had them). A source whose import
    //    adds nothing — a file that can no longer be read (its own import has said why), or one
    //    with nothing to show, which the scene refuses — is SKIPPED, like a missing file the user
    //    chose to leave out: a warning in the log, and the load goes on. The other models, the
    //    environment, the lights and the camera do not depend on it; stopped there, as it used
    //    to be, the scene stood half-loaded under the previous view and lighting, with no frame
    //    requested.
    for (const ProjectFigure& fig : doc.figures) {
        const std::string src = resolve(fig);
        if (src.empty()) {
            continue; // skipped in the recovery dialog
        }
        PendingImport import;
        import.path = QString::fromStdString(src);
        import.kind = ImportFormats::isSplatFile(import.path) ? PendingImport::Kind::Splat
                      : ImportFormats::isModelFile(import.path) ? PendingImport::Kind::Obj
                                                                : PendingImport::Kind::Figure;
        // (… or an import that reports success without adding a model: nothing to restore onto.)
        const std::size_t modelsBefore = scene().modelCount();
        Model* model = runImport(import, /*showProgress=*/true) && scene().modelCount() > modelsBefore
                           ? scene().modelAt(scene().modelCount() - 1)
                           : nullptr;
        if (model == nullptr) {
            qWarning().noquote() << "[project] left out of the load, its import added nothing:" << import.path;
            continue;
        }
        // Rebuild the saved TRS (translation · quaternion rotation · scale) and restore the pose
        // (rotations + root translation + pins).
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), fig.translation)
                                * glm::mat4_cast(glm::quat(fig.rotation.w, fig.rotation.x, fig.rotation.y,
                                                  fig.rotation.z))
                                * glm::scale(glm::mat4(1.0f), fig.scale);
        model->setTransform(transform);

        PoseSnapshot rows;
        for (const auto& [name, value] : fig.pose) {
            rows.emplace_back(name, value);
        }
        if (!fig.rootBone.empty()) {
            rows.emplace_back(std::string(kPoseTranslationPrefix) + fig.rootBone, fig.rootTranslation);
        }
        for (const std::string& pin : fig.pins) {
            rows.emplace_back(std::string(kPosePinPrefix) + pin, glm::vec3(1.0f, 0.0f, 0.0f));
        }
        model->applyPose(rows);
        // The saved material edits, by zone key (a zone the re-imported file no longer has is
        // left out, as an unknown pose row would be).
        const int modelIndex = static_cast<int>(scene().modelCount()) - 1;
        std::vector<std::pair<int, std::vector<MaterialOverlay>>> overlayStacks;
        for (const ProjectMaterial& m : fig.materials) {
            model->applyMaterialOverride(m.zone, m.params, &m.named);
            // … its overlays, painted once every slot below is in place (an overlay image that
            // is gone is left out of the composite, with a warning in the log; the stack keeps it).
            if (!m.params.overlays.empty()) {
                for (std::size_t i = 0; i < model->meshCount(); ++i) {
                    if (model->zoneKey(i) == m.zone) {
                        overlayStacks.emplace_back(static_cast<int>(i), m.params.overlays);
                    }
                }
            }
            // … and its texture slots: a replaced map decoded from its file (a file that is gone
            // leaves the import's own, with a warning in the log), a cleared one cleared.
            for (const ProjectTexture& t : m.textures) {
                MaterialMap map = MaterialMap::BaseColor;
                if (!materialMapByName(t.slot, map)) {
                    continue;
                }
                for (std::size_t i = 0; i < model->meshCount(); ++i) {
                    if (model->zoneKey(i) == m.zone) {
                        applySlotSource(modelIndex, static_cast<int>(i), map, t.source);
                    }
                }
            }
        }
        setModelOverlays(modelIndex, overlayStacks);
        scene().syncSmoothing(modelIndex); // (a zone whose saved smoothing is not the import's: its geometry follows)
    }

    // 5. ENVIRONMENT — the saved dials + HDRI (a no-op path keeps the startup bake, as does a
    //    saved panorama that is no longer on disk). The Environment tab is told both ways: its
    //    dials through lightingRestored (the undo path's signal), its HDRI caption through
    //    environmentRestored — without either it kept showing the values from before the Open,
    //    and the next scrub pushed those over the loaded ones.
    m_deferred.lighting = doc.environment.settings;
    const QString savedHdri = QString::fromStdString(doc.environment.hdri);
    if (!savedHdri.isEmpty() && QFileInfo::exists(savedHdri)) {
        m_deferred.environmentPath = savedHdri;
        beginEnvironmentBake(m_deferred.environmentPath, /*autoAimKey=*/false);
    } else if (!savedHdri.isEmpty()) {
        qWarning() << "[project] the saved panorama is missing, keeping the default:" << savedHdri;
    }
    m_deferred.applyTo(*m_renderer);
    applyLights(doc.lights); // the light objects (the frame's notify tells the Lights tab)
    emit lightingRestored(m_deferred.lighting);
    emit environmentRestored(m_deferred.environmentPath.isEmpty() ? defaultEnvironmentPath()
                                                                   : m_deferred.environmentPath);

    // 6. CAMERA — the saved framing (target + orbit angles + distance + projection).
    m_renderer->camera().restoreFraming(doc.camera.target, doc.camera.yaw, doc.camera.pitch,
                                       doc.camera.distance);
    m_renderer->camera().setOrthographic(doc.camera.ortho);
    noteView(ViewPreset::Free);

    setProjectPath(path);
    setProjectClean();
    requestUpdate();
    return 0;
}

QString VulkanWindow::projectPath() const { return m_project.path(); }
void VulkanWindow::setProjectPath(const QString& path) { m_project.setPath(path); }

bool VulkanWindow::isProjectDirty() const { return m_project.dirty(); }
void VulkanWindow::markProjectDirty() { m_project.markDirty(); }
void VulkanWindow::setProjectClean() { m_project.setClean(); }

} // namespace pose
