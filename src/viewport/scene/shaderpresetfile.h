// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file shaderpresetfile.h
 * @brief A content-library SHADER PRESET (a `.duf` file of asset type `preset_shader`) read as
 *        a MATERIAL: the values and texture maps of PoseStudio's own material
 *        (materialparams.h), to be put on a zone as a saved material is.
 *
 * Content libraries in the native figure format ship surfaces as shader presets: one file a
 * surface — a metal, a fabric, a glaze — to be applied to whatever is selected. The file is
 * the format's usual JSON document (gzip-compressed or plain: both are read), and it says the
 * surface as SETTINGS OF A SHADER: a base definition in its `material_library`, and the
 * preset's own values as single-key tracks addressed to "the selection's materials"
 * (`name://@selection#materials:?<setting>/value`, `…/image_file`, `…/image`).
 *
 * WHAT IS READ: presets of the format's standard physically-based surface shader — the one
 * the figure importer reads for a character's zones (materialparser.h) — in each of its three
 * BASE MIXING modes, which say what the base settings mean:
 *   - metallicity / roughness: nearly PoseStudio's own material, value for value;
 *   - specular / glossiness: a specular color and a glossiness, converted (specgloss.h);
 *   - weighted: a diffuse and a glossy layer mixed by two weights, converted the same way.
 * Above the base it reads the top coat in its four layering modes, the metallic flakes, the
 * backscattering (the material's sheen), refraction, translucency, emission (its units and
 * color temperature), the thin film, the relief (normal, bump, displacement), the cutout and
 * the tiling. The shader has layers the material has no equal for; each is brought in as the
 * nearest thing the material does have, and where something is lost the material's `notes`
 * say so, in words for people:
 *   - a top coat that reflects strongly or in a color (a "specular layer" worked as a coat,
 *     a weighted glaze) is MERGED into the base as a metallic reflection; a plain clear coat
 *     stays a coat, its index set from its reflectance;
 *   - metallic flakes become a plain metallic share of the base (no sparkle);
 *   - a GLOSSINESS map is used as a roughness map read INVERTED (ShaderPresetTexture::inverted).
 *
 * A PARTIAL PRESET names a few settings only — "tile the maps six times", "this bump map" —
 * and changes just those on the surface it is applied to. It is read ONTO the material the
 * zone has (`standing`): the settings it names are set, the rest kept, and `textures` then
 * lists the slots it names alone (an empty uri = the slot emptied). Only settings that are
 * one value or one map of the material can be applied so; a partial preset that names
 * anything else is refused, with the reason.
 *
 * WHAT IS NOT: a preset made for another shader (a custom shader graph, an older renderer's
 * surface) is refused with a message — its look lives in shader code, not in values this
 * reader could carry. A material preset of asset type `preset_material` (one object's named
 * surfaces, all at once) is another kind of file and is refused too.
 *
 * Like materialfile.h this is a codec of BYTES in, values out: the caller reads the file and
 * finds the maps — a map comes back as the URI the file wrote ("/Runtime/Textures/…",
 * percent-encoded, relative to a content root), since the caller is who knows the roots.
 * Colors are stored in the file as display (sRGB) values and come out LINEAR. Qt-free.
 */

#ifndef SHADERPRESETFILE_H
#define SHADERPRESETFILE_H

#include "materialparams.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pose {

/// One texture map of a material read from a shader preset.
struct ShaderPresetTexture {
    MaterialMap slot = MaterialMap::BaseColor;
    std::string uri;              ///< As the file wrote it (root-relative, percent-encoded). Empty: a partial preset empties the slot.
    bool        inverted = false; ///< Read the image inverted (a glossiness map in a roughness slot).
};

struct ShaderPresetMaterial {
    bool                             partial = false; ///< Names part of a surface: `params` is `standing` with those settings changed.
    MaterialParams                   params;          ///< The values (no map sources: `textures` has the maps).
    std::vector<ShaderPresetTexture> textures;        ///< Full: every slot that holds a map. Partial: the slots the preset names.
    std::vector<std::string>         notes;           ///< What the preset says that the material does not carry.
    int                              settings = 0;    ///< How many shader settings the preset names (what told full from partial).
    std::string                      reading;         ///< For a report: how the base was read ("metallic/roughness", "merged", …).
};

/// The mean color of the image a preset names by @p uri, LINEAR, where the caller can say
/// (false: it cannot — the reader then takes the image for white). A specular or coat color
/// given as "white × a map" is only known through its map: whether the surface is a metal or
/// a dielectric is decided by this mean.
using ShaderPresetMapMean = std::function<bool(const std::string& uri, glm::vec3& mean)>;

/// Whether @p bytes — a file's contents, gzip-compressed or plain — are a shader preset: the
/// cheap test a `.duf` file is routed by (the extension is every kind of preset's). Reads the
/// document's head; never throws.
bool isShaderPreset(const std::vector<std::uint8_t>& bytes);

/// isShaderPreset of the file at @p path (UTF-8); false for a file that cannot be read.
bool isShaderPresetFile(const std::string& path);

/// Reads a shader preset's bytes (gzip-compressed or plain) into @p out. @p standing is the
/// material of the zone it is to be applied to: a PARTIAL preset is read onto it, a full one
/// ignores it. False with a human-readable @p error when the bytes are not the format's
/// document, are another kind of preset, are made for a shader this reader does not read, or
/// are a partial preset of settings that cannot be applied one by one.
bool readShaderPreset(const std::vector<std::uint8_t>& bytes, const MaterialParams& standing, ShaderPresetMaterial& out, std::string& error,
                      const ShaderPresetMapMean& mapMean = {});

} // namespace pose

#endif // SHADERPRESETFILE_H
