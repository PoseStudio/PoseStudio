// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file texturedecode.h
 * @brief THE TEXTURE DECODE LAYER: a model's texture sources — files or embedded bytes — turned
 *        into the shared, tightly-packed RGBA8 images the renderer uploads.
 *
 * Image decoding is deliberately NOT in the Qt-free import/ and scene/ subtrees: it lives here, in
 * the Qt layer (QImage and its plugins), so neither the importers nor the renderer need an image
 * codec. Two users:
 *   - THE IMPORT (decodeModelTextures, called by ModelImportService::uploadModelData for every
 *     import path): each unique source decoded ONCE and shared by every mesh that samples it,
 *     the derived images made (the specular / glossiness conversion, the parameter map, the
 *     layer map, the cutout baked into the base color's alpha), and each image gutter-filled
 *     against the UVs of its users — the slots' own images behind a packed or baked binding
 *     too, since the tab makes that binding again from them.
 *   - THE MATERIALS TAB (vulkanwindow_materials.cpp), which runs the same decodes and bakes on
 *     one file when a map is chosen: decodeTextureFile, decodeMaskFile, bakeCutoutIntoBaseColor
 *     and the two packed maps (buildParameterMap, buildLayerMap). It fills the chosen image's
 *     gutters itself, once, where it is decoded (VulkanWindow::decodeSlotSource).
 */

#ifndef TEXTUREDECODE_H
#define TEXTUREDECODE_H

#include <memory>
#include <string>

namespace pose {

struct DecodedImage;
struct ModelData;

/// Decodes every mesh's TextureSources (external paths OR embedded bytes) into shared
/// DecodedImages via QImage, bakes opacity masks into the diffuse alpha, and gutter-fills each
/// image against the union of its users' UVs. Each unique source is decoded ONCE (figure zones
/// commonly share atlas files) and the decodes run across cores. Logs and leaves a mesh slot
/// untextured if its image can't be decoded. Shared by the model and figure import paths so
/// both decode textures identically — keep every import path behind this choke point, or the
/// mip-bleed seams the gutter fill prevents come back.
void decodeModelTextures(ModelData& data);

/// THE TEXTURE SLOTS' helpers (the Materials tab, vulkanwindow_materials.cpp — the same decodes
/// and bakes the import runs, on one file): an image file decoded to RGBA8 (capped like an
/// import's), a cutout mask file (its gray in every channel, opaque), and the base color with a
/// mask baked into its alpha (a white canvas at the mask's size without a base color; a copy of
/// the base without a mask; null with neither). Null on a failed decode (logged).
std::shared_ptr<DecodedImage> decodeTextureFile(const std::string& path);
std::shared_ptr<DecodedImage> decodeMaskFile(const std::string& path);
std::shared_ptr<DecodedImage> bakeCutoutIntoBaseColor(const std::shared_ptr<const DecodedImage>& base,
                                                      const std::shared_ptr<const DecodedImage>& mask);
/// THE PARAMETER MAP (what the shader samples as the roughness texture) from the four slots'
/// plain images: the roughness map's channel @p roughChannel into R, the metalness map's
/// @p metalChannel into G, the ambient occlusion map's red into B and the transmission weight
/// map's red into A — 255 where a map is absent, so the shader's per-texel multiply is a no-op
/// for that value. Sized like the first map present (the others are resampled onto it); null
/// without any of them.
std::shared_ptr<DecodedImage> buildParameterMap(const std::shared_ptr<const DecodedImage>& rough, int roughChannel,
                                                const std::shared_ptr<const DecodedImage>& metal, int metalChannel,
                                                const std::shared_ptr<const DecodedImage>& ao,
                                                const std::shared_ptr<const DecodedImage>& transmission);
/// THE LAYER MAP (set 1 binding 8) from the four slots' plain images — the coat weight, the coat
/// roughness, the sheen weight and the thin-film thickness, each one's red channel into R, G, B
/// and A (255 where a map is absent); null without any of them.
std::shared_ptr<DecodedImage> buildLayerMap(const std::shared_ptr<const DecodedImage>& coatWeight,
                                            const std::shared_ptr<const DecodedImage>& coatRoughness,
                                            const std::shared_ptr<const DecodedImage>& sheenWeight,
                                            const std::shared_ptr<const DecodedImage>& thinFilmThickness);

} // namespace pose

#endif // TEXTUREDECODE_H
