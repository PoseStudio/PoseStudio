/**
 * @file scenebuilder.h
 * @brief Turns the application's scene description (renderapi::Scene) into a Cycles scene.
 *
 * The one place that knows both vocabularies: the canonical material becomes a Principled BSDF
 * graph, a posed mesh a ccl::Mesh with the application's own normals and UV tangents, the
 * viewport's key light a sun, its environment and backdrop the world shader, its orbit camera
 * the Cycles camera. See scenebuilder.cpp for the mapping's rules.
 */

#ifndef POSESTUDIO_CYCLES_SCENEBUILDER_H
#define POSESTUDIO_CYCLES_SCENEBUILDER_H

#include "renderengineapi.h"

#include <string>

namespace ccl {
class Scene;
}

namespace posecycles {

/// Fills @p scene (a new Session's, still empty) from @p in for a render of @p settings. The
/// images of @p in are referenced, not copied: they are read when Cycles first synchronizes the
/// scene. False, with @p error set, if the description cannot be rendered.
bool buildScene(ccl::Scene* scene, const pose::renderapi::Scene& in, const pose::renderapi::Settings& settings,
                std::string& error);

} // namespace posecycles

#endif // POSESTUDIO_CYCLES_SCENEBUILDER_H
