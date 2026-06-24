/**
 * @file objloader.h
 * @brief Parses a Wavefront .obj (+ referenced .mtl) into CPU-side geometry, grouped by material.
 *
 * Pure data + std + GLM — no Vulkan, no Qt — so it stays trivially testable. The GPU upload lives
 * in Mesh/Model (scene/mesh.h); Scene turns the ModelData this returns into device buffers. The
 * tinyobjloader implementation is compiled in objloader.cpp (the project's single TINYOBJLOADER TU).
 */

#ifndef OBJLOADER_H
#define OBJLOADER_H

#include "vertex.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pose {

/// One material group of an imported OBJ: a self-contained triangle mesh, its diffuse base color,
/// and (optionally) its diffuse texture.
struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    // MTL Kd when present, else this default for geometry with no material/texture: a light
    // neutral grey, hex #D1D1D1. Stored LINEAR (sRGB->linear of 209/209/209) because the lit
    // fragment output is hardware-encoded to the sRGB swapchain — so a fully-lit surface reads
    // as exactly #D1D1D1 on screen. Same convention as the renderer's clear-colour note.
    glm::vec3             baseColor{0.6376f, 0.6376f, 0.6376f};

    // Diffuse texture (MTL map_Kd). The loader fills the resolved absolute path; the Qt layer
    // (VulkanWindow) decodes it via QImage into diffusePixels (tightly-packed RGBA8) so the
    // Qt-free renderer never needs an image codec. Empty path / zero size => untextured.
    std::string           diffuseTexturePath;
    std::vector<uint8_t>  diffusePixels;
    uint32_t              diffuseWidth  = 0;
    uint32_t              diffuseHeight = 0;
};

/// One imported OBJ file: all of its material groups.
struct ModelData {
    std::vector<MeshData> meshes;
};

/// Loads and triangulates @p path, resolving its .mtl relative to the file. Generates smooth
/// normals when the file has none. Throws std::runtime_error on parse/IO failure.
ModelData loadObj(const std::string& path);

} // namespace pose

#endif // OBJLOADER_H
