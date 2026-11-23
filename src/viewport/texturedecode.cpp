// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file texturedecode.cpp
 * @brief The texture decode layer: the decodes, the derived images, the gutter fill. See
 *        texturedecode.h.
 *
 * In this order: the single decodes (a source read with the size cap, as RGBA or as a gray
 * mask), the image arithmetic the derived maps share (one channel resampled, the packed maps,
 * the specular / glossiness conversion), THE IMPORT'S DECODE — decodeModelTextures as numbered
 * steps over one DecodeState — and the Materials tab's entry points.
 */

#include "texturedecode.h"

#include "specgloss.h"

#include "import/modeldata.h"
#include "import/texturegutters.h"
#include "parallelfor.h"

#include <QBuffer>
#include <QDebug>
#include <QImage>
#include <QImageReader>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pose {

namespace {

/// A 64-bit hash of an embedded image's bytes (FNV-1a's constants over eight bytes a step: an
/// encoded PNG or JPEG is megabytes, and a byte a step would be felt on a model of many
/// materials). It only has to tell different images apart most of the time — SourceKeys settles
/// equality by comparing the bytes.
std::uint64_t contentHash(const std::vector<uint8_t>& bytes) {
    std::uint64_t  hash = 0xcbf29ce484222325ull ^ static_cast<std::uint64_t>(bytes.size());
    const uint8_t* at = bytes.data();
    std::size_t    left = bytes.size();
    for (; left >= 8; at += 8, left -= 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, at, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 32;
    }
    for (; left > 0; ++at, --left) {
        hash = (hash ^ *at) * 0x100000001b3ull;
    }
    return hash;
}

/// The identity keys of one decode's texture sources (empty key = no source). External files
/// share by PATH — the point of the whole decode cache, since figure zones commonly reference the
/// same atlas files (face/lips/ears share the face maps, torso/arms/legs the body maps). Embedded
/// images share by CONTENT: a reader hands every slot of every material its own copy of the
/// bytes (a GLB's metallic-roughness image arrives once as the roughness map and once as the
/// metalness map, an atlas once per primitive that uses it), so the bytes are all that says two
/// sources are one image — their size and a hash find the candidates, a byte comparison confirms.
/// (Keyed by the source's address, as they were, each copy was decoded, gutter-filled and
/// uploaded on its own.) It keeps pointers to the sources' byte vectors: valid because
/// data.meshes is never reallocated while the decode runs.
class SourceKeys {
public:
    std::string keyFor(const TextureSource& src) {
        if (!src.path.empty()) {
            return src.path;
        }
        if (src.encoded.empty()) {
            return {};
        }
        const std::uint64_t hash = contentHash(src.encoded);
        std::vector<Embedded>& sameHash = m_embedded[hash];
        for (const Embedded& known : sameHash) {
            if (*known.bytes == src.encoded) {
                return known.key;
            }
        }
        // (Two different images with one hash: the place in the hash's list tells them apart.)
        std::string key = "embedded:" + std::to_string(src.encoded.size()) + ":" + std::to_string(hash);
        if (!sameHash.empty()) {
            key += "~" + std::to_string(sameHash.size());
        }
        sameHash.push_back({&src.encoded, key});
        return key;
    }

private:
    struct Embedded {
        const std::vector<uint8_t>* bytes = nullptr;
        std::string                 key;
    };
    std::unordered_map<std::uint64_t, std::vector<Embedded>> m_embedded;
};

// Viewport texture-resolution cap. Characters commonly ship 8192² bump/roughness maps; at 8K each
// costs 4× a 4K map at EVERY stage — decode, gutter fill, staging copy, GPU mip build, and ~341 MB
// of VRAM per map — for pore-level detail the viewport camera can't resolve at working distances.
// Downscaling at decode keeps import fast and VRAM sane (mainstream DCC viewports cap/compress the
// same way); JPEG sources even decode directly at the reduced size (libjpeg DCT scaling). Promote
// to a Preference if a full-res close-up use case appears.
constexpr int kMaxTextureSize = 4096;

// Reads a TextureSource (external path OR embedded bytes) into a QImage, downscaling anything over
// kMaxTextureSize as part of the decode. Returns a null QImage on failure (the caller logs).
// Safe to call from worker threads — QImageReader and the built-in plugins are reentrant.
QImage readCapped(const TextureSource& src) {
    QBuffer buffer;
    QImageReader reader;
    if (!src.encoded.empty()) {
        // qsizetype, not int — an int cast would truncate embedded buffers over 2 GB.
        buffer.setData(reinterpret_cast<const char*>(src.encoded.data()),
                       static_cast<qsizetype>(src.encoded.size()));
        buffer.open(QIODevice::ReadOnly);
        reader.setDevice(&buffer);
    } else {
        reader.setFileName(QString::fromStdString(src.path));
    }
    // size() is a header-only peek for PNG/JPEG; setScaledSize folds the downscale into the decode
    // (a fast DCT-scaled decode for JPEG, decode+smooth-scale for the rest).
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > kMaxTextureSize || size.height() > kMaxTextureSize)) {
        reader.setScaledSize(size.scaled(kMaxTextureSize, kMaxTextureSize, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    // Formats whose header size wasn't available still get capped, just less efficiently.
    if (!image.isNull() &&
        (image.width() > kMaxTextureSize || image.height() > kMaxTextureSize)) {
        image = image.scaled(kMaxTextureSize, kMaxTextureSize, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return image;
}

// Decodes a TextureSource into a tightly-packed RGBA8 image (capped — see kMaxTextureSize).
// Returns null (logged) for an undecodable image.
std::shared_ptr<DecodedImage> decodeSource(const TextureSource& src) {
    QImage image = readCapped(src);
    if (image.isNull()) {
        const QString name = src.path.empty() ? QStringLiteral("<embedded texture>")
                                              : QString::fromStdString(src.path);
        qWarning() << "[viewport] Texture not found/decodable:" << name;
        return nullptr;
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) {
        // convertToFormat can itself fail (allocation failure on a 4K map under memory pressure)
        // — a 0x0 DecodedImage with empty pixels must not reach the GPU upload path.
        qWarning() << "[viewport] Texture conversion failed (out of memory?):"
                   << QString::fromStdString(src.path);
        return nullptr;
    }
    auto out = std::make_shared<DecodedImage>();
    out->width = static_cast<uint32_t>(image.width());
    out->height = static_cast<uint32_t>(image.height());
    const uchar* bits = image.constBits();
    out->pixels.assign(bits, bits + image.sizeInBytes());
    return out;
}

// Decodes an opacity (cutout) mask to grayscale (capped like every other map — it's rescaled to
// the diffuse dimensions at bake anyway). Returns a null QImage (logged) on failure — the mesh
// then keeps its scalar opacity.
QImage decodeMask(const TextureSource& src) {
    QImage mask = readCapped(src);
    if (mask.isNull()) {
        qWarning() << "[viewport] Opacity mask not found/decodable:"
                   << QString::fromStdString(src.path);
        return mask;
    }
    return mask.convertToFormat(QImage::Format_Grayscale8);
}

/// A grayscale mask as a DecodedImage: its gray in every channel, opaque.
std::shared_ptr<DecodedImage> maskToDecoded(const QImage& mask) {
    const QImage gray = mask.format() == QImage::Format_Grayscale8 ? mask : mask.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull()) {
        return nullptr;
    }
    auto out = std::make_shared<DecodedImage>();
    out->width = static_cast<uint32_t>(gray.width());
    out->height = static_cast<uint32_t>(gray.height());
    out->pixels.resize(static_cast<std::size_t>(out->width) * out->height * 4);
    for (uint32_t y = 0; y < out->height; ++y) {
        const uchar* row = gray.constScanLine(static_cast<int>(y));
        uint8_t*     dst = out->pixels.data() + static_cast<std::size_t>(y) * out->width * 4;
        for (uint32_t x = 0; x < out->width; ++x) {
            dst[x * 4] = dst[x * 4 + 1] = dst[x * 4 + 2] = row[x];
            dst[x * 4 + 3] = 255;
        }
    }
    return out;
}

/// A decoded image's channel @p channel (0 red … 3 alpha) as a Grayscale8 QImage, resampled to
/// @p width × @p height when the sizes differ (smooth), for the bakes that combine maps of
/// different sizes.
QImage channelImage(const DecodedImage& img, int channel, uint32_t width, uint32_t height) {
    QImage g(static_cast<int>(img.width), static_cast<int>(img.height), QImage::Format_Grayscale8);
    const int ch = std::clamp(channel, 0, 3);
    for (uint32_t y = 0; y < img.height; ++y) {
        uchar*         row = g.scanLine(static_cast<int>(y));
        const uint8_t* src = img.pixels.data() + static_cast<std::size_t>(y) * img.width * 4;
        for (uint32_t x = 0; x < img.width; ++x) {
            row[x] = src[x * 4 + static_cast<uint32_t>(ch)];
        }
    }
    if (img.width == width && img.height == height) {
        return g;
    }
    // (The re-convert after scaling is load-bearing — see the opacity bake below.)
    return g.scaled(static_cast<int>(width), static_cast<int>(height), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_Grayscale8);
}

/// A decoded RGBA image resampled to @p width × @p height (itself when the size already matches).
std::shared_ptr<const DecodedImage> fitImage(const std::shared_ptr<DecodedImage>& img, uint32_t width, uint32_t height) {
    if (!img || (img->width == width && img->height == height)) {
        return img;
    }
    const QImage src(img->pixels.data(), static_cast<int>(img->width), static_cast<int>(img->height),
                     static_cast<int>(img->width) * 4, QImage::Format_RGBA8888);
    const QImage scaled = src.scaled(static_cast<int>(width), static_cast<int>(height), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGBA8888);
    auto out = std::make_shared<DecodedImage>();
    out->width = width;
    out->height = height;
    out->pixels.resize(static_cast<std::size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out->pixels.data() + static_cast<std::size_t>(y) * width * 4, scaled.constScanLine(static_cast<int>(y)),
                    static_cast<std::size_t>(width) * 4);
    }
    return out;
}

/// A PACKED map: up to four grayscale maps' channels into one RGBA image, 255 where a part is
/// absent (so the shader's per-texel multiply is a no-op there), sized like the first part present
/// (the others resampled onto it). The parameter map and the layer map below are its two uses.
/// Null without any part (the white fallback serves).
std::shared_ptr<DecodedImage> buildPackedMap(const std::array<std::shared_ptr<const DecodedImage>, 4>& parts,
                                             const std::array<int, 4>& channels) {
    const DecodedImage* sized = nullptr;
    for (const auto& part : parts) {
        if (part && !sized) {
            sized = part.get();
        }
    }
    if (sized == nullptr) {
        return nullptr;
    }
    auto out = std::make_shared<DecodedImage>();
    out->width = sized->width;
    out->height = sized->height;
    out->pixels.assign(static_cast<std::size_t>(out->width) * out->height * 4, 255);
    for (std::size_t c = 0; c < parts.size(); ++c) {
        if (!parts[c]) {
            continue;
        }
        // (channelImage copies the channel as it is at the packed map's size, resamples otherwise.)
        const QImage g = channelImage(*parts[c], channels[c], out->width, out->height);
        if (g.isNull()) {
            continue;
        }
        for (uint32_t y = 0; y < out->height; ++y) {
            const uchar* row = g.constScanLine(static_cast<int>(y));
            uint8_t*     dst = out->pixels.data() + static_cast<std::size_t>(y) * out->width * 4;
            for (uint32_t x = 0; x < out->width; ++x) {
                dst[x * 4 + c] = row[x];
            }
        }
    }
    return out;
}

} // namespace

/// THE PARAMETER MAP: roughness (R), metalness (G), ambient occlusion (B) and transmission weight
/// (A), each from its own map's channel — glTF's metallic-roughness texture (G and B of one image)
/// and occlusion texture (R), FBX's and USD's separate grayscale maps, an MTL's map_Pr / map_Pm —
/// 255 where a map is absent, so the shader's per-texel multiply is a no-op for that value. Sized
/// like the first map present (the others are resampled onto it).
std::shared_ptr<DecodedImage> buildParameterMap(const std::shared_ptr<const DecodedImage>& rough, int roughChannel,
                                                const std::shared_ptr<const DecodedImage>& metal, int metalChannel,
                                                const std::shared_ptr<const DecodedImage>& ao,
                                                const std::shared_ptr<const DecodedImage>& transmission) {
    return buildPackedMap({rough, metal, ao, transmission}, {roughChannel, metalChannel, 0, 0});
}

/// THE LAYER MAP (set 1 binding 8): the coat weight (R), the coat roughness (G), the sheen weight
/// (B) and the thin-film thickness (A), each map's red channel; 255 where a map is absent.
std::shared_ptr<DecodedImage> buildLayerMap(const std::shared_ptr<const DecodedImage>& coatWeight,
                                            const std::shared_ptr<const DecodedImage>& coatRoughness,
                                            const std::shared_ptr<const DecodedImage>& sheenWeight,
                                            const std::shared_ptr<const DecodedImage>& thinFilmThickness) {
    return buildPackedMap({coatWeight, coatRoughness, sheenWeight, thinFilmThickness}, {0, 0, 0, 0});
}

namespace {

/// THE SPECULAR / GLOSSINESS BAKE (import/specgloss.h): the standard solve run per texel over a
/// mesh's diffuse map, specular-color map and glossiness map (any of them may be missing — its
/// factor stands in), at the largest of their sizes, into a base-color map (sRGB, the diffuse's
/// alpha kept), a metalness map (grayscale) and — from a glossiness map — a roughness map.
struct SpecGlossBake {
    std::shared_ptr<DecodedImage> base, metal, rough;
};
SpecGlossBake bakeSpecGloss(const std::shared_ptr<DecodedImage>& diffuse, const std::shared_ptr<DecodedImage>& spec,
                            const std::shared_ptr<DecodedImage>& gloss, const MeshData& mesh) {
    SpecGlossBake out;
    uint32_t      width = 1, height = 1;
    for (const auto& img : {diffuse, spec, gloss}) {
        if (img) {
            width = std::max(width, img->width);
            height = std::max(height, img->height);
        }
    }
    const auto d = fitImage(diffuse, width, height);
    const auto s = fitImage(spec, width, height);
    const auto g = (gloss == spec) ? s : fitImage(gloss, width, height);
    // sRGB byte → linear, and linear → sRGB byte through a 4096-step table.
    static const std::array<float, 256> kToLinear = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<std::size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    static const std::array<uint8_t, 4096> kToSrgb = [] {
        std::array<uint8_t, 4096> t{};
        for (int i = 0; i < 4096; ++i) {
            const float c = static_cast<float>(i) / 4095.0f;
            const float e = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            t[static_cast<std::size_t>(i)] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(e * 255.0f)), 0, 255));
        }
        return t;
    }();
    const auto encode = [](float linear) {
        return kToSrgb[static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(linear * 4095.0f)), 0, 4095))];
    };
    const bool bakeColor = d != nullptr || s != nullptr; // (a glossiness map alone leaves the scalar solve's color and metalness)
    const std::size_t texels = static_cast<std::size_t>(width) * height;
    if (bakeColor) {
        out.base = std::make_shared<DecodedImage>();
        out.base->width = width;
        out.base->height = height;
        out.base->pixels.resize(texels * 4);
        out.metal = std::make_shared<DecodedImage>();
        out.metal->width = width;
        out.metal->height = height;
        out.metal->pixels.resize(texels * 4);
    }
    if (g) {
        out.rough = std::make_shared<DecodedImage>();
        out.rough->width = width;
        out.rough->height = height;
        out.rough->pixels.resize(texels * 4);
    }
    const glm::vec3 diffuseFactor(mesh.diffuseFactor);
    const uint32_t  glossChannel = static_cast<uint32_t>(std::clamp(mesh.glossinessChannel, 0, 3));
    parallelFor(static_cast<int>(height), [&](int row) {
        for (uint32_t x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(row) * width + x;
            glm::vec3 dl = diffuseFactor;
            uint8_t   alpha = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(mesh.diffuseFactor.a * 255.0f)), 0, 255));
            if (d) {
                const uint8_t* p = d->pixels.data() + i * 4;
                dl = glm::vec3(kToLinear[p[0]], kToLinear[p[1]], kToLinear[p[2]]) * diffuseFactor;
                alpha = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(p[3] * mesh.diffuseFactor.a)), 0, 255));
            }
            glm::vec3 sl = mesh.specularColorFactor;
            if (s) {
                const uint8_t* p = s->pixels.data() + i * 4;
                sl = glm::vec3(kToLinear[p[0]], kToLinear[p[1]], kToLinear[p[2]]) * mesh.specularColorFactor;
            }
            float gl = mesh.glossinessFactor;
            if (g) {
                gl = static_cast<float>(g->pixels[i * 4 + glossChannel]) / 255.0f * mesh.glossinessFactor; // (linear data)
            }
            const MetallicRoughness mr = convertSpecGloss(dl, sl, gl);
            if (out.base) {
                uint8_t* b = out.base->pixels.data() + i * 4;
                b[0] = encode(mr.baseColor.r);
                b[1] = encode(mr.baseColor.g);
                b[2] = encode(mr.baseColor.b);
                b[3] = alpha;
                const uint8_t m = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(mr.metalness * 255.0f)), 0, 255));
                uint8_t*      mp = out.metal->pixels.data() + i * 4;
                mp[0] = mp[1] = mp[2] = m;
                mp[3] = 255;
            }
            if (out.rough) {
                const uint8_t r = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(mr.roughness * 255.0f)), 0, 255));
                uint8_t*      rp = out.rough->pixels.data() + i * 4;
                rp[0] = rp[1] = rp[2] = r;
                rp[3] = 255;
            }
        }
    });
    return out;
}

// --- THE IMPORT'S DECODE: decodeModelTextures, as its steps over one DecodeState ------------------

/// A unique source to decode, and the key its image is filed under.
struct SourceJob {
    std::string          key;
    const TextureSource* src = nullptr;
};

/// What each slot of one mesh is keyed by in DecodeState::images (empty = the slot has no source).
struct MeshKeys {
    // The slots with a rule of their own, named in the steps.
    std::string diffuse, mask, roughness, metalness, specColor, gloss;
    // The plain slots (kPlainSlots below pairs each with its source and its image).
    std::string normal, specMask, translucency, detailNormal, bump, emission, diffuseRoughness;
    std::string coatWeight, coatColor, coatRoughness, coatReflectivity, coatBump, translucencyWeight;
    std::string specularColorSlot, anisotropy, anisotropyRotation, lobe2Roughness, lobeRatio;
    std::string detailWeight, detailRoughness, displacement, ao, transmissionColor, transmissionWeight;
    std::string coatAnisotropy, coatAnisotropyRotation, sheenWeight, sheenColor, sheenRoughness, thinFilmWeight, thinFilmThickness;
    // Made by the steps.
    std::string roughnessPlain; // the roughness map's own decode, before the parameter map takes its key
    std::string layer;          // the packed LAYER map (buildLayerMap): coat weight / roughness, sheen weight, film thickness
};

/// THE SLOTS THAT ARE DECODED AND ASSIGNED AS THEY ARE — one row a slot: where the reader put
/// its source, what the decode keys it by, and where its decoded image goes. The slots with a
/// rule of their own (the base color and its cutout, the roughness and metalness of the
/// parameter map, the specular / glossiness inputs) are named in the steps instead. A texture
/// slot added to MeshData that needs nothing but its decode is ONE ROW here (and a key in
/// MeshKeys).
struct PlainSlot {
    TextureSource MeshData::*   source;
    std::string MeshKeys::*     key;
    DecodedImagePtr MeshData::* image;
};
constexpr PlainSlot kPlainSlots[] = {
    {&MeshData::normal, &MeshKeys::normal, &MeshData::normalImage},
    {&MeshData::specMask, &MeshKeys::specMask, &MeshData::specMaskImage},
    {&MeshData::translucencyMap, &MeshKeys::translucency, &MeshData::translucencyImage},
    {&MeshData::detailNormalMap, &MeshKeys::detailNormal, &MeshData::detailNormalImage},
    {&MeshData::bumpMap, &MeshKeys::bump, &MeshData::bumpImage},
    {&MeshData::emissionMap, &MeshKeys::emission, &MeshData::emissionImage},
    {&MeshData::diffuseRoughnessMap, &MeshKeys::diffuseRoughness, &MeshData::diffuseRoughnessImage},
    {&MeshData::coatWeightMap, &MeshKeys::coatWeight, &MeshData::coatWeightImage},
    {&MeshData::coatColorMap, &MeshKeys::coatColor, &MeshData::coatColorImage},
    {&MeshData::coatRoughnessMap, &MeshKeys::coatRoughness, &MeshData::coatRoughnessImage},
    {&MeshData::coatReflectivityMap, &MeshKeys::coatReflectivity, &MeshData::coatReflectivityImage},
    {&MeshData::coatBumpMap, &MeshKeys::coatBump, &MeshData::coatBumpImage},
    {&MeshData::translucencyWeightMap, &MeshKeys::translucencyWeight, &MeshData::translucencyWeightImage},
    {&MeshData::specularColorSlotMap, &MeshKeys::specularColorSlot, &MeshData::specularColorSlotImage},
    {&MeshData::anisotropyMap, &MeshKeys::anisotropy, &MeshData::anisotropyImage},
    {&MeshData::anisotropyRotationMap, &MeshKeys::anisotropyRotation, &MeshData::anisotropyRotationImage},
    {&MeshData::lobe2RoughnessMap, &MeshKeys::lobe2Roughness, &MeshData::lobe2RoughnessImage},
    {&MeshData::lobeRatioMap, &MeshKeys::lobeRatio, &MeshData::lobeRatioImage},
    {&MeshData::detailWeightMap, &MeshKeys::detailWeight, &MeshData::detailWeightImage},
    {&MeshData::detailRoughnessMap, &MeshKeys::detailRoughness, &MeshData::detailRoughnessImage},
    {&MeshData::displacementMap, &MeshKeys::displacement, &MeshData::displacementImage},
    {&MeshData::aoMap, &MeshKeys::ao, &MeshData::aoImage},
    {&MeshData::transmissionColorMap, &MeshKeys::transmissionColor, &MeshData::transmissionColorImage},
    {&MeshData::transmissionWeightMap, &MeshKeys::transmissionWeight, &MeshData::transmissionWeightImage},
    {&MeshData::coatAnisotropyMap, &MeshKeys::coatAnisotropy, &MeshData::coatAnisotropyImage},
    {&MeshData::coatAnisotropyRotationMap, &MeshKeys::coatAnisotropyRotation, &MeshData::coatAnisotropyRotationImage},
    {&MeshData::sheenWeightMap, &MeshKeys::sheenWeight, &MeshData::sheenWeightImage},
    {&MeshData::sheenColorMap, &MeshKeys::sheenColor, &MeshData::sheenColorImage},
    {&MeshData::sheenRoughnessMap, &MeshKeys::sheenRoughness, &MeshData::sheenRoughnessImage},
    {&MeshData::thinFilmWeightMap, &MeshKeys::thinFilmWeight, &MeshData::thinFilmWeightImage},
    {&MeshData::thinFilmThicknessMap, &MeshKeys::thinFilmThickness, &MeshData::thinFilmThicknessImage},
};

/// A unique (base color, cutout mask) pair, and the image made of it (bakeCutouts).
struct Combo {
    std::string                   diffuseKey, maskKey;
    std::shared_ptr<DecodedImage> image;
    bool                          masked = false;
};
constexpr std::size_t kNoCombo = static_cast<std::size_t>(-1);

/// One model's decode in progress: what the steps of decodeModelTextures hand each other.
struct DecodeState {
    explicit DecodeState(ModelData& model)
        : data(model), keys(model.meshes.size()), comboOfMesh(model.meshes.size(), kNoCombo) {}

    ModelData& data;
    /// Every image of the decode by its key: the unique sources (null until decoded, and after a
    /// failed decode), then the derived ones ("sgbake:", "param:", "layer:").
    std::unordered_map<std::string, std::shared_ptr<DecodedImage>> images;
    std::vector<SourceJob>                                         imageJobs;
    /// The cutout masks by their key, each ONE image (its gray in every channel) whatever number
    /// of zones it cuts out — the Opacity slot's own image is shared like any other map's, so
    /// what the Materials tab later derives from it is made once for the zones that share it.
    /// Null after a failed decode.
    std::unordered_map<std::string, std::shared_ptr<DecodedImage>> masks;
    std::vector<SourceJob>                                         maskJobs;
    SourceKeys                                                     sourceKeys;
    std::vector<MeshKeys>                   keys;        // one per mesh
    std::vector<Combo>                      combos;      // the unique (base color, mask) pairs …
    std::vector<std::size_t>                comboOfMesh; // … and each mesh's (kNoCombo = it has no mask)
};

/// STEP 1. Collect every unique image source (and opacity mask) across the model's meshes.
void collectSources(DecodeState& s) {
    ModelData& data = s.data;
    auto&      images = s.images;
    auto&      imageJobs = s.imageJobs;
    auto&      masks = s.masks;
    auto&      maskJobs = s.maskJobs;
    auto&      keys = s.keys;
    auto addImageSource = [&](const TextureSource& src) -> std::string {
        std::string key = s.sourceKeys.keyFor(src);
        if (!key.empty() && images.emplace(key, nullptr).second) {
            imageJobs.push_back({key, &src});
        }
        return key;
    };
    auto addMaskSource = [&](const TextureSource& src) -> std::string {
        std::string key = s.sourceKeys.keyFor(src);
        if (!key.empty() && masks.emplace(key, nullptr).second) {
            maskJobs.push_back({key, &src});
        }
        return key;
    };
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        MeshData& mesh = data.meshes[m];
        MeshKeys& k = keys[m];
        k.diffuse = addImageSource(mesh.diffuse);
        k.mask = addMaskSource(mesh.opacityMask);
        k.roughness = addImageSource(mesh.roughnessMap); // (its channel is read into the parameter map below)
        k.metalness = addImageSource(mesh.metalnessMap);
        k.specColor = mesh.convertSpecGloss ? addImageSource(mesh.specularColorMap) : std::string();
        k.gloss = mesh.convertSpecGloss ? addImageSource(mesh.glossinessMap) : std::string();
        for (const PlainSlot& slot : kPlainSlots) {
            k.*slot.key = addImageSource(mesh.*slot.source);
        }
    }
}

/// STEP 2. Decode each unique source exactly once, across cores. The serial per-mesh decode of
/// shared 4K atlas maps used to dominate figure import time.
void decodeSources(DecodeState& s) {
    auto& images = s.images;
    auto& imageJobs = s.imageJobs;
    auto& masks = s.masks;
    auto& maskJobs = s.maskJobs;
    std::vector<std::shared_ptr<DecodedImage>> imageResults(imageJobs.size());
    std::vector<std::shared_ptr<DecodedImage>> maskResults(maskJobs.size());
    const int imageCount = static_cast<int>(imageJobs.size());
    parallelFor(imageCount + static_cast<int>(maskJobs.size()), [&](int i) {
        if (i < imageCount) {
            imageResults[static_cast<std::size_t>(i)] =
                decodeSource(*imageJobs[static_cast<std::size_t>(i)].src);
        } else {
            const std::size_t mi = static_cast<std::size_t>(i - imageCount);
            const QImage      mask = decodeMask(*maskJobs[mi].src);
            maskResults[mi] = mask.isNull() ? nullptr : maskToDecoded(mask);
        }
    });
    for (std::size_t i = 0; i < imageJobs.size(); ++i) {
        images[imageJobs[i].key] = std::move(imageResults[i]);
    }
    for (std::size_t i = 0; i < maskJobs.size(); ++i) {
        masks[maskJobs[i].key] = std::move(maskResults[i]);
    }
}

/// STEP 2a. THE SPECULAR / GLOSSINESS BAKE (specgloss.h): a mesh whose material came in the
/// older convention WITH maps gets the standard solve per texel — a base-color map and a
/// metalness map in place of its diffuse and specular-color maps, a roughness map from a
/// glossiness map — once per unique combination of inputs and factors; the scalars that the
/// maps now carry become 1.
void bakeSpecGlossMaps(DecodeState& s) {
    ModelData& data = s.data;
    auto&      images = s.images;
    auto&      keys = s.keys;
    struct SgBake {
        std::string base, metal, rough; // the synthetic image keys made (empty = none)
    };
    std::unordered_map<std::string, SgBake> bakes;
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        MeshData& mesh = data.meshes[m];
        MeshKeys& k = keys[m];
        if (!mesh.convertSpecGloss || (k.diffuse.empty() && k.specColor.empty() && k.gloss.empty())) {
            continue; // the reader's scalar solve stands
        }
        const auto f = [](float v) { return std::to_string(v); };
        const std::string comboKey = k.diffuse + '\x1f' + k.specColor + '\x1f' + k.gloss + '\x1f' + std::to_string(mesh.glossinessChannel) +
                                     '\x1f' + f(mesh.diffuseFactor.r) + ',' + f(mesh.diffuseFactor.g) + ',' + f(mesh.diffuseFactor.b) + ',' +
                                     f(mesh.diffuseFactor.a) + '\x1f' + f(mesh.specularColorFactor.r) + ',' + f(mesh.specularColorFactor.g) +
                                     ',' + f(mesh.specularColorFactor.b) + '\x1f' + f(mesh.glossinessFactor);
        auto it = bakes.find(comboKey);
        if (it == bakes.end()) {
            SgBake bake;
            const std::shared_ptr<DecodedImage> diffuse = k.diffuse.empty() ? nullptr : images[k.diffuse];
            const std::shared_ptr<DecodedImage> spec = k.specColor.empty() ? nullptr : images[k.specColor];
            const std::shared_ptr<DecodedImage> gloss = k.gloss.empty() ? nullptr : images[k.gloss];
            if (diffuse || spec || gloss) {
                const SpecGlossBake baked = bakeSpecGloss(diffuse, spec, gloss, mesh);
                if (baked.base) {
                    bake.base = "sgbake:base:" + comboKey;
                    images[bake.base] = baked.base;
                }
                if (baked.metal) {
                    bake.metal = "sgbake:metal:" + comboKey;
                    images[bake.metal] = baked.metal;
                }
                if (baked.rough) {
                    bake.rough = "sgbake:rough:" + comboKey;
                    images[bake.rough] = baked.rough;
                }
            }
            it = bakes.emplace(comboKey, bake).first;
        }
        // A slot that now holds a SYNTHESIZED image has no file any more: its source's path is
        // cleared with the key. (Left, the Materials tab named the converted base color by the
        // file it was made from, and Save Material — a slot with a file is saved as that file —
        // stored the UNCONVERTED diffuse beside the converted metalness map and Metallic 1: the
        // metal texels, black in the older convention's diffuse, came back as black metal. With
        // no path the slot reads "embedded" and the save writes the image out.)
        const SgBake& bake = it->second;
        if (!bake.base.empty()) {
            k.diffuse = bake.base;
            mesh.diffuse.path.clear();
            mesh.baseColor = glm::vec3(1.0f);
        }
        if (!bake.metal.empty()) {
            k.metalness = bake.metal;
            mesh.metalnessMap.path.clear();
            mesh.metalnessChannel = 0;
            mesh.metalness = 1.0f;
        }
        if (!bake.rough.empty()) {
            k.roughness = bake.rough;
            mesh.roughnessMap.path.clear();
            mesh.roughnessChannel = 0;
            mesh.roughness = 1.0f;
            mesh.lobe1Roughness = 1.0f;
            mesh.lobe2Roughness = 1.0f;
        }
    }
}

/// STEP 2b. THE PARAMETER MAP (buildParameterMap): what the shader samples as the roughness
/// texture — the roughness map's channel in red, the metalness map's in green, the ambient
/// occlusion map in blue and the transmission weight map in alpha (255 where a map is absent)
/// — one per unique combination, so the plain decode of a file another slot uses stays as
/// it is.
void packParameterMaps(DecodeState& s) {
    ModelData& data = s.data;
    auto&      images = s.images;
    auto&      keys = s.keys;
    std::unordered_map<std::string, std::shared_ptr<DecodedImage>> paramMaps;
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        MeshData& mesh = data.meshes[m];
        MeshKeys& k = keys[m];
        if (k.roughness.empty() && k.metalness.empty() && k.ao.empty() && k.transmissionWeight.empty()) {
            continue;
        }
        const std::string comboKey = k.roughness + "#r" + std::to_string(mesh.roughnessChannel) + '\x1f' + k.metalness + "#m" +
                                     std::to_string(mesh.metalnessChannel) + '\x1f' + k.ao + '\x1f' + k.transmissionWeight;
        auto it = paramMaps.find(comboKey);
        if (it == paramMaps.end()) {
            const auto img = [&](const std::string& key) -> std::shared_ptr<DecodedImage> {
                return key.empty() ? nullptr : images[key];
            };
            it = paramMaps.emplace(comboKey, buildParameterMap(img(k.roughness), mesh.roughnessChannel, img(k.metalness),
                                                              mesh.metalnessChannel, img(k.ao), img(k.transmissionWeight)))
                     .first;
        }
        const std::string paramKey = "param:" + comboKey;
        images[paramKey] = it->second;
        k.roughnessPlain = k.roughness; // (the slot's own image, for the Materials tab)
        k.roughness = it->second ? paramKey : std::string();
    }
}

/// STEP 2c. THE LAYER MAP (buildLayerMap): the coat weight, the coat roughness, the sheen weight
/// and the thin-film thickness maps packed into the one linear image the shader samples
/// (255 where a map is absent) — one per unique combination.
void packLayerMaps(DecodeState& s) {
    ModelData& data = s.data;
    auto&      images = s.images;
    auto&      keys = s.keys;
    std::unordered_map<std::string, std::shared_ptr<DecodedImage>> layerMaps;
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        MeshKeys& k = keys[m];
        if (k.coatWeight.empty() && k.coatRoughness.empty() && k.sheenWeight.empty() && k.thinFilmThickness.empty()) {
            continue;
        }
        const std::string comboKey = k.coatWeight + '\x1f' + k.coatRoughness + '\x1f' + k.sheenWeight + '\x1f' + k.thinFilmThickness;
        auto it = layerMaps.find(comboKey);
        if (it == layerMaps.end()) {
            const auto img = [&](const std::string& key) -> std::shared_ptr<DecodedImage> {
                return key.empty() ? nullptr : images[key];
            };
            it = layerMaps.emplace(comboKey, buildLayerMap(img(k.coatWeight), img(k.coatRoughness), img(k.sheenWeight),
                                                           img(k.thinFilmThickness)))
                     .first;
        }
        if (it->second) {
            const std::string layerKey = "layer:" + comboKey;
            images[layerKey] = it->second;
            k.layer = layerKey;
        }
    }
}

/// STEP 3. Bake each unique (diffuse, mask) pair into its own shared image: the mask (a lash/
/// brow card's shape, an older figure's eye-shell fade) rides in the diffuse map's ALPHA channel,
/// so the shader needs no extra texture/binding. The bake COPIES the diffuse first — the plain
/// shared decode must stay pristine for meshes using it unmasked. Runs before the gutter fill so
/// the baked alpha gets the same seam treatment as the colors.
void bakeCutouts(DecodeState& s) {
    ModelData& data = s.data;
    auto&      images = s.images;
    auto&      masks = s.masks;
    auto&      keys = s.keys;
    auto&      combos = s.combos;
    auto&      comboOfMesh = s.comboOfMesh;
    std::unordered_map<std::string, std::size_t> comboIndex; // "diffuse\x1fmask" -> index
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        if (keys[m].mask.empty()) {
            continue; // no opacity mask — the plain shared diffuse serves as-is
        }
        const std::string ck = keys[m].diffuse + '\x1f' + keys[m].mask;
        const auto [it, inserted] = comboIndex.emplace(ck, combos.size());
        if (inserted) {
            combos.push_back({keys[m].diffuse, keys[m].mask, nullptr, false});
        }
        comboOfMesh[m] = it->second;
    }
    parallelFor(static_cast<int>(combos.size()), [&](int ci) {
        Combo&                               combo = combos[static_cast<std::size_t>(ci)];
        const std::shared_ptr<DecodedImage>& mask = masks.find(combo.maskKey)->second;
        const std::shared_ptr<DecodedImage> base =
            combo.diffuseKey.empty() ? nullptr : images.find(combo.diffuseKey)->second;
        if (!mask) {
            combo.image = base; // undecodable mask: the mesh keeps its scalar opacity
            return;
        }
        // The bake itself is the Materials tab's too (bakeCutoutIntoBaseColor): the mask's gray
        // as a DecodedImage, rescaled to the diffuse dimensions and written into its alpha.
        std::shared_ptr<DecodedImage> baked = bakeCutoutIntoBaseColor(base, mask);
        if (!baked) {
            combo.image = base;
            return;
        }
        combo.image = std::move(baked);
        combo.masked = true;
    });
}

/// STEP 4. Assign the shared images + per-slot flags to each mesh.
void assignImages(DecodeState& s) {
    ModelData& data = s.data;
    auto&      images = s.images;
    auto&      masks = s.masks;
    auto&      keys = s.keys;
    auto&      combos = s.combos;
    auto&      comboOfMesh = s.comboOfMesh;
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        MeshData&       mesh = data.meshes[m];
        const MeshKeys& k = keys[m];
        if (comboOfMesh[m] != kNoCombo) {
            const Combo& combo = combos[comboOfMesh[m]];
            mesh.diffuseImage = combo.image;
            mesh.hasOpacityMask = combo.masked;
            if (combo.masked) {
                // The slots' own images (the Materials tab): the plain base color and the mask.
                mesh.diffusePlainImage = k.diffuse.empty() ? nullptr : images[k.diffuse];
                const auto maskIt = masks.find(k.mask);
                if (maskIt != masks.end()) {
                    mesh.opacityMaskImage = maskIt->second;
                }
            }
        } else if (!k.diffuse.empty()) {
            mesh.diffuseImage = images[k.diffuse];
            if (mesh.opacityFromDiffuseAlpha && mesh.diffuseImage) {
                // The decoded alpha IS the mask: no bake needed — and the image is the Base Color
                // slot's own, alpha and all. (Left without a plain image, the slot read as "a
                // cutout alone, no base-color map": the Materials tab showed no map, a render
                // drew the zone untextured and opaque, and any rebind — an overlay added —
                // replaced the texture with nothing.)
                mesh.hasOpacityMask = true;
                mesh.diffusePlainImage = mesh.diffuseImage;
            }
        }
        if (!mesh.diffusePlainImage && !k.diffuse.empty() && !mesh.hasOpacityMask) {
            mesh.diffusePlainImage = images[k.diffuse]; // (no cutout: the uploaded image is the plain one)
        }
        if (!k.roughnessPlain.empty()) {
            mesh.roughnessPlainImage = images[k.roughnessPlain];
        }
        if (!k.metalness.empty()) {
            mesh.metalnessPlainImage = images[k.metalness];
        }
        for (const PlainSlot& slot : kPlainSlots) {
            if (!(k.*slot.key).empty()) {
                mesh.*slot.image = images[k.*slot.key];
            }
        }
        if (!k.layer.empty()) {
            mesh.layerMapImage = images[k.layer];
        }
        if (!mesh.normalImage) {
            mesh.normalMode = 0; // undecodable/absent detail map: drop to flat shading
        }
        if (!k.roughness.empty()) {
            mesh.roughnessImage = images[k.roughness];
        }
        if (mesh.specMaskImage) {
            // The sampled mask texels now do the scaling the parser's assumed-average discount only
            // guessed at — switch to the material's UNdiscounted specular weight.
            mesh.specularWeight = mesh.specularWeightWithMap;
        }
        if (!mesh.detailNormalImage) {
            mesh.detailWeight = 0.0f; // undecodable map: no detail layer
        }
    }
}

/// STEP 5. Gutter-fill each unique image ONCE, against the union of its users' UV islands, so
/// the GPU-built mip chain never averages the atlas background through a seam (see
/// texturegutters.h). A shared atlas must keep every user's islands intact, which is exactly
/// what the union coverage provides. The micro-detail (pore) normal is excluded — it tiles/
/// wraps, leaving no gutters. Serial over images; the fill itself is parallel inside.
///
/// AN IMAGE A ZONE'S MATERIAL MOVES ACROSS ITS UVs IS LEFT AS IT IS: a material that repeats,
/// shifts or turns its maps (MeshData::uvTiling / uvOffset / uvRotation: a fabric tiled four
/// times over a garment's islands) samples them where the zone's UV islands are NOT, so nothing
/// of such an image is background — filled against the islands as they lie, the texels the
/// zone really draws were overwritten with colors dilated from the ones it does not.
///
/// THE SLOTS' OWN IMAGES BEHIND A PACKED OR BAKED BINDING ARE FILLED TOO — the plain roughness,
/// metalness, ambient occlusion and transmission weight images under the parameter map, the
/// four under the layer map, a cut-out zone's plain base color and its mask. The viewport binds
/// none of them, but the Materials tab makes the binding AGAIN from them whenever its group is
/// bound afresh (an overlay with gloss or metal, a partner map chosen, a Metallic value moved
/// under an overlay), and packed from unfilled images the zone's map lost its fill at that
/// moment. The packed and baked images were made BEFORE this step, so they are what they were.
void fillGutters(DecodeState& s) {
    ModelData& data = s.data;
    auto&      images = s.images;
    auto&      masks = s.masks;
    auto&      keys = s.keys;
    auto&      combos = s.combos;
    auto&      comboOfMesh = s.comboOfMesh;
    std::unordered_map<DecodedImage*, std::vector<UvMeshRef>> usersOf;
    std::unordered_set<const DecodedImage*>                   movedAcross; // (see above: never filled)
    auto noteUser = [&](const std::shared_ptr<DecodedImage>& img, const MeshData& mesh) {
        if (!img || mesh.indices.size() < 3 || mesh.vertices.empty()) {
            return;
        }
        if (mesh.uvTiling != glm::vec2(1.0f) || mesh.uvOffset != glm::vec2(0.0f) || mesh.uvRotation != 0.0f) {
            movedAcross.insert(img.get());
        }
        // (Once a mesh: an image may serve several of its slots — a packed occlusion /
        // roughness / metalness image is three — and coverage is a union.)
        std::vector<UvMeshRef>& users = usersOf[img.get()];
        if (users.empty() || users.back().vertices != &mesh.vertices) {
            users.push_back({&mesh.vertices, &mesh.indices});
        }
    };
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        const MeshData& mesh = data.meshes[m];
        const MeshKeys& k = keys[m];
        if (comboOfMesh[m] != kNoCombo) {
            noteUser(combos[comboOfMesh[m]].image, mesh);
        } else if (!k.diffuse.empty()) {
            noteUser(images[k.diffuse], mesh);
        }
        if (!k.normal.empty()) {
            noteUser(images[k.normal], mesh);
        }
        if (!k.roughness.empty()) {
            noteUser(images[k.roughness], mesh);
        }
        if (!k.specMask.empty()) {
            noteUser(images[k.specMask], mesh);
        }
        if (!k.translucency.empty()) {
            noteUser(images[k.translucency], mesh);
        }
        if (!k.bump.empty()) {
            noteUser(images[k.bump], mesh);
        }
        if (!k.emission.empty()) {
            noteUser(images[k.emission], mesh);
        }
        if (!k.layer.empty()) {
            noteUser(images[k.layer], mesh);
        }
        if (!k.sheenColor.empty()) {
            noteUser(images[k.sheenColor], mesh);
        }
        // The slots' own images behind the packed and baked bindings (see above).
        for (const std::string* key : {&k.roughnessPlain, &k.metalness, &k.ao, &k.transmissionWeight, &k.coatWeight,
                                       &k.coatRoughness, &k.sheenWeight, &k.thinFilmThickness}) {
            if (!key->empty()) {
                noteUser(images[*key], mesh);
            }
        }
        if (comboOfMesh[m] != kNoCombo && combos[comboOfMesh[m]].masked) {
            if (!k.diffuse.empty()) {
                noteUser(images[k.diffuse], mesh);
            }
            noteUser(masks[k.mask], mesh);
        }
    }
    for (auto& [image, users] : usersOf) {
        if (movedAcross.count(image) == 0) {
            fillImageGutters(image->pixels, image->width, image->height, users);
        }
    }
}

} // namespace

void decodeModelTextures(ModelData& data) {
    DecodeState s(data);
    collectSources(s);
    decodeSources(s);
    bakeSpecGlossMaps(s);
    packParameterMaps(s);
    packLayerMaps(s);
    bakeCutouts(s);
    assignImages(s);
    fillGutters(s);
}

// --- THE TEXTURE SLOTS' helpers (the Materials tab: vulkanwindow_materials.cpp) -----------------

std::shared_ptr<DecodedImage> decodeTextureFile(const std::string& path) {
    TextureSource src;
    src.path = path;
    return decodeSource(src);
}

std::shared_ptr<DecodedImage> decodeMaskFile(const std::string& path) {
    TextureSource src;
    src.path = path;
    const QImage mask = decodeMask(src);
    return mask.isNull() ? nullptr : maskToDecoded(mask);
}

std::shared_ptr<DecodedImage> bakeCutoutIntoBaseColor(const std::shared_ptr<const DecodedImage>& base,
                                                      const std::shared_ptr<const DecodedImage>& mask) {
    if (!mask) {
        // No cutout: the plain base color is the whole image (a copy — the shared decode must stay
        // pristine; null without one, for the fallback).
        return base ? std::make_shared<DecodedImage>(*base) : nullptr;
    }
    auto img = std::make_shared<DecodedImage>();
    if (base) {
        *img = *base;
    } else {
        // Untextured mesh: a white RGBA canvas at the mask's own resolution.
        img->width = mask->width;
        img->height = mask->height;
        img->pixels.assign(static_cast<std::size_t>(img->width) * img->height * 4, 255);
    }
    // Rescaled to the diffuse dimensions, so mismatched map sizes are fine (channelImage's
    // re-convert after scaling is load-bearing: QImage::scaled() on a Grayscale8 source silently
    // returns RGB32 whenever the size actually changes, and reading 4-byte BGRX rows as 1-byte
    // gray bakes garbage alpha).
    const QImage scaled = channelImage(*mask, 0, img->width, img->height);
    if (scaled.isNull()) {
        return nullptr;
    }
    for (uint32_t y = 0; y < img->height; ++y) {
        const uchar* row = scaled.constScanLine(static_cast<int>(y));
        uint8_t*     out = img->pixels.data() + static_cast<std::size_t>(y) * img->width * 4;
        for (uint32_t x = 0; x < img->width; ++x) {
            out[x * 4 + 3] = row[x];
        }
    }
    return img;
}

} // namespace pose
