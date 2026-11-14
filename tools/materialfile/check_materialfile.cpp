/**
 * @file check_materialfile.cpp
 * @brief The saved material's guard (CTest `material-file-check`): the `.psm` codec
 *        (scene/materialfile.h), and the ball a material is pictured on with the studio that
 *        lights it (scene/materialball.h).
 *
 * THE FILE, as text — no disk, no Qt:
 *   - a material with every kind of value set, and maps, reads back equal;
 *   - overlays are never written, and one found in a file is dropped;
 *   - what is not a material is rejected WHOLE with a reason: not JSON, another document (a
 *     project file), no version, a newer version, a value of the wrong type or not finite;
 *   - an unknown field and an unknown slot are passed over (a newer build's file still reads);
 *   - an absent value keeps its default.
 * THE ICON STUDIO (what every icon is lit by; the reason a chrome ball reads as chrome):
 *   - what a mirror ball shows of it: dark on the camera's side, a softbox above and to the
 *     left, a thin strip to the right, a pale floor whose edge dips in front of the ball, a
 *     white backdrop behind it;
 *   - its image: the size asked for, neutral grey, finite, three decades of contrast, the
 *     function the right way round, the same twice;
 *   - its exposure, through the built-in shader: a matte white ball light and not blown out, a
 *     chrome ball black beside white.
 * THE BALL:
 *   - the mesh a render engine traces: on the sphere, unit normals pointing outward, triangles
 *     wound counter-clockwise from outside (a path tracer and the viewport both mind), the
 *     middle of the map at the front, tangents along +u with the handedness that puts +v down;
 *   - the built-in shader a copy without an engine uses: a square picture, transparent round
 *     the ball and opaque on it with an antialiased edge, the base color showing, a metal
 *     unlike a dielectric, a rough ball unlike a polished one, emission adding light, a base
 *     color map read at the front, opacity in the alpha, and the same picture twice.
 *
 * Exit code = number of failed checks (0 = green).
 */

#include "materialball.h"
#include "materialfile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace pose;

namespace {

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

bool approx(float a, float b, float eps = 1.0e-5f) {
    return std::abs(a - b) <= eps;
}

bool rejects(const std::string& text, std::string* why = nullptr) {
    MaterialDocument doc;
    doc.name = "left over";
    std::string error;
    const bool  ok = readMaterialFileText(text, doc, error);
    if (why != nullptr) {
        *why = error;
    }
    return !ok && !error.empty() && doc.name.empty() && doc.textures.empty();
}

void checkFile() {
    std::printf("-- the .psm file\n");
    MaterialDocument doc;
    doc.name = "Brushed Gold";
    MaterialParams& p = doc.params;
    p.baseColor = glm::vec3(0.9f, 0.7f, 0.3f);
    p.roughness = 0.35f;
    p.metalness = 1.0f;
    p.specular = 0.8f;
    p.ior = 1.45f;
    p.anisotropy = 0.6f;
    p.anisotropyRotation = 0.25f;
    p.topCoat = 0.4f;
    p.topCoatRoughness = 0.1f;
    p.coatColor = glm::vec3(1.0f, 0.9f, 0.8f);
    p.transmission = 0.2f;
    p.transmissionColor = glm::vec3(0.5f, 0.8f, 0.6f);
    p.transmissionDepth = 0.02f;
    p.transmissionRoughness = 0.05f;
    p.thinWalled = true;
    p.sheen = 0.3f;
    p.sheenColor = glm::vec3(0.8f, 0.2f, 0.4f);
    p.emission = 2.0f;
    p.emissionColor = glm::vec3(1.0f, 0.5f, 0.1f);
    p.opacity = 0.75f;
    p.normalStrength = 1.5f;
    p.uvTiling = glm::vec2(3.0f, 2.0f);
    p.uvOffset = glm::vec2(0.25f, 0.5f);
    p.subsurfaceEnabled = false;
    p.detailEnabled = false;
    doc.textures = {{"basecolor", "maps/gold_color.png"},
                    {"roughness", "D:/Textures/gold rough.png"},
                    {"normal", "maps/gold_normal.png"}};

    const std::string text = materialFileText(doc);
    check(text.find("\"posestudio.material\"") != std::string::npos && text.find("\"version\": 1") != std::string::npos,
          "the text carries the format marker and version 1");

    MaterialDocument back;
    std::string      error;
    check(readMaterialFileText(text, back, error) && error.empty(), "a written material reads back");
    check(back.name == "Brushed Gold", "its name comes back");
    check(back.params.sameValues(doc.params), "every value comes back equal");
    check(approx(back.params.anisotropy, 0.6f) && approx(back.params.transmissionRoughness, 0.05f) && back.params.thinWalled &&
              !back.params.subsurfaceEnabled && !back.params.detailEnabled && approx(back.params.uvTiling.x, 3.0f) &&
              approx(back.params.uvOffset.y, 0.5f) && approx(back.params.emissionColor.y, 0.5f),
          "… a value, a flag, a pair and a color among them, each as written");
    check(back.textures.size() == 3 && back.textures[0].slot == "basecolor" && back.textures[0].source == "maps/gold_color.png" &&
              back.textures[1].source == "D:/Textures/gold rough.png" && back.textures[2].slot == "normal",
          "the maps come back in order, their sources as written (relative and absolute alike)");
    check(materialFileText(back) == text, "written again, the text is the same");

    // Overlays are an object's, never a material file's.
    MaterialDocument tattooed = doc;
    MaterialOverlay  overlay;
    overlay.name = "Tattoo";
    overlay.color = "D:/tats/dragon.png";
    tattooed.params.overlays.push_back(overlay);
    const std::string tattooedText = materialFileText(tattooed);
    check(tattooedText.find("dragon") == std::string::npos && tattooedText == text, "an overlay is not written");
    std::string withOverlay = text;
    const std::string anchor = "\"material\": {";
    withOverlay.insert(withOverlay.find(anchor) + anchor.size(), "\n    \"overlays\": [ { \"name\": \"Tattoo\", \"color\": \"x.png\" } ],");
    MaterialDocument dropped;
    check(readMaterialFileText(withOverlay, dropped, error) && dropped.params.overlays.empty() &&
              dropped.params.sameValues(doc.params),
          "an overlay found in a file is dropped, the material read all the same");

    // THE SMOOTHING (the Materials tab's Smooth by Angle) is an object's too: how a zone's faces
    // are shaded is neither a material file's to carry nor to change.
    MaterialDocument smoothed = doc;
    smoothed.params.smoothByAngle = !doc.params.smoothByAngle;
    smoothed.params.smoothAngle = 35.0f;
    const std::string smoothedText = materialFileText(smoothed);
    check(smoothedText == text && smoothedText.find("smooth") == std::string::npos, "the smoothing is not written");
    std::string withSmoothing = text;
    withSmoothing.insert(withSmoothing.find(anchor) + anchor.size(), "\n    \"smoothByAngle\": true,\n    \"smoothAngle\": 35.0,");
    MaterialDocument unsmoothed;
    check(readMaterialFileText(withSmoothing, unsmoothed, error) && !unsmoothed.params.smoothByAngle &&
              unsmoothed.params.smoothAngle == pose::MaterialParams().smoothAngle && unsmoothed.params.sameValues(doc.params),
          "a smoothing found in a file is not taken, the material read all the same");

    // What is not a material.
    std::string why;
    check(rejects("this is not json", &why), "text that is not JSON is rejected, with a reason");
    check(rejects("[1, 2, 3]"), "JSON that is not an object is rejected");
    check(rejects("{ \"format\": \"posestudio.project\", \"version\": 1, \"figures\": [] }"), "a project file is rejected");
    check(rejects("{ \"format\": \"posestudio.material\", \"material\": {} }"), "a material with no version is rejected");
    check(rejects("{ \"format\": \"posestudio.material\", \"version\": 2, \"material\": {} }", &why) &&
              why.find("newer") != std::string::npos,
          "a newer version is rejected, and the reason says so");
    check(rejects("{ \"format\": \"posestudio.material\", \"version\": 1 }"), "a material file without a material is rejected");
    check(rejects("{ \"format\": \"posestudio.material\", \"version\": 1, \"material\": { \"roughness\": \"shiny\" } }"),
          "a value of the wrong type is rejected");
    check(rejects("{ \"format\": \"posestudio.material\", \"version\": 1, \"material\": { \"baseColor\": [1, 2] } }"),
          "a color of two numbers is rejected");
    check(rejects("{ \"format\": \"posestudio.material\", \"version\": 1, \"material\": { \"textures\": [ { \"slot\": 7 } ] } }"),
          "a malformed map entry is rejected");

    // What a newer build may add.
    MaterialDocument tolerant;
    check(readMaterialFileText("{ \"format\": \"posestudio.material\", \"version\": 1, \"name\": \"Plain\", \"author\": \"someone\", "
                               "\"material\": { \"roughness\": 0.2, \"shimmer\": 5, "
                               "\"textures\": [ { \"slot\": \"hologram\", \"source\": \"h.png\" }, { \"slot\": \"basecolor\", \"source\": \"c.png\" } ] } }",
                               tolerant, error),
          "an unknown field and an unknown slot do not reject a file");
    check(tolerant.name == "Plain" && approx(tolerant.params.roughness, 0.2f) && tolerant.textures.size() == 1 &&
              tolerant.textures[0].slot == "basecolor",
          "… what is known is read, the unknown slot passed over");
    const MaterialParams fresh;
    check(approx(tolerant.params.metalness, fresh.metalness) && approx(tolerant.params.ior, fresh.ior) &&
              tolerant.params.baseEnabled == fresh.baseEnabled,
          "a value the file leaves out keeps its default");
}

void checkMesh() {
    std::printf("-- the ball's mesh\n");
    const glm::vec3 center(0.5f, 1.0f, -2.0f);
    const float     radius = 0.25f;
    const BallMesh  ball = makeBallMesh(center, radius, 64, 32);
    const std::size_t count = ball.positions.size();
    check(count == 65u * 33u && ball.normals.size() == count && ball.uvs.size() == count && ball.tangents.size() == count,
          "one position, normal, uv and tangent a vertex");
    check(ball.indices.size() == 64u * 32u * 6u - 2u * 64u * 3u && ball.indices.size() % 3 == 0,
          "two triangles a cell, one in the rows at the poles");

    bool onSphere = true, unit = true, tangentOk = true, inRange = true;
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 fromCenter = ball.positions[i] - center;
        onSphere = onSphere && approx(glm::length(fromCenter), radius, 1.0e-5f);
        unit = unit && approx(glm::length(ball.normals[i]), 1.0f, 1.0e-5f) && glm::dot(fromCenter, ball.normals[i]) > 0.0f;
        const glm::vec3 t(ball.tangents[i]);
        tangentOk = tangentOk && approx(glm::length(t), 1.0f, 1.0e-5f) && approx(glm::dot(t, ball.normals[i]), 0.0f, 1.0e-5f) &&
                    ball.tangents[i].w == -1.0f;
        inRange = inRange && ball.uvs[i].x >= 0.0f && ball.uvs[i].x <= 2.0f && ball.uvs[i].y >= 0.0f && ball.uvs[i].y <= 1.0f;
    }
    check(onSphere, "every vertex lies on the sphere");
    check(unit, "every normal is of unit length and points outward");
    check(tangentOk, "every tangent is of unit length, square to its normal, handedness -1");
    check(inRange, "u runs 0..2 and v 0..1");

    bool outward = true, uvForward = true;
    for (std::size_t t = 0; t + 2 < ball.indices.size(); t += 3) {
        const uint32_t  a = ball.indices[t], b = ball.indices[t + 1], c = ball.indices[t + 2];
        const glm::vec3 face = glm::cross(ball.positions[b] - ball.positions[a], ball.positions[c] - ball.positions[a]);
        const glm::vec3 middle = (ball.positions[a] + ball.positions[b] + ball.positions[c]) / 3.0f - center;
        outward = outward && glm::dot(face, middle) > 0.0f;
        // The map is wound the same way in every triangle — no flipped or folded piece of it. (With
        // v running DOWN the ball, a triangle counter-clockwise in space is clockwise in the map.)
        const glm::vec2 e1 = ball.uvs[b] - ball.uvs[a], e2 = ball.uvs[c] - ball.uvs[a];
        uvForward = uvForward && (e1.x * e2.y - e1.y * e2.x) < 0.0f;
    }
    check(outward, "every triangle is wound counter-clockwise seen from outside");
    check(uvForward, "the map is laid the same way round in every triangle");

    // Where the map's middle is: the front vertex (+Z) on the equator carries u = 0.5.
    bool frontFound = false, leftFound = false, topDown = true;
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 n = ball.normals[i];
        if (approx(n.z, 1.0f, 1.0e-5f)) {
            frontFound = approx(ball.uvs[i].x, 0.5f, 1.0e-4f) && approx(ball.uvs[i].y, 0.5f, 1.0e-4f) &&
                         approx(ball.tangents[i].x, 1.0f, 1.0e-4f);
        }
        if (approx(n.x, -1.0f, 1.0e-5f) && ball.uvs[i].x < 1.0f) {
            leftFound = approx(ball.uvs[i].x, 0.0f, 1.0e-4f);
        }
        topDown = topDown && approx(ball.uvs[i].y, std::acos(glm::clamp(n.y, -1.0f, 1.0f)) / 3.14159265f, 1.0e-4f);
    }
    check(frontFound, "the middle of the map (u 0.5, v 0.5) faces the camera, +u to the right");
    check(leftFound, "the map's edge (u 0) is at the ball's left side, on the silhouette");
    check(topDown, "v runs from the top of the ball down");
}

struct Pixel {
    int r, g, b, a;
};

Pixel at(const std::vector<std::uint8_t>& image, int size, float xShare, float yShare) {
    const int x = std::min(static_cast<int>(xShare * static_cast<float>(size)), size - 1);
    const int y = std::min(static_cast<int>(yShare * static_cast<float>(size)), size - 1);
    const std::uint8_t* p = &image[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4u];
    return {p[0], p[1], p[2], p[3]};
}

double meanLevel(const std::vector<std::uint8_t>& image) {
    double      sum = 0.0;
    std::size_t covered = 0;
    for (std::size_t i = 0; i + 3 < image.size(); i += 4) {
        if (image[i + 3] == 255) {
            sum += (image[i] + image[i + 1] + image[i + 2]) / 3.0;
            ++covered;
        }
    }
    return covered > 0 ? sum / static_cast<double>(covered) : 0.0;
}

/// How much the picture varies from pixel to its right neighbour on the ball: a polished ball
/// mirrors the studio's detail, a rough one smooths it away.
double detail(const std::vector<std::uint8_t>& image, int size) {
    double      sum = 0.0;
    std::size_t n = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x + 1 < size; ++x) {
            const std::uint8_t* p = &image[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4u];
            if (p[3] == 255 && p[7] == 255) {
                sum += std::abs(p[0] - p[4]) + std::abs(p[1] - p[5]) + std::abs(p[2] - p[6]);
                ++n;
            }
        }
    }
    return n > 0 ? sum / static_cast<double>(n) : 0.0;
}

/// A direction of the icon's frame: @p azimuth from +Z (the camera's side) toward +X (the
/// picture's right), @p elevation up from the horizon, in degrees.
glm::vec3 studioDirection(float azimuth, float elevation) {
    const float az = glm::radians(azimuth), el = glm::radians(elevation);
    return glm::vec3(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az));
}

/// The share of the ball's opaque pixels whose grey level is under @p below (or, with
/// @p below < 0, over @p above).
double shareOfBall(const std::vector<std::uint8_t>& image, int below, int above) {
    std::size_t covered = 0, counted = 0;
    for (std::size_t i = 0; i + 3 < image.size(); i += 4) {
        if (image[i + 3] != 255) {
            continue;
        }
        ++covered;
        const int level = (image[i] + image[i + 1] + image[i + 2]) / 3;
        counted += (below >= 0 ? level < below : level > above) ? 1 : 0;
    }
    return covered > 0 ? static_cast<double>(counted) / static_cast<double>(covered) : 0.0;
}

void checkStudio() {
    std::printf("-- the icon studio\n");
    const auto at = [](float azimuth, float elevation) { return iconStudioRadiance(studioDirection(azimuth, elevation)); };

    // What a mirror ball shows of it.
    check(at(0.0f, 0.0f) < 0.05f && at(20.0f, 10.0f) < 0.05f && at(40.0f, -20.0f) < 0.05f && at(0.0f, 90.0f) < 0.05f,
          "the camera's side and the ceiling are dark: a mirror ball's middle is black");
    check(at(-40.0f, 54.0f) > 5.0f && at(-40.0f, 54.0f) < 10.0f, "a softbox above and to the left, on the camera's side");
    check(at(62.0f, 38.0f) > 5.0f && at(70.0f, 38.0f) < 0.05f && at(54.0f, 38.0f) < 0.05f && at(62.0f, 55.0f) > 5.0f,
          "a strip of light to the right, tall and thin");
    check(at(0.0f, -90.0f) > 0.5f && at(180.0f, -60.0f) > 0.5f && at(0.0f, -60.0f) > 0.5f && at(90.0f, -40.0f) > 0.5f, "a pale floor below");
    check(at(0.0f, -40.0f) < 0.05f && at(90.0f, -15.0f) < 0.05f, "its edge dipping in front of the ball: the dark dome a mirror ball shows");
    // BEHIND the ball the room falls away into dimness: what is there is the color of every
    // ball's edge, and an icon lies on a dark page.
    check(at(180.0f, -80.0f) > 0.7f && at(180.0f, -80.0f) > at(180.0f, -45.0f) && at(180.0f, -45.0f) > at(180.0f, -15.0f) &&
              at(180.0f, -15.0f) > at(180.0f, -2.0f),
          "the floor fades toward the far horizon");
    check(at(180.0f, 0.0f) < 0.03f && at(180.0f, 8.0f) < 0.03f && at(180.0f, -8.0f) < 0.03f && at(172.0f, 0.0f) < 0.03f && at(-172.0f, 0.0f) < 0.03f,
          "DARK straight behind the ball: what its very rim mirrors, whatever it is made of");
    check(at(125.0f, 2.0f) > 0.1f && at(125.0f, 2.0f) < 0.25f && approx(at(-125.0f, 2.0f), at(125.0f, 2.0f), 0.01f) &&
              at(125.0f, 2.0f) > at(150.0f, 2.0f) && at(150.0f, 2.0f) > at(165.0f, 2.0f) && at(180.0f, 45.0f) < 0.05f && at(90.0f, 2.0f) < 0.05f,
          "a DIM wall to either side of that, from the floor up, coming in smoothly: neither white nor a line");
    check(approx(at(180.0f, -2.0f), at(180.0f, 2.0f), 0.01f) && approx(at(150.0f, -2.0f), at(150.0f, 2.0f), 0.02f),
          "the floor runs into that wall with no line between them");
    check(iconStudioRadiance(glm::vec3(0.0f, 0.0f, 5.0f)) == at(0.0f, 0.0f) && std::isfinite(iconStudioRadiance(glm::vec3(0.0f))),
          "a direction need not be normalized, and none at all is no crash");

    // Its image.
    const EnvironmentImage image = makeIconStudio(512, 256);
    bool  grey = true, finite = true;
    float lowest = 1.0e9f, highest = 0.0f;
    for (const glm::vec3& p : image.pixels) {
        grey = grey && p.r == p.g && p.g == p.b;
        finite = finite && std::isfinite(p.r) && p.r >= 0.0f;
        lowest = std::min(lowest, p.r);
        highest = std::max(highest, p.r);
    }
    check(image.width == 512 && image.height == 256 && image.pixels.size() == 512u * 256u, "its image is the size asked for");
    check(grey && finite, "neutral grey and finite throughout");
    check(lowest > 0.0f && highest / lowest > 300.0f, "with more than two decades between the walls and the softbox");
    const auto shown = [&](float azimuth, float elevation) { return image.sample(studioDirection(azimuth, elevation)).r; };
    check(approx(shown(-40.0f, 54.0f), at(-40.0f, 54.0f), 0.05f) && approx(shown(62.0f, 38.0f), at(62.0f, 38.0f), 0.3f) &&
              approx(shown(0.0f, 0.0f), at(0.0f, 0.0f), 0.01f) && approx(shown(180.0f, 0.0f), at(180.0f, 0.0f), 0.2f) &&
              approx(shown(0.0f, -80.0f), at(0.0f, -80.0f), 0.05f) && shown(40.0f, 54.0f) < 0.05f,
          "the image shows the studio the right way round (the softbox to the LEFT)");
    check(makeIconStudio(512, 256).pixels == image.pixels, "the same studio twice");
    const auto              start = std::chrono::steady_clock::now();
    const EnvironmentImage  full = makeIconStudio();
    const double            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("     (the studio at %d x %d, as an icon's render is handed it: made in %.0f ms)\n", full.width, full.height, ms);

    // Its dials, and what they make of a ball (the built-in shader's picture).
    const LightingSettings lighting = iconStudioLighting();
    check(lighting.keyIntensity == 0.0f && lighting.ambientFill == 0.0f && lighting.diffuseIntensity == 1.0f && lighting.exposure == 1.0f &&
              lighting.tonemap && lighting.environmentRotationDeg == 0.0f,
          "an icon's dials: the studio as it is and nothing else, tonemapped");
    const BallMaps none;
    const int      size = 128;
    MaterialParams matte;
    matte.baseColor = glm::vec3(0.8f);
    matte.roughness = 0.9f;
    const std::vector<std::uint8_t> white = shadeMaterialBall(matte, none, image, lighting, size);
    std::printf("     (a matte white ball: mean level %.0f, %.1f %% of it over 250, %.1f %% under 60)\n", meanLevel(white),
                shareOfBall(white, -1, 250) * 100.0, shareOfBall(white, 60, 0) * 100.0);
    check(meanLevel(white) > 120.0 && meanLevel(white) < 235.0 && shareOfBall(white, -1, 250) < 0.02 && shareOfBall(white, 60, 0) < 0.10,
          "a matte white ball is light, not blown out, and has no black side");
    MaterialParams chrome;
    chrome.baseColor = glm::vec3(0.549f, 0.556f, 0.554f);
    chrome.metalness = 1.0f;
    chrome.roughness = 0.05f;
    const std::vector<std::uint8_t> mirror = shadeMaterialBall(chrome, none, image, lighting, size);
    std::printf("     (a chrome ball: %.0f %% of it under 30, %.0f %% over 180; pixel-to-pixel detail %.1f)\n", shareOfBall(mirror, 30, 0) * 100.0,
                shareOfBall(mirror, -1, 180) * 100.0, detail(mirror, size));
    check(shareOfBall(mirror, 30, 0) > 0.25 && shareOfBall(mirror, -1, 180) > 0.15, "a chrome ball is black beside white");

    // The ball's EDGE (5 % of its radius in: the ball is kMaterialBallShare of the picture).
    const float inset = 0.5f * kMaterialBallShare * 0.95f;
    const Pixel left = ::at(mirror, size, 0.5f - inset, 0.5f), right = ::at(mirror, size, 0.5f + inset, 0.5f);
    const Pixel bottom = ::at(mirror, size, 0.5f, 0.5f + inset), floorShown = ::at(mirror, size, 0.5f, 0.5f + 0.5f * kMaterialBallShare * 0.75f);
    MaterialParams gold = chrome;
    gold.baseColor = glm::vec3(1.0f, 0.766f, 0.336f);
    const std::vector<std::uint8_t> golden = shadeMaterialBall(gold, none, image, lighting, size);
    const Pixel goldLeft = ::at(golden, size, 0.5f - inset, 0.5f), goldBottom = ::at(golden, size, 0.5f, 0.5f + inset);
    std::printf("     (a chrome ball's edge: left %d, right %d, bottom %d against %d further in; a gold ball's: left %d %d %d, bottom %d %d %d)\n", left.r,
                right.r, bottom.r, floorShown.r, goldLeft.r, goldLeft.g, goldLeft.b, goldBottom.r, goldBottom.g, goldBottom.b);
    check(left.a == 255 && right.a == 255 && bottom.a == 255 && left.r > 30 && left.r < 160 && right.r > 30 && right.r < 160,
          "a mirror ball's edge is dim, not white: no pale rim on a dark page (and not lost in it either)");
    check(bottom.r < floorShown.r - 15, "... and darker at its lower edge than the floor it shows further in");
    check(goldLeft.r < 190 && goldLeft.r - goldLeft.b > 25 && goldBottom.r - goldBottom.b > 25, "a gold ball's edge is gold, not whitish");

    // ... and its very RIM (3 % in: the outer pixels at an icon's size), on a SATURATED metal. At
    // that grazing angle a metal reflects without color, so whatever light stands straight
    // behind the ball comes back GREY: a yellow metal has no blue of its own, and its blue there
    // is that grey. (0.16 of wall straight behind drew a grey ring round every colored ball.)
    MaterialParams yellow = chrome;
    yellow.baseColor = glm::vec3(1.0f, 0.78f, 0.0f);
    const std::vector<std::uint8_t> lemon = shadeMaterialBall(yellow, none, image, lighting, size);
    const float rim = 0.5f * kMaterialBallShare * 0.97f;
    const Pixel rimLeft = ::at(lemon, size, 0.5f - rim, 0.5f), rimRight = ::at(lemon, size, 0.5f + rim, 0.5f);
    const Pixel rimTop = ::at(lemon, size, 0.5f, 0.5f - rim), rimBottom = ::at(lemon, size, 0.5f, 0.5f + rim);
    const Pixel chromeRim = ::at(mirror, size, 0.5f - rim, 0.5f), chromeTop = ::at(mirror, size, 0.5f, 0.5f - rim);
    std::printf("     (a yellow metal's rim: left %d %d %d, right %d %d %d, top %d %d %d, bottom %d %d %d; a chrome ball's: left %d, top %d)\n", rimLeft.r,
                rimLeft.g, rimLeft.b, rimRight.r, rimRight.g, rimRight.b, rimTop.r, rimTop.g, rimTop.b, rimBottom.r, rimBottom.g, rimBottom.b, chromeRim.r,
                chromeTop.r);
    check(rimLeft.a == 255 && rimTop.a == 255 && rimBottom.a == 255 && rimLeft.b < 40 && rimRight.b < 40 && rimTop.b < 40 && rimBottom.b < 40,
          "a saturated metal's rim carries no grey: nothing stands straight behind the ball to come back colorless");
    check(chromeRim.r < 70 && chromeTop.r < 70 && rimTop.r < 70, "... and falls into the dark page: no ring round the ball's top");
}

void checkShader() {
    std::printf("-- the built-in ball shader\n");
    const EnvironmentImage studio = makeIconStudio(1024, 512);
    const LightingSettings lighting = iconStudioLighting();
    const BallMaps         none;
    const int              size = 96;

    MaterialParams red;
    red.baseColor = glm::vec3(0.8f, 0.05f, 0.05f);
    red.roughness = 0.5f;
    const std::vector<std::uint8_t> plastic = shadeMaterialBall(red, none, studio, lighting, size);
    check(plastic.size() == static_cast<std::size_t>(size) * size * 4u, "the picture is size x size RGBA");
    check(at(plastic, size, 0.0f, 0.0f).a == 0 && at(plastic, size, 0.99f, 0.0f).a == 0 && at(plastic, size, 0.0f, 0.99f).a == 0 &&
              at(plastic, size, 0.99f, 0.99f).a == 0,
          "the corners are transparent");
    check(at(plastic, size, 0.5f, 0.5f).a == 255 && at(plastic, size, 0.5f, 0.17f).a == 255 && at(plastic, size, 0.17f, 0.5f).a == 255 &&
              at(plastic, size, 0.83f, 0.5f).a == 255 && at(plastic, size, 0.5f, 0.83f).a == 255,
          "the ball is opaque");
    check(at(plastic, size, 0.5f, 0.05f).a == 0 && at(plastic, size, 0.05f, 0.5f).a == 0 && at(plastic, size, 0.95f, 0.5f).a == 0 &&
              at(plastic, size, 0.5f, 0.95f).a == 0,
          "there is a margin round it: the middles of the picture's edges are clear");
    int partial = 0;
    for (std::size_t i = 3; i < plastic.size(); i += 4) {
        partial += plastic[i] > 0 && plastic[i] < 255 ? 1 : 0;
    }
    check(partial > size, "its edge is antialiased (pixels of partial coverage round it)");
    const Pixel middle = at(plastic, size, 0.5f, 0.5f);
    check(middle.r > middle.g + 40 && middle.r > middle.b + 40, "a red material is red");
    check(shadeMaterialBall(red, none, studio, lighting, size) == plastic, "the same material twice is the same picture");

    MaterialParams metal = red;
    metal.metalness = 1.0f;
    metal.roughness = 0.15f;
    const std::vector<std::uint8_t> polished = shadeMaterialBall(metal, none, studio, lighting, size);
    check(polished != plastic, "a metal is not the dielectric's picture");
    const Pixel metalMiddle = at(polished, size, 0.5f, 0.5f);
    check(metalMiddle.r >= metalMiddle.g && metalMiddle.r >= metalMiddle.b, "a metal's reflections take its color");
    MaterialParams rough = metal;
    rough.roughness = 0.9f;
    check(detail(polished, size) > detail(shadeMaterialBall(rough, none, studio, lighting, size), size) * 1.2,
          "a polished ball shows more of the studio than a rough one");

    MaterialParams glowing = red;
    glowing.emission = 1.5f;
    glowing.emissionColor = glm::vec3(1.0f);
    check(meanLevel(shadeMaterialBall(glowing, none, studio, lighting, size)) > meanLevel(plastic) + 10.0, "emission adds light");
    glowing.emissionEnabled = false;
    check(shadeMaterialBall(glowing, none, studio, lighting, size) == plastic, "… and none with its layer switched off");

    MaterialParams faint = red;
    faint.opacity = 0.5f;
    const int faintAlpha = at(shadeMaterialBall(faint, none, studio, lighting, size), size, 0.5f, 0.5f).a;
    check(faintAlpha >= 126 && faintAlpha <= 129, "the opacity is in the alpha");

    // A base color map: left half green, right half blue; the map's middle faces the camera.
    auto map = std::make_shared<DecodedImage>();
    map->width = 8;
    map->height = 2;
    map->pixels.resize(8u * 2u * 4u);
    for (std::uint32_t y = 0; y < 2; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            std::uint8_t* p = &map->pixels[(y * 8u + x) * 4u];
            p[0] = 0;
            p[1] = x < 4 ? 255 : 0;
            p[2] = x < 4 ? 0 : 255;
            p[3] = 255;
        }
    }
    MaterialParams white;
    white.roughness = 0.6f;
    BallMaps mapped;
    mapped.baseColor = map;
    const std::vector<std::uint8_t> painted = shadeMaterialBall(white, mapped, studio, lighting, size);
    const Pixel                     left = at(painted, size, 0.3f, 0.5f), right = at(painted, size, 0.7f, 0.5f);
    check(left.g > left.b + 40 && right.b > right.g + 40, "a base color map is read: its left half left of the middle, its right half right");

    const EnvironmentImage dark;
    const std::vector<std::uint8_t> unlit = shadeMaterialBall(red, none, dark, lighting, size);
    check(unlit.size() == plastic.size() && meanLevel(unlit) == 0.0 && at(unlit, size, 0.5f, 0.5f).a == 0,
          "with no environment at all, an empty picture and no crash");
}

} // namespace

int main() {
    checkFile();
    checkMesh();
    checkStudio();
    checkShader();
    std::printf("%d failed\n", failures);
    return failures;
}
