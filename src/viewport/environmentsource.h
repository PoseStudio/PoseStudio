/**
 * @file environmentsource.h
 * @brief Loads an on-disk HDR panorama into an EnvironmentImage.
 *
 * This is the environment codec boundary. Like model textures (see ModelImportService), image
 * decoding is deliberately kept OUT of the Qt-free rendering core: this lives in the app-facing
 * layer, decodes the `.hdr` with stb_image, and hands the core a ready EnvironmentImage (linear
 * float RGB). The engine (scene/environment.*) then bakes the IBL from it, codec-free.
 */

#ifndef ENVIRONMENTSOURCE_H
#define ENVIRONMENTSOURCE_H

#include "environment.h" // EnvironmentImage

#include <optional>
#include <string>

namespace pose {

/// Loads a Radiance `.hdr` equirectangular panorama from @p path into an EnvironmentImage (linear
/// float RGB, row 0 = +Y pole). Returns nullopt if the file is missing or fails to decode.
std::optional<EnvironmentImage> loadHdrEnvironment(const std::string& path);

} // namespace pose

#endif // ENVIRONMENTSOURCE_H
