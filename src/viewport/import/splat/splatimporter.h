/**
 * @file splatimporter.h
 * @brief GAUSSIAN SPLATS (2026-11-05; the user: "import Gaussian Splats into a scene … static, no
 *        rigging … these should look super realistic, as splats often do"): the reader for a 3D
 *        Gaussian Splatting scene — the trained `.ply` the reference implementation writes (one
 *        vertex per splat: position, log scales, a quaternion, a logit opacity and spherical-
 *        harmonic color up to degree 3) and the compact `.splat` (32 bytes a splat: position,
 *        scales, an RGBA byte color, a byte quaternion; degree 0 only) and the compressed `.spz`
 *        (Niantic's layout, versions 1-3: one gzip stream of byte-quantized attributes — see
 *        loadSpz in the .cpp) — into a format-neutral SplatData the scene uploads (SplatCloud).
 *        Qt-free (std + GLM); throws std::runtime_error with a readable message on anything it
 *        cannot read.
 *
 * What is NOT read: a compressed PLY (SuperSplat's chunked `.ply`), an `.spz` of version 4 (ZSTD
 * streams), `.ksplat` — each refused with a message naming it; a plain point cloud `.ply` with no
 * scales (not a splat scene).
 */

#ifndef SPLATIMPORTER_H
#define SPLATIMPORTER_H

#include <glm/glm.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace pose {

/// A Gaussian splat scene as the file has it: the splats' own frame (the capture's, typically
/// y-down — the import service turns it over) and units (the capture's, often metres-ish).
struct SplatData {
    std::vector<glm::vec3> positions; ///< Centres.
    std::vector<glm::vec3> scales;    ///< Standard deviations along the splat's own axes (linear; the file's log scales exponentiated).
    std::vector<glm::vec4> rotations; ///< Unit quaternions (w, x, y, z).
    std::vector<float>     opacities; ///< 0..1 (the file's logits through the sigmoid).
    int                    shDegree = 0; ///< 0..3 — 1, 4, 9 or 16 coefficients per color channel.
    /// Per splat, coefficient-major: coefficients() entries of (r, g, b) — the degree-0 term first
    /// (the reference's f_dc), then the higher terms in the reference's basis order.
    std::vector<float>     sh;
    std::string            format;    ///< "ply", "splat" or "spz" (for the log).
    /// The file's frame is y-DOWN (the trainers' camera convention: a `.ply`, a `.splat`) and
    /// wants turnOverYDown; an `.spz` is y-up already (RUB: x right, y up, z toward the viewer —
    /// the frame the turn-over puts a `.ply` in).
    bool                   yDown = true;
    /// Trained with the antialiasing (mip) formulation (an `.spz` flag): the renderer then
    /// compensates each splat's opacity for the low-pass dilation, as that formulation expects.
    bool                   antialiased = false;

    std::size_t count() const { return positions.size(); }
    int         coefficients() const { return (shDegree + 1) * (shDegree + 1); }
};

/// Reads a `.ply` (the reference 3D Gaussian Splatting layout), a `.splat` or an `.spz` file.
SplatData loadSplatFile(const std::string& path);

/// Turns the scene over about x (y → −y, z → −z): a trained scene is almost always y-DOWN (the
/// camera convention its images were posed in). Positions, rotations and the harmonics (whose
/// basis functions odd in y or z change sign) are rewritten in place, so the model transform can
/// start at the identity and the Transform tab's rows read 0 for the orientation it imported in.
void turnOverYDown(SplatData& data);

} // namespace pose

#endif // SPLATIMPORTER_H
