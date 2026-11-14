/**
 * @file materialxfile.h
 * @brief A MaterialX document (`.mtlx`) read as MATERIALS: each material of the file as the
 *        values and texture maps of PoseStudio's own material (materialparams.h), to be put on
 *        a zone as a saved material is.
 *
 * MaterialX is the interchange format material libraries are published in: an XML document
 * that describes a surface as a shading node — one of a few standard models — whose inputs
 * are values or small graphs of nodes ending in image files. This reader does not evaluate
 * graphs; it TRACES each input of the shading node to what it is, and that is one of two
 * things PoseStudio's material can hold: a VALUE, or an IMAGE FILE (with the channel it is
 * read from, a constant it is multiplied by, and the tiling it is placed with).
 *
 * WHAT IS READ:
 *   - The shading models `standard_surface` (Autodesk's — what most libraries publish),
 *     `open_pbr_surface` (OpenPBR), `gltf_pbr` and `UsdPreviewSurface`, each input mapped to
 *     the material's own (PoseStudio's material IS the OpenPBR parameter set, so the mapping
 *     is nearly one to one), in the material's own units.
 *   - Materials named by `surfacematerial` nodes, by the older `material` / `shaderref` /
 *     `bindinput` form, or — a file with shading nodes and no material — one per shading node.
 *   - Texture nodes `image`, `tiledimage`, the glTF library's `gltf_image` / `gltf_colorimage`
 *     / `gltf_normalmap` and `UsdUVTexture`; `normalmap`, `bump` and `heighttonormal` (which
 *     slot a map goes to, and its strength); `extract`, `separate2/3/4` and `swizzle` (which
 *     channel); `multiply` by a constant (the value the map scales); `place2d`, a multiplied
 *     `texcoord` and `tiledimage`'s own inputs (the tiling, offset and rotation); `constant`,
 *     `convert` and arithmetic on constants; nodegraphs and their outputs; `fileprefix` and
 *     `colorspace` as inherited; `xi:include`; and a `displacement` shader's map and scale.
 *
 * WHAT IS NOT: a procedural node (noise, a ramp, a brick pattern) has no image to name, and
 * arithmetic between two textures is not one image — the input then keeps what else is known
 * of it, and the material's `notes` say so, in words for people. Looks, geometry assignments,
 * lights and node definitions are passed over.
 *
 * Like materialfile.h this is a codec of TEXT: the caller reads the bytes (and hands a reader
 * for includes), so a path outside the system code page is no obstacle. A file a material
 * names comes back AS WRITTEN — its fileprefix applied — relative to the document's folder or
 * absolute: finding it is the caller's, who knows where the document is. Qt-free.
 */

#ifndef MATERIALXFILE_H
#define MATERIALXFILE_H

#include "materialparams.h"

#include <functional>
#include <string>
#include <vector>

namespace pose {

/// One texture map of a material read from a MaterialX document.
struct MaterialXTexture {
    MaterialMap slot = MaterialMap::BaseColor;
    std::string file;  ///< As the document names it (fileprefix applied): relative to the document's folder, or absolute.
    int         channel = 0; ///< The channel of the image the value is read from: 0 red … 3 alpha.
};

struct MaterialXMaterial {
    std::string                   name;   ///< The material's name in the document.
    std::string                   model;  ///< The shading model it was read from ("standard_surface", …).
    MaterialParams                params; ///< The values (no map sources: `textures` has the maps).
    std::vector<MaterialXTexture> textures;
    std::vector<std::string>      notes;  ///< What the document says that the material does not carry.
};

struct MaterialXDocument {
    std::string                    version;
    std::vector<MaterialXMaterial> materials;
};

/// Hands back the text of a document named by an `xi:include` (the `href` as written: relative
/// to the including document). False when it cannot be had; the include is then passed over.
using MaterialXInclude = std::function<bool(const std::string& href, std::string& text)>;

/// Reads the text of a MaterialX document. False with a human-readable @p error when the text
/// is not XML, is not a MaterialX document, or holds no material this reader knows how to read
/// (@p out then lists none).
bool readMaterialXText(const std::string& text, MaterialXDocument& out, std::string& error, const MaterialXInclude& include = {});

} // namespace pose

#endif // MATERIALXFILE_H
