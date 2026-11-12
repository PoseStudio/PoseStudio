/**
 * @file scenebuilder.cpp
 * @brief The application's scene description as a Cycles scene. See scenebuilder.h.
 *
 * THE MAPPING'S RULES, each where the two sides' conventions differ:
 *
 *   SPACE     Both are right-handed with the same axes: positions go across unchanged (metres,
 *             +Y up). Cycles has no "up" of its own except in the equirectangular lookup, which
 *             reads +Z as the zenith — the world shader permutes the direction (worldGraph).
 *   CAMERA    A Cycles camera looks down its +Z; the application's down -Z: the matrix's Z column
 *             is negated. The viewplane is set explicitly so the field of view is always the
 *             VERTICAL one (Cycles's automatic viewplane applies it to the wider side).
 *   UV        v runs DOWN in the application's UVs and its images' row 0 is their top; the
 *             in-memory loader hands rows over unflipped (memoryimage.h), so the same UV finds
 *             the same texel and neither side converts.
 *   NORMALS   The application's own vertex normals and UV tangents (with their handedness) are
 *             given as attributes: a normal map is then perturbed in the very frame the viewport
 *             perturbs it in — T, sign · (N × T), N — and the seams of a mesh split at its UV
 *             borders stay smooth (Cycles's own normals would crease every split vertex).
 *   WINDING   Cycles derives a triangle's front side from its winding and flips the shading
 *             normal of a back side; imported winding is not reliable (the viewport lights two-
 *             sided), so each triangle is wound to agree with its vertex normals.
 *   SPECULAR  The canonical weight is 1 = the dielectric's reflectance at its ior; Principled's
 *             "Specular IOR Level" says the same with 0.5.
 *   GLASS     A transmissive surface is a Principled BSDF for every ray but SHADOW rays, which
 *             pass through it tinted (a transparent BSDF): what lies behind clear geometry — an
 *             eye behind its cornea — is lit directly instead of only through caustics, which a
 *             path tracer resolves slowly and Cycles's integrator is told to leave out. Its TINT
 *             is the surface's where the material names no depth for it; with a depth it is the
 *             volume's (an absorption volume inside a solid; nothing in a thin wall). A THIN,
 *             CLEAR wall — an eye's film of moisture, a pane — is not "transmission" at all: a
 *             transparent closure beside what its surface reflects (MaterialGraph::build).
 *   SKIN      Subsurface is the skin-type random walk, its radius the per-channel distance light
 *             diffuses under the surface, in metres (renderexport derives it).
 *   FLOOR     The floor is a shadow catcher, and to Cycles a catcher shows what the SCENE takes
 *             away from the light of the "footage" it stands in: a light counts as the footage's
 *             — is in the catcher's reference pass — only if its object is flagged a shadow
 *             catcher too. Left unflagged, a light can only ADD light to the floor and its
 *             shadows never show (a sun-lit scene had none at all). The sun and the environment
 *             light are the scene's own lights: both are flagged.
 *   LIGHT     A light's strength is an IRRADIANCE (renderapi::Light): a sun's is Cycles's own
 *             number; a point or spot light's — the irradiance a metre away, a radiant intensity
 *             — is 1/4π of the flux Cycles asks for, and an area light's on-axis intensity 1/π
 *             of its flux (a Lambertian face). Every light shines along its object's -Z; an area
 *             light's width is its object's X, held level (lightFrame). The environment's
 *             strength is one number for every bounce (see renderapi::Environment), and only
 *             CAMERA rays see the backdrop instead.
 *   UPDATES   An interactive render's scene is REBUILT IN PLACE (SceneBuilder::build called
 *             again): the objects and their geometry — the lights are geometry — are deleted and
 *             made anew, but Cycles never deletes a SHADER, so the materials' shaders are a pool
 *             that is given new graphs (a shader left over is given an empty one, which lets go
 *             of its images). An image that both the old and the new graphs sample is found by
 *             its loader's identity and is not loaded twice.
 */

#include "scenebuilder.h"

#include "memoryimage.h"

#include "scene/background.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/image.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"

#include "util/colorspace.h"
#include "util/set.h"
#include "util/transform.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace posecycles {

using namespace pose::renderapi;

namespace {

// How far a height map's white stands over its black, in metres, at a bump strength of 1. The
// viewport applies a bump map as a slope per TEXEL (mesh.frag's bumpSlope: 1.5 × the central
// difference); on a figure's 4K maps, about four thousand texels to the metre, that is the slope
// of a relief this deep.
constexpr float kBumpDistance = 0.0002f;
// Under this roughness a thin transmissive wall is CLEAR: what is behind it is seen sharp.
constexpr float kClearShellRoughness = 0.03f;
// The index of the surface light enters a scattering medium through (skin's, about).
constexpr float kSubsurfaceIor = 1.4f;

ccl::float3 toFloat3(const float v[3]) {
    return ccl::make_float3(v[0], v[1], v[2]);
}

bool isWhite(const float c[3]) {
    return c[0] >= 0.999f && c[1] >= 0.999f && c[2] >= 0.999f;
}

/// The image handles of one scene: each application image is added to Cycles's image manager
/// once (per row order), however many materials sample it.
class ImageSet {
public:
    ImageSet(ccl::Scene* scene, const Scene& in) : m_scene(scene), m_in(in) {}

    bool valid(int index) const {
        return index >= 0 && static_cast<uint32_t>(index) < m_in.imageCount && m_in.images[index].pixels != nullptr;
    }
    const Image& image(int index) const { return m_in.images[index]; }

    static ccl::ustring colorSpaceOf(const Image& image) {
        if (image.colorSpace == ColorSpace::SRgb) {
            return ccl::u_colorspace_scene_linear_srgb; // sRGB-encoded, the scene's primaries
        }
        return image.format == PixelFormat::Float32 ? ccl::u_colorspace_scene_linear : ccl::u_colorspace_data;
    }

    ccl::ImageHandle handle(int index, bool flipRows) {
        const std::pair<int, bool> key(index, flipRows);
        const auto found = m_handles.find(key);
        if (found != m_handles.end()) {
            return found->second;
        }
        ccl::ImageParams params;
        params.colorspace = colorSpaceOf(m_in.images[index]);
        // The channels are independent data (a cutout in a base color's alpha, four masks packed
        // into one image): never premultiplied.
        params.alpha_type = ccl::IMAGE_ALPHA_CHANNEL_PACKED;
        params.extension = ccl::EXTENSION_REPEAT;
        params.interpolation = ccl::INTERPOLATION_LINEAR;
        ccl::ImageHandle handle = m_scene->image_manager->add_image(
            std::make_unique<MemoryImageLoader>(m_in.images[index], flipRows), params);
        m_handles.emplace(key, handle);
        return handle;
    }

private:
    ccl::Scene*                                   m_scene;
    const Scene&                                  m_in;
    std::map<std::pair<int, bool>, ccl::ImageHandle> m_handles;
};

// ---------------------------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------------------------

class MaterialGraph {
public:
    MaterialGraph(ImageSet& images, const Material& material)
        : m_images(images), m_m(material), m_graph(std::make_unique<ccl::ShaderGraph>()) {}

    std::unique_ptr<ccl::ShaderGraph> build();

private:
    const Texture& map(MapSlot slot) const { return m_m.maps[static_cast<int>(slot)]; }
    bool           has(MapSlot slot) const { return m_images.valid(map(slot).image); }

    /// The material's transformed UV, or null where the transform is the identity (an image
    /// node then reads the mesh's UV by itself).
    ccl::ShaderOutput* uv();
    /// The image node of @p texture (one per image and UV in this graph).
    ccl::ImageTextureNode* imageNode(const Texture& texture, ccl::ShaderOutput* coords);
    /// @p texture as a color, and as a scalar (its channel).
    ccl::ShaderOutput* colorOf(const Texture& texture);
    ccl::ShaderOutput* scalarOf(const Texture& texture);

    /// input = value × map (the constant alone without a map).
    void linkScalar(ccl::ShaderNode* node, const char* input, float value, MapSlot slot);
    /// input = color × map.
    void linkColor(ccl::ShaderNode* node, const char* input, const float color[3], MapSlot slot);

    ccl::ShaderOutput* multiply(ccl::ShaderOutput* a, float b);
    ccl::ShaderOutput* multiply(ccl::ShaderOutput* a, ccl::ShaderOutput* b);

    ImageSet&                         m_images;
    const Material&                   m_m;
    std::unique_ptr<ccl::ShaderGraph> m_graph;
    ccl::ShaderOutput*                m_uv = nullptr;
    bool                              m_uvBuilt = false;
    std::map<std::pair<int, ccl::ShaderOutput*>, ccl::ImageTextureNode*> m_imageNodes;
    std::map<ccl::ImageTextureNode*, ccl::SeparateColorNode*>            m_separate;
};

ccl::ShaderOutput* MaterialGraph::uv() {
    if (m_uvBuilt) {
        return m_uv;
    }
    m_uvBuilt = true;
    const bool identity = m_m.uvTiling[0] == 1.0f && m_m.uvTiling[1] == 1.0f && m_m.uvOffset[0] == 0.0f &&
                          m_m.uvOffset[1] == 0.0f && m_m.uvRotation == 0.0f;
    if (identity) {
        return nullptr;
    }
    // uv' = R(rotation) · (uv × tiling) + offset — the viewport's order (mesh.vert), whose
    // rotation (c·x + s·y, -s·x + c·y) is the mapping node's by the negative angle.
    auto* coords = m_graph->create_node<ccl::UVMapNode>();
    auto* mapping = m_graph->create_node<ccl::MappingNode>();
    mapping->set_mapping_type(ccl::NODE_MAPPING_TYPE_POINT);
    mapping->set_scale(ccl::make_float3(m_m.uvTiling[0], m_m.uvTiling[1], 1.0f));
    mapping->set_rotation(ccl::make_float3(0.0f, 0.0f, -m_m.uvRotation));
    mapping->set_location(ccl::make_float3(m_m.uvOffset[0], m_m.uvOffset[1], 0.0f));
    m_graph->connect(coords->output("UV"), mapping->input("Vector"));
    m_uv = mapping->output("Vector");
    return m_uv;
}

ccl::ImageTextureNode* MaterialGraph::imageNode(const Texture& texture, ccl::ShaderOutput* coords) {
    const std::pair<int, ccl::ShaderOutput*> key(texture.image, coords);
    const auto found = m_imageNodes.find(key);
    if (found != m_imageNodes.end()) {
        return found->second;
    }
    auto* node = m_graph->create_node<ccl::ImageTextureNode>();
    node->set_colorspace(ImageSet::colorSpaceOf(m_images.image(texture.image)));
    node->set_alpha_type(ccl::IMAGE_ALPHA_CHANNEL_PACKED);
    node->set_extension(ccl::EXTENSION_REPEAT);
    node->set_interpolation(ccl::INTERPOLATION_LINEAR);
    node->handle = m_images.handle(texture.image, /*flipRows=*/false);
    if (coords != nullptr) {
        m_graph->connect(coords, node->input("Vector"));
    }
    m_imageNodes.emplace(key, node);
    return node;
}

ccl::ShaderOutput* MaterialGraph::colorOf(const Texture& texture) {
    return imageNode(texture, uv())->output("Color");
}

ccl::ShaderOutput* MaterialGraph::scalarOf(const Texture& texture) {
    ccl::ImageTextureNode* node = imageNode(texture, uv());
    if (texture.channel == 3) {
        return node->output("Alpha");
    }
    if (texture.channel < 0 || m_images.image(texture.image).channels == 1) {
        return node->output("Color"); // (a color read as a scalar is its luminance)
    }
    ccl::SeparateColorNode*& separate = m_separate[node];
    if (separate == nullptr) {
        separate = m_graph->create_node<ccl::SeparateColorNode>();
        m_graph->connect(node->output("Color"), separate->input("Color"));
    }
    return separate->output(texture.channel == 0 ? "Red" : (texture.channel == 1 ? "Green" : "Blue"));
}

ccl::ShaderOutput* MaterialGraph::multiply(ccl::ShaderOutput* a, float b) {
    if (b == 1.0f) {
        return a;
    }
    auto* math = m_graph->create_node<ccl::MathNode>();
    math->set_math_type(ccl::NODE_MATH_MULTIPLY);
    math->set_value2(b);
    m_graph->connect(a, math->input("Value1"));
    return math->output("Value");
}

ccl::ShaderOutput* MaterialGraph::multiply(ccl::ShaderOutput* a, ccl::ShaderOutput* b) {
    auto* math = m_graph->create_node<ccl::MathNode>();
    math->set_math_type(ccl::NODE_MATH_MULTIPLY);
    m_graph->connect(a, math->input("Value1"));
    m_graph->connect(b, math->input("Value2"));
    return math->output("Value");
}

void MaterialGraph::linkScalar(ccl::ShaderNode* node, const char* input, float value, MapSlot slot) {
    ccl::ShaderInput* in = node->input(input);
    if (!has(slot) || value == 0.0f) {
        in->set(value);
        return;
    }
    m_graph->connect(multiply(scalarOf(map(slot)), value), in);
}

void MaterialGraph::linkColor(ccl::ShaderNode* node, const char* input, const float color[3], MapSlot slot) {
    ccl::ShaderInput* in = node->input(input);
    if (!has(slot)) {
        in->set(toFloat3(color));
        return;
    }
    ccl::ShaderOutput* texel = colorOf(map(slot));
    if (isWhite(color)) {
        m_graph->connect(texel, in);
        return;
    }
    auto* mix = m_graph->create_node<ccl::MixColorNode>();
    mix->set_blend_type(ccl::NODE_MIX_MUL);
    mix->set_fac(1.0f);
    mix->set_use_clamp(false);
    mix->set_b(toFloat3(color));
    m_graph->connect(texel, mix->input("A"));
    m_graph->connect(mix->output("Result"), in);
}

std::unique_ptr<ccl::ShaderGraph> MaterialGraph::build() {
    auto*       bsdf = m_graph->create_node<ccl::PrincipledBsdfNode>();
    const float transmission = std::clamp(m_m.transmission, 0.0f, 1.0f);
    const bool  thin = (m_m.flags & kMaterialThinWalled) != 0;
    const float white[3] = {1.0f, 1.0f, 1.0f};

    // --- What passes through (see GLASS in the file comment). The tint is the surface's where
    // the material gives no depth for it; given a depth it is the VOLUME's — absorption, below —
    // and a thin wall, which has no depth to speak of, is then clear. ---
    const bool   deep = m_m.transmissionDepth > 0.0f;
    const float* tint = deep ? white : m_m.transmissionColor;
    // How rough the surface is to what passes through it (its own value where the material keeps
    // one apart from its reflection's).
    const float throughRoughness = m_m.transmissionRoughness >= 0.0f ? m_m.transmissionRoughness : m_m.roughness;
    const bool  clearShell = thin && transmission > 0.0f && throughRoughness <= kClearShellRoughness &&
                             !has(MapSlot::TransmissionWeight);

    // --- Base. A transmissive surface's color is what passes through it: Principled tints its
    // transmission by the base color, so the surface's tint is folded in by the weight. (A clear
    // shell transmits beside the Principled BSDF, which keeps what the surface does not let
    // through.) ---
    float base[3];
    for (int c = 0; c < 3; ++c) {
        base[c] = clearShell ? m_m.baseColor[c] * (1.0f - transmission)
                             : m_m.baseColor[c] * (1.0f + (tint[c] - 1.0f) * transmission);
    }
    linkColor(bsdf, "Base Color", base, MapSlot::BaseColor);
    linkScalar(bsdf, "Metallic", m_m.metallic, MapSlot::Metallic);
    bsdf->set_diffuse_roughness(m_m.diffuseRoughness);

    // --- Specular (see SPECULAR in the file comment). Principled has ONE roughness for what a
    // surface reflects and what it transmits: a transmissive one takes its transmission's, by as
    // much as it transmits (what is seen THROUGH glass is the larger part of how it looks). ---
    float roughness = m_m.roughness;
    if (!clearShell && transmission > 0.0f && m_m.transmissionRoughness >= 0.0f) {
        roughness += (m_m.transmissionRoughness - roughness) * transmission;
    }
    linkScalar(bsdf, "Roughness", roughness, MapSlot::Roughness);
    linkScalar(bsdf, "Specular IOR Level", 0.5f * m_m.specular, MapSlot::Specular);
    bsdf->set_specular_tint(toFloat3(m_m.specularColor));
    bsdf->set_ior(std::max(m_m.ior, 1.0f));
    if (m_m.anisotropy > 0.0f) {
        bsdf->set_anisotropic(std::clamp(m_m.anisotropy, 0.0f, 1.0f));
        bsdf->set_anisotropic_rotation(m_m.anisotropyRotation / (2.0f * 3.14159265f)); // (turns)
        auto* tangent = m_graph->create_node<ccl::TangentNode>();
        tangent->set_direction_type(ccl::NODE_TANGENT_UVMAP);
        m_graph->connect(tangent->output("Tangent"), bsdf->input("Tangent"));
    }

    // --- Transmission, subsurface, sheen, coat, thin film, emission ---
    if (!clearShell) {
        linkScalar(bsdf, "Transmission Weight", transmission, MapSlot::TransmissionWeight);
        bsdf->set_thin_wall(thin ? 1 : 0);
    }

    if (m_m.subsurface > 0.0f) {
        // The skin-type random walk: the radius is how far light diffuses under the surface, per
        // channel, and the surface it enters through refracts.
        linkScalar(bsdf, "Subsurface Weight", std::clamp(m_m.subsurface, 0.0f, 1.0f), MapSlot::Subsurface);
        bsdf->set_subsurface_method(ccl::CLOSURE_BSSRDF_RANDOM_WALK_SKIN_ID);
        bsdf->set_subsurface_radius(toFloat3(m_m.subsurfaceRadius));
        bsdf->set_subsurface_scale(1.0f); // (the radius is given in metres)
        bsdf->set_subsurface_ior(kSubsurfaceIor);
        bsdf->set_subsurface_anisotropy(std::clamp(m_m.subsurfaceAnisotropy, -0.9f, 0.9f));
    }

    if (m_m.sheen > 0.0f) {
        linkScalar(bsdf, "Sheen Weight", m_m.sheen, MapSlot::SheenWeight);
        linkColor(bsdf, "Sheen Tint", m_m.sheenColor, MapSlot::SheenColor);
        bsdf->set_sheen_roughness(std::clamp(m_m.sheenRoughness, 0.0f, 1.0f));
    }

    if (m_m.coat > 0.0f) {
        linkScalar(bsdf, "Coat Weight", m_m.coat, MapSlot::CoatWeight);
        linkScalar(bsdf, "Coat Roughness", m_m.coatRoughness, MapSlot::CoatRoughness);
        bsdf->set_coat_ior(std::max(m_m.coatIor, 1.0f));
        bsdf->set_coat_tint(toFloat3(m_m.coatColor));
    }

    if (m_m.thinFilmThickness > 0.0f) {
        linkScalar(bsdf, "Thin Film Thickness", m_m.thinFilmThickness, MapSlot::ThinFilmThickness);
        bsdf->set_thin_film_ior(std::max(m_m.thinFilmIor, 1.0f));
    }

    if (m_m.emissionStrength > 0.0f) {
        linkColor(bsdf, "Emission Color", m_m.emissionColor, MapSlot::Emission);
        bsdf->set_emission_strength(m_m.emissionStrength);
    }

    // --- Opacity: the scalar × the cutout. (A clear shell is cut out below, around the whole of
    // it.) ---
    const float opacity = std::clamp(m_m.opacity, 0.0f, 1.0f);
    if (!clearShell) {
        linkScalar(bsdf, "Alpha", opacity, MapSlot::Opacity);
    }

    // --- The surface maps: a normal map in the mesh's own tangent frame, a bump map over it ---
    ccl::ShaderOutput* normal = nullptr;
    if (has(MapSlot::Normal) && m_m.normalStrength > 0.0f) {
        auto* normalMap = m_graph->create_node<ccl::NormalMapNode>();
        normalMap->set_space(ccl::NODE_NORMAL_MAP_TANGENT);
        normalMap->set_strength(m_m.normalStrength);
        m_graph->connect(colorOf(map(MapSlot::Normal)), normalMap->input("Color"));
        normal = normalMap->output("Normal");
    }
    if (has(MapSlot::Bump) && m_m.bumpStrength != 0.0f) {
        auto* bump = m_graph->create_node<ccl::BumpNode>();
        bump->set_strength(1.0f);
        bump->set_distance(kBumpDistance * std::abs(m_m.bumpStrength));
        bump->set_invert(m_m.bumpStrength < 0.0f);
        m_graph->connect(scalarOf(map(MapSlot::Bump)), bump->input("Height"));
        if (normal != nullptr) {
            m_graph->connect(normal, bump->input("Normal"));
        }
        normal = bump->output("Normal");
    }
    if (normal != nullptr) {
        m_graph->connect(normal, bsdf->input("Normal"));
        if (m_m.coat > 0.0f) {
            m_graph->connect(normal, bsdf->input("Coat Normal"));
        }
    }

    // --- The surface closure (see GLASS in the file comment) ---
    ccl::ShaderOutput* surface = bsdf->output("BSDF");
    if (clearShell) {
        // A thin, clear wall — the film of moisture over an eye, a window pane: what is behind it
        // is seen straight through (a thin wall does not bend light), less what its surface
        // reflects. A transparent closure is no "transmission" to the integrator: light reaches
        // what is behind it directly and nothing of it is noise.
        // What the surface lets through is 1 − its Fresnel reflectance — Schlick's, from the
        // angle alone: a thin wall has the same reflectance from both of its sides, where the
        // Fresnel node gives a back face the inside of a solid's (total reflection past 49
        // degrees: light could not get back out of an eye).
        const float f0 = std::clamp(std::pow((std::max(m_m.ior, 1.0f) - 1.0f) / (std::max(m_m.ior, 1.0f) + 1.0f), 2.0f) *
                                        std::min(m_m.specular, 1.0f),
                                    0.0f, 1.0f);
        auto* geometry = m_graph->create_node<ccl::GeometryNode>();
        auto* facing = m_graph->create_node<ccl::VectorMathNode>();
        facing->set_math_type(ccl::NODE_VECTOR_MATH_DOT_PRODUCT);
        m_graph->connect(geometry->output("Incoming"), facing->input("Vector1"));
        m_graph->connect(geometry->output("Normal"), facing->input("Vector2"));
        auto* grazing = m_graph->create_node<ccl::MathNode>(); // 1 − |cos|
        grazing->set_math_type(ccl::NODE_MATH_SUBTRACT);
        grazing->set_value1(1.0f);
        auto* absolute = m_graph->create_node<ccl::MathNode>();
        absolute->set_math_type(ccl::NODE_MATH_ABSOLUTE);
        m_graph->connect(facing->output("Value"), absolute->input("Value1"));
        m_graph->connect(absolute->output("Value"), grazing->input("Value2"));
        auto* fifth = m_graph->create_node<ccl::MathNode>();
        fifth->set_math_type(ccl::NODE_MATH_POWER);
        fifth->set_value2(5.0f);
        m_graph->connect(grazing->output("Value"), fifth->input("Value1"));
        auto* passed = m_graph->create_node<ccl::MathNode>(); // (1 − f0)(1 − grazing⁵)
        passed->set_math_type(ccl::NODE_MATH_MULTIPLY_ADD);
        passed->set_value2(-(1.0f - f0));
        passed->set_value3(1.0f - f0);
        m_graph->connect(fifth->output("Value"), passed->input("Value1"));
        auto* scaled = m_graph->create_node<ccl::VectorMathNode>();
        scaled->set_math_type(ccl::NODE_VECTOR_MATH_SCALE);
        scaled->set_vector1(ccl::make_float3(std::clamp(tint[0] * m_m.baseColor[0], 0.0f, 1.0f) * transmission,
                                            std::clamp(tint[1] * m_m.baseColor[1], 0.0f, 1.0f) * transmission,
                                            std::clamp(tint[2] * m_m.baseColor[2], 0.0f, 1.0f) * transmission));
        m_graph->connect(passed->output("Value"), scaled->input("Scale"));
        auto* through = m_graph->create_node<ccl::TransparentBsdfNode>();
        m_graph->connect(scaled->output("Vector"), through->input("Color"));
        auto* add = m_graph->create_node<ccl::AddClosureNode>();
        m_graph->connect(surface, add->input("Closure1"));
        m_graph->connect(through->output("BSDF"), add->input("Closure2"));
        surface = add->output("Closure");
        if (opacity < 1.0f || has(MapSlot::Opacity)) {
            auto* hole = m_graph->create_node<ccl::TransparentBsdfNode>();
            hole->set_color(ccl::make_float3(1.0f, 1.0f, 1.0f));
            auto* cut = m_graph->create_node<ccl::MixClosureNode>();
            linkScalar(cut, "Fac", opacity, MapSlot::Opacity);
            m_graph->connect(hole->output("BSDF"), cut->input("Closure1"));
            m_graph->connect(surface, cut->input("Closure2"));
            surface = cut->output("Closure");
        }
    } else if (transmission > 0.0f) {
        auto* through = m_graph->create_node<ccl::TransparentBsdfNode>();
        float shadow[3];
        for (int c = 0; c < 3; ++c) {
            shadow[c] = std::clamp(tint[c] * m_m.baseColor[c], 0.0f, 1.0f) * transmission;
        }
        through->set_color(toFloat3(shadow));
        auto* path = m_graph->create_node<ccl::LightPathNode>();
        auto* mix = m_graph->create_node<ccl::MixClosureNode>();
        m_graph->connect(path->output("Is Shadow Ray"), mix->input("Fac"));
        m_graph->connect(surface, mix->input("Closure1"));
        m_graph->connect(through->output("BSDF"), mix->input("Closure2"));
        surface = mix->output("Closure");
    }
    m_graph->connect(surface, m_graph->output()->input("Surface"));

    // --- The volume of a solid that tints by depth: absorption that leaves the material's tint
    // after its depth (Beer's law per channel). ---
    if (transmission > 0.0f && deep && !thin && !isWhite(m_m.transmissionColor)) {
        float sigma[3];
        float density = 0.0f;
        for (int c = 0; c < 3; ++c) {
            sigma[c] = -std::log(std::clamp(m_m.transmissionColor[c], 1.0e-3f, 1.0f)) / m_m.transmissionDepth;
            density = std::max(density, sigma[c]);
        }
        if (density > 0.0f) {
            auto* absorb = m_graph->create_node<ccl::AbsorptionVolumeNode>();
            absorb->set_color(ccl::make_float3(1.0f - sigma[0] / density, 1.0f - sigma[1] / density, 1.0f - sigma[2] / density));
            absorb->set_density(density * transmission);
            m_graph->connect(absorb->output("Volume"), m_graph->output()->input("Volume"));
        }
    }
    return std::move(m_graph);
}

/// @p material's graph into @p shader (a new shader, or one of the pool given another material).
void buildMaterial(ccl::Scene* scene, ImageSet& images, const Material& material, ccl::Shader* shader) {
    shader->name = ccl::ustring(material.name != nullptr ? material.name : "material");
    shader->set_graph(MaterialGraph(images, material).build());
    shader->tag_update(scene);
}

/// A graph that shades nothing and samples nothing: what a pooled shader no mesh uses is left with.
void buildIdleShader(ccl::Scene* scene, ccl::Shader* shader) {
    auto  graph = std::make_unique<ccl::ShaderGraph>();
    auto* diffuse = graph->create_node<ccl::DiffuseBsdfNode>();
    graph->connect(diffuse->output("BSDF"), graph->output()->input("Surface"));
    shader->set_graph(std::move(graph));
    shader->tag_update(scene);
}

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------

void buildMesh(ccl::Scene* scene, const Mesh& in, ccl::Shader* shader) {
    if (in.positions == nullptr || in.indices == nullptr || in.vertexCount == 0 || in.indexCount < 3) {
        return;
    }
    const int vertexCount = static_cast<int>(in.vertexCount);

    // The triangles whose indices are in range, each wound to agree with its vertex normals
    // (see WINDING in the file comment).
    std::vector<int> triangles;
    triangles.reserve(in.indexCount);
    for (uint32_t i = 0; i + 2 < in.indexCount; i += 3) {
        uint32_t a = in.indices[i];
        uint32_t b = in.indices[i + 1];
        uint32_t c = in.indices[i + 2];
        if (a >= in.vertexCount || b >= in.vertexCount || c >= in.vertexCount) {
            continue;
        }
        if (in.normals != nullptr) {
            const float* pa = in.positions + 3 * a;
            const float* pb = in.positions + 3 * b;
            const float* pc = in.positions + 3 * c;
            const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
            const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
            const float face[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                   e1[0] * e2[1] - e1[1] * e2[0]};
            float facing = 0.0f;
            for (const uint32_t v : {a, b, c}) {
                const float* n = in.normals + 3 * v;
                facing += face[0] * n[0] + face[1] * n[1] + face[2] * n[2];
            }
            if (facing < 0.0f) {
                std::swap(b, c);
            }
        }
        triangles.push_back(static_cast<int>(a));
        triangles.push_back(static_cast<int>(b));
        triangles.push_back(static_cast<int>(c));
    }
    const int triangleCount = static_cast<int>(triangles.size() / 3);
    if (triangleCount == 0) {
        return;
    }

    ccl::Mesh* mesh = scene->create_node<ccl::Mesh>();
    mesh->name = ccl::ustring(in.name != nullptr ? in.name : "mesh");
    ccl::array<ccl::Node*> usedShaders;
    usedShaders.push_back_slow(shader);
    mesh->set_used_shaders(usedShaders);

    mesh->resize_mesh(vertexCount, triangleCount);
    ccl::packed_float3* positions = mesh->get_position_for_write();
    for (int v = 0; v < vertexCount; ++v) {
        positions[v] = ccl::make_float3(in.positions[3 * v], in.positions[3 * v + 1], in.positions[3 * v + 2]);
    }
    std::copy(triangles.begin(), triangles.end(), mesh->get_triangles().data());
    std::fill_n(mesh->get_smooth().data(), triangleCount, true);
    std::fill_n(mesh->get_shader().data(), triangleCount, 0);
    mesh->tag_position_modified();
    mesh->tag_triangles_modified();
    mesh->tag_shader_modified();
    mesh->tag_smooth_modified();

    if (in.normals != nullptr) {
        ccl::Attribute* attribute = mesh->attributes.add(ccl::ATTR_STD_VERTEX_NORMAL);
        auto* normals = attribute->data_for_write<ccl::packed_normal>();
        for (int v = 0; v < vertexCount; ++v) {
            normals[v] = ccl::packed_normal(
                ccl::make_float3(in.normals[3 * v], in.normals[3 * v + 1], in.normals[3 * v + 2]));
        }
    }
    if (in.uvs != nullptr) {
        ccl::Attribute* attribute = mesh->attributes.add(ccl::ATTR_STD_UV);
        auto* uvs = attribute->data_for_write<ccl::float2>();
        for (int corner = 0; corner < triangleCount * 3; ++corner) {
            const int v = triangles[static_cast<std::size_t>(corner)];
            uvs[corner] = ccl::make_float2(in.uvs[2 * v], in.uvs[2 * v + 1]);
        }
    }
    if (in.uvs != nullptr && in.tangents != nullptr) {
        ccl::Attribute* tangentAttribute = mesh->attributes.add(ccl::ATTR_STD_UV_TANGENT);
        ccl::Attribute* signAttribute = mesh->attributes.add(ccl::ATTR_STD_UV_TANGENT_SIGN);
        auto* tangents = tangentAttribute->data_for_write<ccl::packed_float3>();
        auto* signs = signAttribute->data_for_write<float>();
        for (int corner = 0; corner < triangleCount * 3; ++corner) {
            const int    v = triangles[static_cast<std::size_t>(corner)];
            const float* t = in.tangents + 4 * v;
            tangents[corner] = ccl::make_float3(t[0], t[1], t[2]);
            signs[corner] = t[3] < 0.0f ? -1.0f : 1.0f;
        }
    }

    ccl::Object* object = scene->create_node<ccl::Object>();
    object->name = mesh->name;
    object->set_geometry(mesh);
    object->set_tfm(ccl::transform_identity());
}

/// The floor: one large quad at @p height that shows only the shadows cast on it.
ccl::Shader* buildFloorShader(ccl::Scene* scene) {
    ccl::Shader* shader = scene->create_node<ccl::Shader>();
    shader->name = ccl::ustring("floor");
    auto  graph = std::make_unique<ccl::ShaderGraph>();
    auto* diffuse = graph->create_node<ccl::DiffuseBsdfNode>();
    diffuse->set_color(ccl::make_float3(0.8f, 0.8f, 0.8f));
    graph->connect(diffuse->output("BSDF"), graph->output()->input("Surface"));
    shader->set_graph(std::move(graph));
    shader->tag_update(scene);
    return shader;
}

void buildShadowFloor(ccl::Scene* scene, float height, ccl::Shader* shader) {

    const float kHalf = 200.0f; // metres: well past any shadow the key light throws
    ccl::Mesh*  mesh = scene->create_node<ccl::Mesh>();
    mesh->name = ccl::ustring("floor");
    ccl::array<ccl::Node*> usedShaders;
    usedShaders.push_back_slow(shader);
    mesh->set_used_shaders(usedShaders);
    mesh->resize_mesh(4, 2);
    ccl::packed_float3* p = mesh->get_position_for_write();
    p[0] = ccl::make_float3(-kHalf, height, -kHalf);
    p[1] = ccl::make_float3(-kHalf, height, kHalf);
    p[2] = ccl::make_float3(kHalf, height, kHalf);
    p[3] = ccl::make_float3(kHalf, height, -kHalf);
    const int triangles[6] = {0, 1, 2, 0, 2, 3}; // (+Y is the front)
    std::copy(triangles, triangles + 6, mesh->get_triangles().data());
    std::fill_n(mesh->get_smooth().data(), 2, false);
    std::fill_n(mesh->get_shader().data(), 2, 0);
    mesh->tag_position_modified();
    mesh->tag_triangles_modified();
    mesh->tag_shader_modified();
    mesh->tag_smooth_modified();

    ccl::Object* object = scene->create_node<ccl::Object>();
    object->name = mesh->name;
    object->set_geometry(mesh);
    object->set_tfm(ccl::transform_identity());
    object->set_is_shadow_catcher(true);
    // Seen by the camera alone: the floor shows the shadows cast on it and does nothing else —
    // it bounces no light onto the scene, hides none of the environment from it and is in no
    // reflection, as in the viewport, whose environment lights from every side.
    object->set_visibility(ccl::PATH_RAY_VISIBILITY_CAMERA);
}

// ---------------------------------------------------------------------------------------------
// Lights and the world
// ---------------------------------------------------------------------------------------------

/// The shader every light of the scene emits through: white, at a strength of 1 — a light's
/// own strength carries its color and power. (Cycles's default light shader emits NOTHING.)
ccl::Shader* buildLightShader(ccl::Scene* scene) {
    ccl::Shader* shader = scene->create_node<ccl::Shader>();
    shader->name = ccl::ustring("light");
    auto  graph = std::make_unique<ccl::ShaderGraph>();
    auto* emission = graph->create_node<ccl::EmissionNode>();
    emission->set_color(ccl::make_float3(1.0f, 1.0f, 1.0f));
    emission->set_strength(1.0f);
    graph->connect(emission->output("Emission"), graph->output()->input("Surface"));
    shader->set_graph(std::move(graph));
    shader->tag_update(scene);
    return shader;
}

/// A light object's frame as Cycles's object transform: it shines along its -Z (the third
/// COLUMN points back at where the light comes from — ccl::make_transform_frame puts the axes
/// in the ROWS: the inverse), and its X is level — an area light's width — wherever the light
/// does not shine straight up or down (the application draws its lights by the same rule:
/// scenelight.h's lightFrame).
ccl::Transform lightFrame(const ccl::float3& shines, const ccl::float3& position) {
    const ccl::float3 z = -shines;
    const ccl::float3 x = std::abs(z.y) < 0.999f ? ccl::normalize(ccl::cross(ccl::make_float3(0.0f, 1.0f, 0.0f), z))
                                                 : ccl::make_float3(1.0f, 0.0f, 0.0f);
    const ccl::float3 y = ccl::cross(z, x);
    return ccl::make_transform(x.x, y.x, z.x, position.x, x.y, y.y, z.y, position.y, x.z, y.z, z.z, position.z);
}

void buildLight(ccl::Scene* scene, const Light& in, ccl::Shader* lightShader) {
    if (in.strength <= 0.0f) {
        return;
    }
    ccl::float3 shines = ccl::safe_normalize(toFloat3(in.direction));
    if (ccl::len(shines) < 0.5f) {
        shines = ccl::make_float3(0.0f, -1.0f, 0.0f);
    }
    const float kPi = 3.14159265f;

    // The strength: an irradiance — a sun's as it is; a point or spot light's a metre away (its
    // radiant intensity: a 4π-th of the flux Cycles asks for); an area light's a metre away on
    // its axis (a Lambertian face's intensity there: a π-th of its flux). See renderapi::Light.
    ccl::Light* light = nullptr;
    float       flux = in.strength;
    switch (in.type) {
    case LightType::Point: {
        ccl::PointLight* point = scene->create_node<ccl::PointLight>();
        point->set_radius(std::max(in.radius, 0.0f));
        flux *= 4.0f * kPi;
        light = point;
        break;
    }
    case LightType::Spot: {
        ccl::SpotLight* spot = scene->create_node<ccl::SpotLight>();
        spot->set_radius(std::max(in.radius, 0.0f));
        spot->set_angle(std::clamp(in.angle, 0.0175f, 3.1241f)); // 1 .. 179 degrees
        spot->set_smooth(std::clamp(in.blend, 0.0f, 1.0f));
        flux *= 4.0f * kPi;
        light = spot;
        break;
    }
    case LightType::Area: {
        ccl::AreaLight* area = scene->create_node<ccl::AreaLight>();
        area->set_sizeu(std::max(in.sizeU, 1.0e-3f));
        area->set_sizev(std::max(in.sizeV, 1.0e-3f));
        area->set_ellipse(false);
        flux *= kPi;
        light = area;
        break;
    }
    default: {
        ccl::SunLight* sun = scene->create_node<ccl::SunLight>();
        sun->set_angle(std::max(in.angle, 0.0f));
        light = sun;
        break;
    }
    }
    light->set_strength(toFloat3(in.color) * flux);
    light->set_cast_shadow(in.castShadow != 0);
    light->set_use_mis(true);
    ccl::array<ccl::Node*> usedShaders;
    usedShaders.push_back_slow(lightShader);
    light->set_used_shaders(usedShaders);

    ccl::Object* object = scene->create_node<ccl::Object>();
    object->set_tfm(lightFrame(shines, toFloat3(in.position)));
    object->set_visibility(ccl::PATH_RAY_VISIBILITY_ALL & ~ccl::PATH_RAY_VISIBILITY_CAMERA);
    object->set_is_shadow_catcher(true); // (see FLOOR in the file comment)
    object->set_geometry(light);
}

/// The world shader: the environment lights the scene, and the camera sees the backdrop.
void buildWorld(ccl::Scene* scene, ImageSet& images, const Environment& env, const Settings& settings) {
    auto graph = std::make_unique<ccl::ShaderGraph>();

    // The environment's radiance along a direction. The panorama is laid out the application's
    // way — u = atan2(x, -z) / 2π, the zenith +Y (EnvironmentImage::sample) — and Cycles's lookup
    // reads u = 1/2 - atan2(y, x) / 2π with the zenith +Z: the direction (x, y, z) is looked up
    // as (z, x, y), a cyclic permutation (a rotation, so nothing is mirrored). The viewport's
    // rotation turns the lookup about the vertical, which is the permuted Z.
    ccl::ShaderOutput* radiance = nullptr;
    const bool         hasImage = images.valid(env.image);
    const bool         hasFile = env.file != nullptr && env.file[0] != '\0';
    if (hasImage || hasFile) {
        auto* geometry = graph->create_node<ccl::GeometryNode>();
        auto* separate = graph->create_node<ccl::SeparateXYZNode>();
        auto* combine = graph->create_node<ccl::CombineXYZNode>();
        graph->connect(geometry->output("Position"), separate->input("Vector"));
        graph->connect(separate->output("Z"), combine->input("X"));
        graph->connect(separate->output("X"), combine->input("Y"));
        graph->connect(separate->output("Y"), combine->input("Z"));
        auto* mapping = graph->create_node<ccl::MappingNode>();
        mapping->set_mapping_type(ccl::NODE_MAPPING_TYPE_POINT);
        // The viewport samples along rotateY(d, a) = (c·x + s·z, y, -s·x + c·z); permuted, that is
        // the lookup vector turned about Z by +a.
        mapping->set_rotation(ccl::make_float3(0.0f, 0.0f, env.rotation));
        graph->connect(combine->output("Vector"), mapping->input("Vector"));

        auto* texture = graph->create_node<ccl::EnvironmentTextureNode>();
        texture->set_projection(ccl::NODE_ENVIRONMENT_EQUIRECTANGULAR);
        texture->set_interpolation(ccl::INTERPOLATION_LINEAR);
        if (hasImage) {
            texture->set_colorspace(ImageSet::colorSpaceOf(images.image(env.image)));
            texture->set_alpha_type(ccl::IMAGE_ALPHA_IGNORE);
            // (Cycles's equirectangular v = 1 is the zenith and an application image's row 0 is
            // its top: the rows are flipped at load.)
            ccl::ImageParams params;
            params.colorspace = ImageSet::colorSpaceOf(images.image(env.image));
            params.alpha_type = ccl::IMAGE_ALPHA_IGNORE;
            params.extension = ccl::EXTENSION_REPEAT;
            texture->handle = scene->image_manager->add_image(
                std::make_unique<MemoryImageLoader>(images.image(env.image), /*flipRows=*/true), params);
        } else {
            texture->set_filename(ccl::ustring(env.file));
            texture->set_colorspace(ccl::u_colorspace_auto);
        }
        graph->connect(mapping->output("Vector"), texture->input("Vector"));
        radiance = texture->output("Color");
    }

    // What lights the scene: the environment × its strength, plus the uniform ambient fill.
    auto* light = graph->create_node<ccl::BackgroundNode>();
    light->set_strength(std::max(env.strength, 0.0f));
    if (radiance != nullptr) {
        graph->connect(radiance, light->input("Color"));
    } else {
        light->set_color(toFloat3(env.color));
    }
    ccl::ShaderOutput* lighting = light->output("Background");
    if (env.ambient > 0.0f) {
        auto* ambient = graph->create_node<ccl::BackgroundNode>();
        ambient->set_color(ccl::make_float3(1.0f, 1.0f, 1.0f));
        ambient->set_strength(env.ambient);
        auto* add = graph->create_node<ccl::AddClosureNode>();
        graph->connect(lighting, add->input("Closure1"));
        graph->connect(ambient->output("Background"), add->input("Closure2"));
        lighting = add->output("Closure");
    }

    // What the camera sees: the environment itself, or a flat backdrop. (Only CAMERA rays branch:
    // a light sample is shaded with no path flags at all, so anything else would light the same
    // surface differently by how the light was found.)
    auto* backdrop = graph->create_node<ccl::BackgroundNode>();
    if (env.backdrop == 1 && radiance != nullptr) {
        graph->connect(radiance, backdrop->input("Color"));
        backdrop->set_strength(std::max(env.strength * env.backdropStrength, 0.0f));
    } else {
        backdrop->set_color(toFloat3(env.backdropColor));
        backdrop->set_strength(1.0f);
    }
    auto* path = graph->create_node<ccl::LightPathNode>();
    auto* mix = graph->create_node<ccl::MixClosureNode>();
    graph->connect(path->output("Is Camera Ray"), mix->input("Fac"));
    graph->connect(lighting, mix->input("Closure1"));
    graph->connect(backdrop->output("Background"), mix->input("Closure2"));
    graph->connect(mix->output("Closure"), graph->output()->input("Surface"));

    ccl::Shader* shader = scene->default_background;
    shader->set_graph(std::move(graph));
    shader->tag_update(scene);

    scene->background->set_transparent(settings.transparentBackground != 0);

    // A panorama is SAMPLED AS A LIGHT (the background light: an importance map built from the
    // world shader): found by bounce rays alone, a panorama's sun — a few bright texels — is
    // noise that never settles. A uniform environment needs none (Cycles would disable it).
    if (radiance != nullptr) {
        ccl::BackgroundLight* light = scene->create_node<ccl::BackgroundLight>();
        light->set_use_mis(true);
        // (0 = the panorama's own resolution. An interactive render rebuilds the map whenever the
        // lighting changes — a light dragged, a dial turned: bounded there.)
        light->set_map_resolution(settings.interactive != 0 ? 1024 : 0);
        ccl::array<ccl::Node*> usedShaders;
        usedShaders.push_back_slow(scene->default_background);
        light->set_used_shaders(usedShaders);
        ccl::Object* object = scene->create_node<ccl::Object>();
        object->set_tfm(ccl::transform_identity());
        object->set_is_shadow_catcher(true); // (see FLOOR in the file comment)
        object->set_geometry(light);
    }
}

// ---------------------------------------------------------------------------------------------
// Camera and render settings
// ---------------------------------------------------------------------------------------------

void buildCamera(ccl::Scene* scene, const Camera& in, const Settings& settings) {
    ccl::Camera* camera = scene->camera;
    camera->set_full_width(settings.width);
    camera->set_full_height(settings.height);

    // Column-major camera -> world; the Z column negated (see CAMERA in the file comment).
    const float* m = in.cameraToWorld;
    camera->set_matrix(ccl::make_transform(m[0], m[4], -m[8], m[12], m[1], m[5], -m[9], m[13], m[2], m[6], -m[10],
                                           m[14]));
    const float aspect = static_cast<float>(settings.width) / static_cast<float>(std::max(settings.height, 1));
    if (in.orthoHeight > 0.0f) {
        const float half = 0.5f * in.orthoHeight;
        camera->set_camera_type(ccl::CAMERA_ORTHOGRAPHIC);
        camera->set_viewplane_left(-half * aspect);
        camera->set_viewplane_right(half * aspect);
        camera->set_viewplane_bottom(-half);
        camera->set_viewplane_top(half);
    } else {
        camera->set_camera_type(ccl::CAMERA_PERSPECTIVE);
        camera->set_fov(in.fovY);
        camera->set_viewplane_left(-aspect);
        camera->set_viewplane_right(aspect);
        camera->set_viewplane_bottom(-1.0f);
        camera->set_viewplane_top(1.0f);
    }
    camera->set_nearclip(std::max(in.nearClip, 1.0e-4f));
    camera->set_farclip(std::max(in.farClip, in.nearClip + 1.0f));
    camera->need_flags_update = true;
    // (The camera is left MODIFIED: Cycles's own scene update computes it and sends it to the
    // device. Updated here, it is no longer "modified" — and a camera that is all that changed,
    // an interactive render's orbit, was then no reason for the scene to update at all: the
    // picture stayed where it was.)
    (void)scene;
}

} // namespace

SceneBuilder::SceneBuilder(ccl::Scene* scene) : m_scene(scene) {}

void SceneBuilder::clear() {
    // Every object, and every geometry — the meshes and the lights — it had. (The shaders stay:
    // see UPDATES in the file comment.)
    ccl::set<ccl::Object*> objects;
    for (std::size_t i = 0; i < m_scene->objects.size(); ++i) {
        objects.insert(m_scene->objects[i]);
    }
    if (!objects.empty()) {
        m_scene->delete_nodes(objects);
    }
    ccl::set<ccl::Geometry*> geometry;
    for (std::size_t i = 0; i < m_scene->geometry.size(); ++i) {
        geometry.insert(m_scene->geometry[i]);
    }
    if (!geometry.empty()) {
        m_scene->delete_nodes(geometry);
    }
    m_lightingObjects.clear();
    m_lightingGeometry.clear();
}

void SceneBuilder::clearLighting() {
    if (!m_lightingObjects.empty()) {
        m_scene->delete_nodes(ccl::set<ccl::Object*>(m_lightingObjects.begin(), m_lightingObjects.end()));
    }
    if (!m_lightingGeometry.empty()) {
        m_scene->delete_nodes(ccl::set<ccl::Geometry*>(m_lightingGeometry.begin(), m_lightingGeometry.end()));
    }
    m_lightingObjects.clear();
    m_lightingGeometry.clear();
}

void SceneBuilder::buildLighting(const Scene& in, const Settings& settings) {
    ccl::Scene* scene = m_scene;
    // (Nothing is deleted between here and the end: what this makes is what is appended.)
    const std::size_t objectsBefore = scene->objects.size();
    const std::size_t geometryBefore = scene->geometry.size();

    ImageSet images(scene, in);
    if (in.floorShadows != 0) {
        if (m_floorShader == nullptr) {
            m_floorShader = buildFloorShader(scene);
        }
        buildShadowFloor(scene, in.floorHeight, m_floorShader);
    }
    if (in.lightCount > 0 && m_lightShader == nullptr) {
        m_lightShader = buildLightShader(scene);
    }
    for (uint32_t i = 0; i < in.lightCount; ++i) {
        buildLight(scene, in.lights[i], m_lightShader);
    }
    buildWorld(scene, images, in.environment, settings);

    for (std::size_t i = objectsBefore; i < scene->objects.size(); ++i) {
        m_lightingObjects.push_back(scene->objects[i]);
    }
    for (std::size_t i = geometryBefore; i < scene->geometry.size(); ++i) {
        m_lightingGeometry.push_back(scene->geometry[i]);
    }
}

void SceneBuilder::lighting(const Scene& in, const Settings& settings) {
    clearLighting();
    buildLighting(in, settings);
}

void SceneBuilder::camera(const Camera& in, const Settings& settings) {
    buildCamera(m_scene, in, settings);
}

bool SceneBuilder::build(const Scene& in, const Settings& settings, std::string& error) {
    if (settings.width <= 0 || settings.height <= 0) {
        error = "The image size is not valid.";
        return false;
    }
    ccl::Scene* scene = m_scene;
    clear();

    ImageSet images(scene, in);

    // Materials, each into a shader of the pool — the new graphs BEFORE anything of the old is
    // let go of, so an image both sample stays loaded. (A mesh that names none, or one out of
    // range, takes Cycles's default surface.)
    while (m_materialShaders.size() < in.materialCount) {
        m_materialShaders.push_back(scene->create_node<ccl::Shader>());
    }
    for (uint32_t i = 0; i < in.materialCount; ++i) {
        buildMaterial(scene, images, in.materials[i], m_materialShaders[i]);
    }
    for (std::size_t i = in.materialCount; i < std::min(m_idleFrom, m_materialShaders.size()); ++i) {
        buildIdleShader(scene, m_materialShaders[i]); // (in use by the build before: let go of its images)
    }
    m_idleFrom = in.materialCount;

    for (uint32_t i = 0; i < in.meshCount; ++i) {
        const Mesh&  mesh = in.meshes[i];
        ccl::Shader* shader = (mesh.material >= 0 && static_cast<uint32_t>(mesh.material) < in.materialCount)
                                  ? m_materialShaders[static_cast<std::size_t>(mesh.material)]
                                  : scene->default_surface;
        buildMesh(scene, mesh, shader);
    }
    buildLighting(in, settings); // the floor, the lights, the world
    buildCamera(scene, in.camera, settings);

    // The film: a floor that catches shadows over an opaque backdrop. (The exposure is applied
    // to the finished pixels: see FrameOutputDriver.)
    scene->film->set_exposure(1.0f);
    // (Under a transparent film the floor leaves no trace — alpha is the scene's coverage — unless
    // the caller composites the picture over a plate of its own and asks for the shadows as
    // coverage: Cycles's approximate catcher then puts 1 - the shadow's ratio into the alpha.)
    scene->film->set_use_approximate_shadow_catcher(settings.transparentBackground == 0 || settings.shadowAlpha != 0);

    // The integrator. Caustics through glass are left out (see GLASS in the file comment).
    ccl::Integrator* integrator = scene->integrator;
    const int        bounces = std::clamp(settings.maxBounces, 1, 64);
    integrator->set_max_bounce(bounces);
    integrator->set_max_diffuse_bounce(std::min(bounces, 4));
    integrator->set_max_glossy_bounce(std::min(bounces, 4));
    integrator->set_max_transmission_bounce(bounces);
    integrator->set_transparent_max_bounce(std::max(bounces, 16));
    integrator->set_caustics_refractive(false);
    integrator->set_use_adaptive_sampling(true);
    integrator->set_adaptive_threshold(0.01f);
    return true;
}

} // namespace posecycles
