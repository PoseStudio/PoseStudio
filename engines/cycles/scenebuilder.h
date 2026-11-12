/**
 * @file scenebuilder.h
 * @brief Turns the application's scene descriptions (renderapi::Scene) into a Cycles scene.
 *
 * The one place that knows both vocabularies: the canonical material becomes a Principled BSDF
 * graph, a posed mesh a ccl::Mesh with the application's own normals and UV tangents, the
 * viewport's key light and its light objects Cycles's lights, its environment and backdrop the
 * world shader, its orbit camera
 * the Cycles camera. See scenebuilder.cpp for the mapping's rules.
 */

#ifndef POSESTUDIO_CYCLES_SCENEBUILDER_H
#define POSESTUDIO_CYCLES_SCENEBUILDER_H

#include "renderengineapi.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ccl {
class Geometry;
class Object;
class Scene;
class Shader;
} // namespace ccl

namespace posecycles {

/// Builds — and, for an interactive render, REBUILDS — one Cycles scene from the application's
/// descriptions of it. One per session: it remembers the shaders it made (Cycles never deletes
/// one), so a rebuild reuses them.
class SceneBuilder {
public:
    explicit SceneBuilder(ccl::Scene* scene);

    /// Makes the scene @p in for a render of @p settings, replacing whatever an earlier call
    /// built. The images of @p in are referenced, not copied: they are read when Cycles next
    /// synchronizes the scene. False, with @p error set, if the description cannot be rendered.
    /// (The caller holds the scene's mutex once its session renders.)
    bool build(const pose::renderapi::Scene& in, const pose::renderapi::Settings& settings, std::string& error);

    /// The camera alone (its framing and the image's size).
    void camera(const pose::renderapi::Camera& in, const pose::renderapi::Settings& settings);

    /// The LIGHTING alone, from @p in: its lights, its environment and backdrop, its shadow-
    /// catching floor — replacing the ones the last build() or lighting() made, the meshes and
    /// their materials left as they are. (A light being moved is this, many times a second.)
    void lighting(const pose::renderapi::Scene& in, const pose::renderapi::Settings& settings);

private:
    void clear();
    void clearLighting();
    void buildLighting(const pose::renderapi::Scene& in, const pose::renderapi::Settings& settings);

    ccl::Scene*               m_scene;
    std::vector<ccl::Shader*> m_materialShaders; // the pool: one per material of the largest scene so far
    std::size_t               m_idleFrom = 0;    // pool shaders from here on hold the idle graph
    ccl::Shader*              m_floorShader = nullptr;
    ccl::Shader*              m_lightShader = nullptr;
    // What buildLighting made (the lights and their objects, the floor): what lighting() replaces.
    std::vector<ccl::Object*>   m_lightingObjects;
    std::vector<ccl::Geometry*> m_lightingGeometry;
};

} // namespace posecycles

#endif // POSESTUDIO_CYCLES_SCENEBUILDER_H
