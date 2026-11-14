/**
 * @file materialball.h
 * @brief The ball a saved material is pictured on: its mesh, and a small built-in shader for a
 *        copy of the application that has no render engine.
 *
 * A material file's icon (materialfile.h; the Materials tab's Save) is a picture of the
 * material on a ball under a fixed studio light — THE ICON STUDIO, makeIconStudio() below — so
 * that every icon of a library is lit alike whatever scene it was saved from and whatever
 * panoramas the user's library holds. TWO things make that picture:
 *
 * - THE RENDER ENGINE, where there is one (VulkanWindow::renderMaterialIcon): the ball of
 *   makeBallMesh() path traced with the zone's material and maps, exactly as a render of the
 *   scene would trace them — glass refracts, skin scatters, a coat lies over its base.
 * - shadeMaterialBall(), where there is none: the same ball shaded here, on the processor, in
 *   one pass per pixel. It is a real, if plain, physically based shader — the base layer
 *   (color, metalness, roughness, the dielectric's reflectance with their maps), a coat,
 *   emission, a see-through look for transmission — lit by the same studio, pre-blurred per
 *   roughness. It does not scatter, refract through the ball's volume, or read normal and bump
 *   maps: a stand-in, so that a saved material always has a picture, never the picture a
 *   render makes.
 *
 * Qt-free and device-free (glm + the scene's plain types).
 */

#ifndef MATERIALBALL_H
#define MATERIALBALL_H

#include "environment.h"
#include "lightingsettings.h"
#include "materialparams.h"
#include "modeldata.h" // DecodedImage

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace pose {

/// How much of the picture's half-width the ball's radius takes: the ball is 78 % as wide as its
/// icon, with a margin all round (a ball that touches the picture's edges reads as cropped in a
/// grid of thumbnails). ONE number for the rendered ball's camera and the built-in shader.
constexpr float kMaterialBallShare = 0.78f;

// --- THE ICON STUDIO ------------------------------------------------------------------------
// The room a material's ball is pictured in, MADE HERE rather than read from a panorama. A
// polished ball is a mirror of its surroundings: under a photographed room it shows that room —
// dim, grey and low in contrast, a chrome ball reads as fuzzy however well it is rendered — so
// the studio is drawn for what a ball should show, as a product photographer builds a tent:
//
//   - DARK WALLS on the camera's side: a mirror ball's middle goes black;
//   - a pale FLOOR whose edge lies on the horizon behind the ball and DIPS in front of it, so
//     the line between the dark and the light curves across the ball like a dome;
//   - DIMNESS BEHIND the ball, and DARK straight behind it: the floor fades toward the far
//     horizon and meets a dim wall there, and both go dark within a few degrees of straight
//     behind. A ball's rim mirrors what is straight behind it, colorlessly, so that is the
//     color of every ball's edge — and an icon lies on a DARK page: its edge is to fall into
//     the page, never to be a pale or grey ring round it (a white backdrop, the first studio,
//     drew a white one; a dim wall alone, the second, a grey one on a saturated metal);
//   - a large SOFTBOX above and to the left on the camera's side (the key: a matte ball's form,
//     a glossy one's highlight) and a thin STRIP to the right (a second, hard line).
//
// Crisp edges, three decades between the walls and the softbox, and neutral grey throughout: a
// material's own color is the only color in its icon. The radiances are ABSOLUTE — the studio is
// used as it is, never normalized like a panorama — and are balanced for iconStudioLighting()'s
// dials, under which a matte white ball is light without clipping.

/// The studio's radiance (grey: one value) along @p direction (need not be normalized), in the
/// icon's frame: the camera on +Z looking toward -Z, +X the picture's right, +Y up.
float iconStudioRadiance(const glm::vec3& direction);

/// The studio as an equirectangular image (EnvironmentImage's mapping), each texel the mean of
/// 2 × 2 samples of iconStudioRadiance. Deterministic.
EnvironmentImage makeIconStudio(int width = 2048, int height = 1024);

/// The dials an icon is made with: the studio at its own radiances and nothing else — no key
/// light (the softbox is the key), no fill — at exposure 1, tonemapped.
LightingSettings iconStudioLighting();

/// A UV sphere as a renderer takes it (the layout of Model::PosedMesh).
struct BallMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;      ///< u runs TWICE round the ball (0..2), v from the top (0) down (1)
    std::vector<glm::vec4> tangents; ///< along +u; w = the bitangent's handedness
    std::vector<uint32_t>  indices;  ///< counter-clockwise seen from outside
};

/// The ball: a sphere of @p radius about @p center, @p around segments round and @p down from
/// pole to pole, seen from +Z. u grows to the right in the picture and goes round TWICE, so a
/// square map keeps its proportions at the equator (a ball is twice as far round as it is from
/// pole to pole) instead of being stretched to double width. u = 0 is at the ball's left edge:
/// the map's own edges — where a map that does not tile shows a seam — fall on the silhouette,
/// left and right, and the MIDDLE of the map faces the camera.
BallMesh makeBallMesh(const glm::vec3& center, float radius, int around = 160, int down = 80);

/// The maps the built-in shader reads, as the zone has them (any may be null).
struct BallMaps {
    std::shared_ptr<const DecodedImage> baseColor; ///< sRGB
    std::shared_ptr<const DecodedImage> roughness; ///< its value in `roughnessChannel`
    std::shared_ptr<const DecodedImage> metalness;
    std::shared_ptr<const DecodedImage> specular;
    std::shared_ptr<const DecodedImage> emission;  ///< sRGB
    int roughnessChannel = 0;
    int metalnessChannel = 0;
    int specularChannel = 0;
};

/// The ball shaded on the processor (see the file comment): @p size × @p size pixels of RGBA8,
/// straight alpha, top row first — the ball's silhouette antialiased into the alpha, nothing
/// behind it. @p environment lights it as it is (the icon studio; a panorama already normalized)
/// with @p lighting's dials, exposure and tonemap. Deterministic.
std::vector<std::uint8_t> shadeMaterialBall(const MaterialParams& material, const BallMaps& maps,
                                            const EnvironmentImage& environment, const LightingSettings& lighting,
                                            int size);

} // namespace pose

#endif // MATERIALBALL_H
