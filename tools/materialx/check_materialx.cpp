/**
 * @file check_materialx.cpp
 * @brief The MaterialX material reader on documents of its own (CTest `materialx-check`), and
 *        a report of what a REAL file reads as.
 *
 * src/viewport/scene/materialxfile.h turns a MaterialX document into PoseStudio materials:
 * values and image files. Checked here on small documents written in this file — each of the
 * four shading models; a value, an image, an image through a nodegraph; which slot a map goes
 * to and how strong it is (normalmap, bump); which channel it is read from (extract,
 * separate3); a constant it is multiplied by; its tiling; the file prefix and the color space
 * as inherited; the older material / shaderref form; a file with shading nodes and no
 * material; an include; a displacement shader; and what is said of what cannot be carried —
 * and on what is NOT a material file: broken markup, another kind of XML, a shading model the
 * reader does not know.
 *
 *     PoseStudioMaterialXCheck                  the checks; exit code = failures
 *     PoseStudioMaterialXCheck --report <file>  what that file's materials read as (includes
 *                                               are read relative to it)
 *
 * No Qt, no device.
 */

#include "materialxfile.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using pose::MaterialMap;
using pose::MaterialXDocument;
using pose::MaterialXMaterial;

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

bool about(float a, float b, float tolerance = 1e-4f) {
    return std::fabs(a - b) <= tolerance;
}

bool about(const glm::vec3& a, float r, float g, float b, float tolerance = 1e-4f) {
    return about(a.x, r, tolerance) && about(a.y, g, tolerance) && about(a.z, b, tolerance);
}

/// The texture of @p slot, or null.
const pose::MaterialXTexture* map(const MaterialXMaterial& m, MaterialMap slot) {
    for (const pose::MaterialXTexture& t : m.textures) {
        if (t.slot == slot) {
            return &t;
        }
    }
    return nullptr;
}

bool hasMap(const MaterialXMaterial& m, MaterialMap slot, const char* file, int channel = 0) {
    const pose::MaterialXTexture* t = map(m, slot);
    return t != nullptr && t->file == file && t->channel == channel;
}

bool noted(const MaterialXMaterial& m, const char* fragment) {
    for (const std::string& note : m.notes) {
        if (note.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

MaterialXDocument read(const std::string& text, bool* ok = nullptr, std::string* error = nullptr, const pose::MaterialXInclude& include = {}) {
    MaterialXDocument doc;
    std::string       why;
    const bool        good = pose::readMaterialXText(text, doc, why, include);
    if (ok != nullptr) {
        *ok = good;
    }
    if (error != nullptr) {
        *error = why;
    }
    return doc;
}

std::string wrap(const std::string& body, const std::string& attributes = "version=\"1.38\" colorspace=\"lin_rec709\"") {
    return "<?xml version=\"1.0\"?>\n<materialx " + attributes + ">\n" + body + "</materialx>\n";
}

void checkValues() {
    std::printf("-- standard_surface: values\n");
    bool              ok = false;
    const MaterialXDocument doc = read(wrap(R"(
  <!-- a comment with a <tag> in it -->
  <standard_surface name="SR_gold" type="surfaceshader">
    <input name="base" type="float" value="1" />
    <input name="base_color" type="color3" value="0.944, 0.776, 0.373" />
    <input name="specular" type="float" value="0.5" />
    <input name="specular_color" type="color3" value="0.998, 0.981, 0.751" />
    <input name="specular_roughness" type="float" value="0.02" />
    <input name="specular_IOR" type="float" value="1.4" />
    <input name="specular_anisotropy" type="float" value="0.5" />
    <input name="specular_rotation" type="float" value="0.25" />
    <input name="metalness" type="float" value="1" />
    <input name="coat" type="float" value="0.4" />
    <input name="coat_roughness" type="float" value="0.05" />
    <input name="coat_IOR" type="float" value="1.6" />
    <input name="sheen" type="float" value="0.3" />
    <input name="sheen_color" type="color3" value="0.2, 0.4, 0.6" />
    <input name="thin_film_thickness" type="float" value="550" />
    <input name="emission" type="float" value="2" />
    <input name="emission_color" type="color3" value="1, 0.5, 0.25" />
    <input name="transmission" type="float" value="0.1" />
    <input name="thin_walled" type="boolean" value="true" />
    <input name="subsurface" type="float" value="0.2" />
    <input name="subsurface_scale" type="float" value="0.003" />
    <input name="opacity" type="color3" value="0.5, 0.5, 0.5" />
  </standard_surface>
  <surfacematerial name="Gold" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_gold" />
  </surfacematerial>
)"), &ok);
    check(ok && doc.version == "1.38" && doc.materials.size() == 1 && doc.materials[0].name == "Gold" && doc.materials[0].model == "standard_surface",
          "a surfacematerial and its standard_surface are one material, under the material's name");
    if (doc.materials.empty()) {
        return;
    }
    const pose::MaterialParams& p = doc.materials[0].params;
    check(about(p.baseColor, 0.944f, 0.776f, 0.373f) && about(p.metalness, 1.0f) && about(p.roughness, 0.02f) && about(p.lobe1Roughness, 0.02f) &&
              about(p.lobe2Roughness, 0.02f) && about(p.lobeRatio, 1.0f),
          "base color, metalness and roughness (one lobe)");
    check(about(p.specular, 0.5f) && about(p.specularColor, 0.998f, 0.981f, 0.751f) && about(p.ior, 1.4f) && about(p.anisotropy, 0.5f) &&
              about(p.anisotropyRotation, 90.0f),
          "the specular layer: weight, color, index, anisotropy, and a quarter turn as 90 degrees");
    check(about(p.topCoat, 0.4f) && about(p.topCoatRoughness, 0.05f) && about(p.coatIor, 1.6f) && about(p.sheen, 0.3f) &&
              about(p.sheenColor, 0.2f, 0.4f, 0.6f) && about(p.sheenRoughness, 0.3f),
          "coat and sheen (the sheen roughness the file leaves out is the model's 0.3)");
    check(about(p.thinFilm, 1.0f) && about(p.thinFilmThickness, 550.0f) && about(p.emission, 2.0f) && about(p.emissionColor, 1.0f, 0.5f, 0.25f),
          "a film with a thickness is on; the emission weight and color");
    check(about(p.transmission, 0.1f) && p.thinWalled && about(p.translucency, 0.2f) && about(p.subsurfaceRadius, 0.3f) && about(p.opacity, 0.5f),
          "transmission, thin walled, the subsurface weight, its scale in centimetres, a color opacity as its mean");
    check(doc.materials[0].textures.empty() && doc.materials[0].notes.empty(), "no maps, nothing to say");

    // What a file leaves out is the MODEL's value.
    const MaterialXDocument bare = read(wrap(R"(
  <standard_surface name="Plain" type="surfaceshader" />
)"), &ok);
    check(ok && bare.materials.size() == 1 && bare.materials[0].name == "Plain", "a shading node with no material beside it is a material by its own name");
    if (!bare.materials.empty()) {
        const pose::MaterialParams& d = bare.materials[0].params;
        check(about(d.baseColor, 0.8f, 0.8f, 0.8f) && about(d.roughness, 0.2f) && about(d.metalness, 0.0f) && about(d.specular, 1.0f) &&
                  about(d.emission, 0.0f) && about(d.thinFilm, 0.0f) && about(d.translucency, 0.0f) && about(d.topCoat, 0.0f) && about(d.opacity, 1.0f),
              "an empty standard_surface is the model's default: grey 0.8, roughness 0.2, no emission, film, coat or subsurface");
    }

    // A color said to be sRGB is taken to linear; one in the working space is as written.
    const MaterialXDocument srgb = read(wrap(R"(
  <standard_surface name="A" type="surfaceshader">
    <input name="base" type="float" value="1" />
    <input name="base_color" type="color3" value="0.5, 0.5, 0.5" colorspace="srgb_texture" />
    <input name="coat_color" type="color3" value="0.5, 0.5, 0.5" />
  </standard_surface>
)"), &ok);
    check(ok && srgb.materials.size() == 1 && about(srgb.materials[0].params.baseColor, 0.21404f, 0.21404f, 0.21404f, 1e-3f) &&
              about(srgb.materials[0].params.coatColor, 0.5f, 0.5f, 0.5f),
          "a color given in sRGB is made linear; a color in the working space is as written");
}

void checkTextures() {
    std::printf("-- images: slots, channels, factors, placement\n");
    bool              ok = false;
    const MaterialXDocument doc = read(wrap(R"(
  <nodegraph name="NG" fileprefix="maps/">
    <tiledimage name="color" type="color3">
      <input name="file" type="filename" value="wood_color.jpg" colorspace="srgb_texture" />
      <input name="uvtiling" type="vector2" value="4.0, 2.0" />
      <input name="uvoffset" type="vector2" value="0.25, 0.0" />
    </tiledimage>
    <image name="orm" type="color3">
      <input name="file" type="filename" value="wood_orm.png" />
    </image>
    <extract name="rough" type="float">
      <input name="in" type="color3" nodename="orm" />
      <input name="index" type="integer" value="1" />
    </extract>
    <separate3 name="split" type="multioutput">
      <input name="in" type="color3" nodename="orm" />
    </separate3>
    <multiply name="rough_scaled" type="float">
      <input name="in1" type="float" nodename="rough" />
      <input name="in2" type="float" value="0.8" />
    </multiply>
    <image name="nrm" type="vector3">
      <input name="file" type="filename" value="wood_normal.png" fileprefix="normals/" />
    </image>
    <normalmap name="nmap" type="vector3">
      <input name="in" type="vector3" nodename="nrm" />
      <input name="scale" type="float" value="1.5" />
    </normalmap>
    <output name="out_color" type="color3" nodename="color" />
    <output name="out_rough" type="float" nodename="rough_scaled" />
    <output name="out_metal" type="float" nodename="split" output="outb" />
    <output name="out_ao" type="float" nodename="split" output="outr" />
    <output name="out_normal" type="vector3" nodename="nmap" />
  </nodegraph>
  <standard_surface name="SR" type="surfaceshader">
    <input name="base" type="float" value="1" />
    <input name="base_color" type="color3" nodegraph="NG" output="out_color" />
    <input name="specular_roughness" type="float" nodegraph="NG" output="out_rough" />
    <input name="metalness" type="float" nodegraph="NG" output="out_metal" />
    <input name="normal" type="vector3" nodegraph="NG" output="out_normal" />
    <input name="specular_IOR" type="float" nodegraph="NG" output="out_ao" />
  </standard_surface>
  <displacement name="disp" type="displacementshader">
    <input name="displacement" type="float" nodename="height" />
    <input name="scale" type="float" value="0.02" />
  </displacement>
  <image name="height" type="float">
    <input name="file" type="filename" value="wood_height.exr" />
  </image>
  <surfacematerial name="Wood" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR" />
    <input name="displacementshader" type="displacementshader" nodename="disp" />
  </surfacematerial>
)"), &ok);
    check(ok && doc.materials.size() == 1, "a material whose inputs come out of a nodegraph");
    if (doc.materials.empty()) {
        return;
    }
    const MaterialXMaterial&    m = doc.materials[0];
    const pose::MaterialParams& p = m.params;
    check(hasMap(m, MaterialMap::BaseColor, "maps/wood_color.jpg") && about(p.baseColor, 1.0f, 1.0f, 1.0f),
          "the base color map, its file under the nodegraph's prefix; the tint beside it white");
    check(about(p.uvTiling.x, 4.0f) && about(p.uvTiling.y, 2.0f) && about(p.uvOffset.x, -0.25f) && about(p.uvOffset.y, 0.0f),
          "a tiled image's tiling and offset are the material's");
    check(hasMap(m, MaterialMap::Roughness, "maps/wood_orm.png", 1) && about(p.roughness, 0.8f),
          "roughness from the GREEN of a packed image (extract), times the constant it is multiplied by");
    check(hasMap(m, MaterialMap::Metalness, "maps/wood_orm.png", 2) && about(p.metalness, 1.0f), "metalness from its BLUE (separate3's outb)");
    check(hasMap(m, MaterialMap::Normal, "normals/wood_normal.png") && about(p.normalStrength, 1.5f) && map(m, MaterialMap::Bump) == nullptr,
          "a normalmap node's image is the normal map, at the node's scale; the nearest prefix (the input's own) wins");
    check(hasMap(m, MaterialMap::Displacement, "wood_height.exr") && about(p.displacementStrength, 2.0f) && about(p.displacementMidLevel, 0.0f),
          "the displacement shader's map (outside the nodegraph: no prefix), its scale in centimetres from black");
    check(about(p.ior, 1.5f) && noted(m, "index of refraction"), "an image on an input the material has no map for is said, and the value keeps its default");

    // A height read as relief, a file read from its alpha, and a placement node.
    const MaterialXDocument more = read(wrap(R"(
  <texcoord name="uv" type="vector2" />
  <multiply name="uv4" type="vector2">
    <input name="in1" type="vector2" nodename="uv" />
    <input name="in2" type="float" value="3" />
  </multiply>
  <image name="leaf" type="color4">
    <input name="file" type="filename" value="leaf.png" />
    <input name="texcoord" type="vector2" nodename="uv4" />
  </image>
  <separate4 name="rgba" type="multioutput">
    <input name="in" type="color4" nodename="leaf" />
  </separate4>
  <image name="h" type="float">
    <input name="file" type="filename" value="leaf_height.png" />
  </image>
  <bump name="relief" type="vector3">
    <input name="height" type="float" nodename="h" />
    <input name="scale" type="float" value="0.5" />
  </bump>
  <noise2d name="speckle" type="float" />
  <standard_surface name="Leaf" type="surfaceshader">
    <input name="base_color" type="color3" nodename="leaf" />
    <input name="opacity" type="color3" nodename="rgba" output="outa" />
    <input name="normal" type="vector3" nodename="relief" />
    <input name="specular_roughness" type="float" nodename="speckle" />
  </standard_surface>
)"), &ok);
    check(ok && more.materials.size() == 1, "top-level nodes (no nodegraph)");
    if (!more.materials.empty()) {
        const MaterialXMaterial& leaf = more.materials[0];
        check(hasMap(leaf, MaterialMap::BaseColor, "leaf.png") && hasMap(leaf, MaterialMap::Opacity, "leaf.png", 3),
              "the base color image, and its ALPHA as the cutout (separate4's outa)");
        check(about(leaf.params.uvTiling.x, 3.0f) && about(leaf.params.uvTiling.y, 3.0f), "a texcoord multiplied by 3 is a tiling of 3");
        check(hasMap(leaf, MaterialMap::Bump, "leaf_height.png") && about(leaf.params.bumpStrength, 0.5f) && map(leaf, MaterialMap::Normal) == nullptr,
              "a bump node's height image is the bump map, at the node's scale");
        check(about(leaf.params.roughness, 0.2f) && noted(leaf, "noise2d") && map(leaf, MaterialMap::Roughness) == nullptr,
              "a procedural node has no image: the input keeps the model's default, and it is said");
        check(about(leaf.params.baseColor, 0.8f, 0.8f, 0.8f), "the model's base weight 0.8 still scales a mapped base color");
    }
}

void checkModels() {
    std::printf("-- the other shading models, and the older form\n");
    bool              ok = false;
    const MaterialXDocument open = read(wrap(R"(
  <open_pbr_surface name="SR" type="surfaceshader">
    <input name="base_weight" type="float" value="0.5" />
    <input name="base_color" type="color3" value="0.8, 0.2, 0.2" />
    <input name="base_metalness" type="float" value="0.25" />
    <input name="specular_roughness" type="float" value="0.4" />
    <input name="specular_ior" type="float" value="1.33" />
    <input name="fuzz_weight" type="float" value="0.6" />
    <input name="fuzz_color" type="color3" value="1, 1, 1" />
    <input name="coat_weight" type="float" value="1" />
    <input name="thin_film_weight" type="float" value="0.5" />
    <input name="thin_film_thickness" type="float" value="0.4" />
    <input name="emission_luminance" type="float" value="500" />
    <input name="emission_color" type="color3" value="1, 1, 0" />
    <input name="geometry_opacity" type="float" value="0.9" />
  </open_pbr_surface>
  <surfacematerial name="Open" type="material"><input name="surfaceshader" type="surfaceshader" nodename="SR" /></surfacematerial>
)", "version=\"1.39\""), &ok);
    check(ok && open.materials.size() == 1 && open.materials[0].model == "open_pbr_surface" && open.version == "1.39", "an open_pbr_surface material");
    if (!open.materials.empty()) {
        const pose::MaterialParams& p = open.materials[0].params;
        check(about(p.baseColor, 0.4f, 0.1f, 0.1f) && about(p.metalness, 0.25f) && about(p.roughness, 0.4f) && about(p.ior, 1.33f),
              "OpenPBR base (weight times color), metalness, roughness, index");
        check(about(p.sheen, 0.6f) && about(p.topCoat, 1.0f) && about(p.coatIor, 1.6f) && about(p.topCoatRoughness, 0.0f) && about(p.thinFilm, 0.5f) &&
                  about(p.thinFilmThickness, 400.0f) && about(p.emission, 0.5f) && about(p.opacity, 0.9f),
              "fuzz as sheen, the coat with OpenPBR's own defaults, a film in micrometres as nanometres, 500 nits as half the luminance factor");
    }

    const MaterialXDocument gltf = read(wrap(R"(
  <gltf_colorimage name="base" type="multioutput">
    <input name="file" type="filename" value="helmet_albedo.png" colorspace="srgb_texture" />
    <input name="color" type="color3" value="0.5, 0.6, 0.7" />
  </gltf_colorimage>
  <gltf_image name="orm" type="vector3">
    <input name="file" type="filename" value="helmet_orm.png" />
  </gltf_image>
  <separate3 name="orm_split" type="multioutput"><input name="in" type="vector3" nodename="orm" /></separate3>
  <gltf_normalmap name="nrm" type="vector3"><input name="file" type="filename" value="helmet_normal.png" /></gltf_normalmap>
  <gltf_pbr name="SR" type="surfaceshader">
    <input name="base_color" type="color3" nodename="base" output="outcolor" />
    <input name="alpha" type="float" nodename="base" output="outa" />
    <input name="occlusion" type="float" nodename="orm_split" output="outx" />
    <input name="roughness" type="float" nodename="orm_split" output="outy" />
    <input name="metallic" type="float" nodename="orm_split" output="outz" />
    <input name="normal" type="vector3" nodename="nrm" />
    <input name="emissive" type="color3" value="1, 1, 1" />
    <input name="emissive_strength" type="float" value="3" />
    <input name="clearcoat" type="float" value="0.7" />
  </gltf_pbr>
  <surfacematerial name="Helmet" type="material"><input name="surfaceshader" type="surfaceshader" nodename="SR" /></surfacematerial>
)"), &ok);
    check(ok && gltf.materials.size() == 1 && gltf.materials[0].model == "gltf_pbr", "a gltf_pbr material");
    if (!gltf.materials.empty()) {
        const MaterialXMaterial& m = gltf.materials[0];
        check(hasMap(m, MaterialMap::BaseColor, "helmet_albedo.png") && about(m.params.baseColor, 0.5f, 0.6f, 0.7f),
              "the glTF color image, and its color factor as the tint");
        check(hasMap(m, MaterialMap::AmbientOcclusion, "helmet_orm.png", 0) && hasMap(m, MaterialMap::Roughness, "helmet_orm.png", 1) &&
                  hasMap(m, MaterialMap::Metalness, "helmet_orm.png", 2),
              "occlusion, roughness and metalness from the red, green and blue of one image");
        check(hasMap(m, MaterialMap::Normal, "helmet_normal.png") && about(m.params.emission, 3.0f) && about(m.params.topCoat, 0.7f),
              "the glTF normal map, emission at its strength, the clearcoat");
        check(map(m, MaterialMap::Opacity) == nullptr && about(m.params.opacity, 1.0f), "the alpha is not used while the alpha mode is opaque (the default)");
    }

    const MaterialXDocument usd = read(wrap(R"(
  <UsdUVTexture name="tex" type="multioutput"><input name="file" type="filename" value="brass.jpg" /></UsdUVTexture>
  <UsdPreviewSurface name="SR" type="surfaceshader">
    <input name="diffuseColor" type="color3" nodename="tex" output="rgb" />
    <input name="roughness" type="float" nodename="tex" output="g" />
    <input name="metallic" type="float" value="1" />
    <input name="clearcoat" type="float" value="0.5" />
  </UsdPreviewSurface>
)"), &ok);
    check(ok && usd.materials.size() == 1 && usd.materials[0].model == "UsdPreviewSurface" && hasMap(usd.materials[0], MaterialMap::BaseColor, "brass.jpg") &&
              hasMap(usd.materials[0], MaterialMap::Roughness, "brass.jpg", 1) && about(usd.materials[0].params.metalness, 1.0f) &&
              about(usd.materials[0].params.topCoat, 0.5f) && about(usd.materials[0].params.emission, 0.0f),
          "a UsdPreviewSurface material: its texture's rgb and g outputs");

    // The older form (1.36): a material element, a shader reference, bound inputs, parameters.
    const MaterialXDocument legacy = read(wrap(R"(
  <nodegraph name="NG">
    <image name="c" type="color3"><parameter name="file" type="filename" value="old_color.png" /></image>
    <output name="out" type="color3" nodename="c" />
  </nodegraph>
  <material name="Old">
    <shaderref name="SR" node="standard_surface">
      <bindinput name="base" type="float" value="1" />
      <bindinput name="base_color" type="color3" nodegraph="NG" output="out" />
      <bindinput name="specular_roughness" type="float" value="0.35" />
    </shaderref>
  </material>
)", "version=\"1.36\""), &ok);
    check(ok && legacy.materials.size() == 1 && legacy.materials[0].name == "Old" && hasMap(legacy.materials[0], MaterialMap::BaseColor, "old_color.png") &&
              about(legacy.materials[0].params.roughness, 0.35f),
          "the older material / shaderref / bindinput / parameter form");

    // Several materials in one file, in the file's order.
    const MaterialXDocument two = read(wrap(R"(
  <standard_surface name="A" type="surfaceshader"><input name="metalness" type="float" value="1" /></standard_surface>
  <standard_surface name="B" type="surfaceshader"><input name="metalness" type="float" value="0" /></standard_surface>
  <surfacematerial name="First" type="material"><input name="surfaceshader" type="surfaceshader" nodename="A" /></surfacematerial>
  <surfacematerial name="Second" type="material"><input name="surfaceshader" type="surfaceshader" nodename="B" /></surfacematerial>
)"), &ok);
    check(ok && two.materials.size() == 2 && two.materials[0].name == "First" && about(two.materials[0].params.metalness, 1.0f) &&
              two.materials[1].name == "Second" && about(two.materials[1].params.metalness, 0.0f),
          "two materials in one file, in the file's order");
}

void checkDocuments() {
    std::printf("-- includes, and what is not a material file\n");
    bool        ok = false;
    std::string error;
    // An include: the shading node lives in another document.
    const pose::MaterialXInclude include = [](const std::string& href, std::string& text) {
        if (href != "shaders/base.mtlx") {
            return false;
        }
        text = wrap(R"(
  <standard_surface name="Shared" type="surfaceshader"><input name="specular_roughness" type="float" value="0.6" /></standard_surface>
)");
        return true;
    };
    const MaterialXDocument included = read(wrap(R"(
  <xi:include href="shaders/base.mtlx" />
  <xi:include href="missing.mtlx" />
  <surfacematerial name="Uses" type="material"><input name="surfaceshader" type="surfaceshader" nodename="Shared" /></surfacematerial>
)"), &ok, &error, include);
    check(ok && included.materials.size() == 1 && included.materials[0].name == "Uses" && about(included.materials[0].params.roughness, 0.6f),
          "a shading node in an included document is found (an include that cannot be had is passed over)");

    read("<materialx version=\"1.38\"><standard_surface name=\"A\" type=\"surfaceshader\">", &ok, &error);
    check(!ok && error.find("not valid XML") != std::string::npos, "markup that is not closed is refused, with the reason");
    read("<materialx version=\"1.38\"><input name=broken /></materialx>", &ok, &error);
    check(!ok && error.find("not quoted") != std::string::npos, "an attribute without its quotes is refused");
    read("<?xml version=\"1.0\"?><svg><rect/></svg>", &ok, &error);
    check(!ok && error.find("<materialx>") != std::string::npos, "another kind of XML is refused");
    read("this is not xml at all", &ok, &error);
    check(!ok && !error.empty(), "a file that is no XML is refused");
    read(wrap(R"(
  <nodegraph name="NG"><output name="out" type="color3" value="1,1,1" /></nodegraph>
)"), &ok, &error);
    check(!ok && error.find("no material") != std::string::npos, "a document with no surface shader holds no material");
    read(wrap(R"(
  <my_studio_shader name="SR" type="surfaceshader" />
  <surfacematerial name="M" type="material"><input name="surfaceshader" type="surfaceshader" nodename="SR" /></surfacematerial>
)"), &ok, &error);
    check(!ok && error.find("my_studio_shader") != std::string::npos, "a shading model the reader does not know is named in the refusal");

    // Entities and single quotes in attribute values.
    const MaterialXDocument quoted = read(wrap(R"(
  <image name='c' type='color3'><input name='file' type='filename' value='Tom &amp; Jerry&apos;s &#233;t&#xE9;.png' /></image>
  <standard_surface name='SR' type='surfaceshader'><input name='base_color' type='color3' nodename='c' /></standard_surface>
)"), &ok);
    check(ok && quoted.materials.size() == 1 && hasMap(quoted.materials[0], MaterialMap::BaseColor, "Tom & Jerry's \xC3\xA9t\xC3\xA9.png"),
          "single quotes and entities in a file name are read (as UTF-8)");
}

int report(const std::string& path) {
    const auto slurp = [](const std::string& file, std::string& text) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        text = buffer.str();
        return true;
    };
    std::string text;
    if (!slurp(path, text)) {
        std::printf("cannot read %s\n", path.c_str());
        return 1;
    }
    const std::size_t slash = path.find_last_of("/\\");
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    MaterialXDocument doc;
    std::string       error;
    if (!pose::readMaterialXText(text, doc, error, [&](const std::string& href, std::string& included) { return slurp(folder + href, included); })) {
        std::printf("%s\n  NOT READ: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    std::printf("%s (MaterialX %s): %d material(s)\n", path.c_str(), doc.version.c_str(), static_cast<int>(doc.materials.size()));
    for (const MaterialXMaterial& m : doc.materials) {
        const pose::MaterialParams& p = m.params;
        std::printf("  %s [%s]\n", m.name.c_str(), m.model.c_str());
        std::printf("    base %.3f %.3f %.3f  metal %.2f  rough %.2f  spec %.2f  ior %.2f  coat %.2f  sheen %.2f  transmission %.2f  subsurface %.2f  "
                    "emission %.2f  film %.2f  opacity %.2f\n",
                    p.baseColor.x, p.baseColor.y, p.baseColor.z, p.metalness, p.roughness, p.specular, p.ior, p.topCoat, p.sheen, p.transmission,
                    p.translucency, p.emission, p.thinFilm, p.opacity);
        if (p.uvTiling != glm::vec2(1.0f) || p.uvOffset != glm::vec2(0.0f)) {
            std::printf("    tiling %.3f %.3f  offset %.3f %.3f\n", p.uvTiling.x, p.uvTiling.y, p.uvOffset.x, p.uvOffset.y);
        }
        for (const pose::MaterialXTexture& t : m.textures) {
            std::printf("    map %-18s %s%s\n", pose::materialMapName(t.slot), t.file.c_str(),
                        t.channel == 0 ? "" : t.channel == 1 ? "  (green)" : t.channel == 2 ? "  (blue)" : "  (alpha)");
        }
        for (const std::string& note : m.notes) {
            std::printf("    note: %s\n", note.c_str());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--report") {
        int failed = 0;
        for (int i = 2; i < argc; ++i) {
            failed += report(argv[i]);
        }
        return failed;
    }
    checkValues();
    checkTextures();
    checkModels();
    checkDocuments();
    std::printf("%d failed\n", failures);
    return failures;
}
