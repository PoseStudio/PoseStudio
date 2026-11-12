/**
 * @file rendersnapshot.h
 * @brief The viewport's scene, frozen for a render engine: the geometry as posed, the materials,
 *        the images they sample, the camera and the lighting — and the storage behind them.
 *
 * A render runs for seconds to minutes on the engine's own threads while the user goes on
 * posing; it must not read the live scene. VulkanWindow::captureRenderScene copies what a
 * render needs into one of these (the meshes skinned on the CPU as the vertex stage skins them —
 * Model::bakePosedMesh; the decoded images SHARED with the scene, not copied, since a
 * DecodedImage is immutable once a mesh carries it) and the description's pointers point into
 * it. Qt-free: std containers and the scene's own types.
 */

#ifndef RENDERSNAPSHOT_H
#define RENDERSNAPSHOT_H

#include "model.h"       // Model::PosedMesh
#include "modeldata.h"   // DecodedImage
#include "renderscene.h" // RenderSceneHolder

#include <glm/glm.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pose {

class RenderSnapshot final : public RenderSceneHolder {
public:
    const renderapi::Scene& scene() const override { return description; }
    View                    view() const override { return viewSettings; }
    int                     skippedSplatScenes() const override { return splatScenes; }
    std::shared_ptr<RenderSplatLayers> splatLayers(int width, int height) override {
        return splatSource ? splatSource(width, height) : nullptr;
    }

    /// Points `description` at the vectors below. Call once, after the last one is filled (a
    /// vector that grows afterwards moves, and the description would point at freed memory).
    void link() {
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            const Model::PosedMesh& g = geometry[i];
            renderapi::Mesh&        m = meshes[i];
            m.positions = g.positions.empty() ? nullptr : &g.positions[0].x;
            m.normals = g.normals.empty() ? nullptr : &g.normals[0].x;
            m.uvs = g.uvs.empty() ? nullptr : &g.uvs[0].x;
            m.tangents = g.tangents.empty() ? nullptr : &g.tangents[0].x;
            m.vertexCount = static_cast<uint32_t>(g.positions.size());
            m.indices = g.indices.data();
            m.indexCount = static_cast<uint32_t>(g.indices.size());
        }
        description.images = images.data();
        description.imageCount = static_cast<uint32_t>(images.size());
        description.materials = materials.data();
        description.materialCount = static_cast<uint32_t>(materials.size());
        description.meshes = meshes.data();
        description.meshCount = static_cast<uint32_t>(meshes.size());
        description.lights = lights.data();
        description.lightCount = static_cast<uint32_t>(lights.size());
    }

    // --- The storage (filled by the window; the description points into it after link()) ---
    std::deque<Model::PosedMesh>                     geometry;    // one per mesh (a deque: elements never move)
    std::deque<std::string>                          names;       // the C strings the description names things by
    std::vector<std::shared_ptr<const DecodedImage>> imageOwners; // the scene's own images, kept alive
    /// The procedural studio's pixels, when no panorama is loaded — SHARED with the window, which
    /// makes them once: an interactive render is handed many descriptions, and pixels at one
    /// address are one image to the engine (loaded once).
    std::shared_ptr<const std::vector<glm::vec3>>    environmentPixels;
    std::string                                      environmentFile;   // … or the panorama's path

    std::vector<renderapi::Image>    images;
    std::vector<renderapi::Material> materials;
    std::vector<renderapi::Mesh>     meshes;
    std::vector<renderapi::Light>    lights; // the key light (a sun), then the scene's light objects
    renderapi::Scene                 description;
    View                             viewSettings;
    int                              splatScenes = 0;
    /// Draws the scene's splats as a render's layers (the window's: captureSplatLayers) — set
    /// when the scene has a splat scene.
    std::function<std::shared_ptr<RenderSplatLayers>(int, int)> splatSource;
};

} // namespace pose

#endif // RENDERSNAPSHOT_H
