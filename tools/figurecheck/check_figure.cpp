/**
 * @file check_figure.cpp
 * @brief The native figure importer's guard and its report tool (target PoseStudioFigureCheck).
 *
 * Two uses, both through the REAL importer (FigureImporter::load — Qt-free, no Vulkan device):
 *
 *   PoseStudioFigureCheck
 *       The CTest guard (figure-shape-check): writes a tiny synthetic content library into the OS
 *       temp folder — a base figure, shape morphs, controls, a follower, scenes of several
 *       figures — and checks what each character preset comes out as. The real thing is vendor
 *       content and lives outside the repository; the rules that turn "a preset dials a control"
 *       into "the cage has this shape" and "this zone has that material" are tested here on files
 *       written for the purpose. Exit code = the number of failed checks.
 *
 *   PoseStudioFigureCheck --report <preset> [content root …]
 *       Imports a real preset and prints what came of it: the shape morphs that were applied and
 *       the ones that could not be, the skeleton, and per material zone its vertices and the maps
 *       its material names. Two reports of one preset from two builds are compared with `diff`:
 *       that is how a change to the importer is checked against a whole content library.
 *       With no content root given, the folder above the preset that contains "data" is used.
 *       POSESTUDIO_MORPH_TRACE=1 adds the walk itself (which dial drives what) on stderr.
 *
 *   PoseStudioFigureCheck --dump <preset> [content root …]
 *       The same import printed WHOLE — every material field, every bone's frame and limits, every
 *       corrective's formulas, a hash of each zone's vertices. For proving a change neutral: two
 *       dumps that compare equal are two imports a caller cannot tell apart.
 */

#include "figureimporter.h"

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// ================================================================================================
// --report
// ================================================================================================

std::string baseName(const std::string& path) {
    return path.empty() ? std::string("-") : fs::path(path).filename().string();
}

// The content root a preset's "/data/…" references resolve against, as the application detects
// it for an imported file (none: an empty list).
std::vector<std::string> detectRoots(const std::string& file) {
    const std::string root = pose::FigureImporter::contentRootOf(file);
    return root.empty() ? std::vector<std::string>() : std::vector<std::string>{root};
}

int report(const std::string& preset, std::vector<std::string> roots) {
    if (roots.empty()) {
        roots = detectRoots(preset);
    }
    pose::FigureData fig;
    try {
        fig = pose::FigureImporter().load(preset, roots);
    } catch (const std::exception& e) {
        std::printf("FAILED %s\n", e.what());
        return 1;
    }

    std::printf("scale %.4f\n", static_cast<double>(fig.figureScale));
    std::printf("correctives %zu\n", fig.correctives.size());

    std::printf("shape %zu morphs, %d joints moved\n", fig.shape.applied.size(), fig.shape.jointsMoved);
    for (const pose::ShapeReport::Morph& m : fig.shape.applied) {
        std::printf("  morph %s weight %.4f deltas %zu\n", m.id.c_str(), static_cast<double>(m.weight),
                    m.deltas);
    }
    for (const std::string& missing : fig.shape.missing) {
        std::printf("  MISSING %s\n", missing.c_str());
    }
    for (const std::string& skipped : fig.skipped) {
        std::printf("  LEFT OUT %s\n", skipped.c_str());
    }

    glm::dvec3 originSum(0.0);
    for (const pose::FigureBone& b : fig.bones) {
        originSum += glm::dvec3(b.origin);
    }
    std::printf("bones %zu origins %.2f %.2f %.2f\n", fig.bones.size(), originSum.x, originSum.y,
                originSum.z);
    for (const pose::FigureBone& b : fig.bones) {
        std::printf("  bone %s %.3f %.3f %.3f\n", b.name.c_str(), static_cast<double>(b.origin.x),
                    static_cast<double>(b.origin.y), static_cast<double>(b.origin.z));
    }

    std::printf("meshes %zu\n", fig.meshes.size());
    for (const pose::FigureMesh& m : fig.meshes) {
        glm::dvec3 sum(0.0);
        glm::vec3  lo(1e30f), hi(-1e30f);
        for (const pose::Vertex& v : m.vertices) {
            sum += glm::dvec3(v.pos);
            lo = glm::min(lo, v.pos);
            hi = glm::max(hi, v.pos);
        }
        if (m.vertices.empty()) {
            lo = hi = glm::vec3(0.0f);
        }
        const pose::FigureMaterial& mat = m.material;
        std::printf("  mesh %s verts %zu sum %.1f %.1f %.1f box %.2f %.2f %.2f .. %.2f %.2f %.2f\n",
                    m.materialZone.c_str(), m.vertices.size(), sum.x, sum.y, sum.z,
                    static_cast<double>(lo.x), static_cast<double>(lo.y), static_cast<double>(lo.z),
                    static_cast<double>(hi.x), static_cast<double>(hi.y), static_cast<double>(hi.z));
        std::printf("    material base %.3f %.3f %.3f opacity %.3f rough %.3f spec %.3f | diffuse %s | "
                    "normal %s | bump %s | cutout %s\n",
                    static_cast<double>(mat.baseColor.r), static_cast<double>(mat.baseColor.g),
                    static_cast<double>(mat.baseColor.b), static_cast<double>(mat.opacity),
                    static_cast<double>(mat.roughness), static_cast<double>(mat.specularWeight),
                    baseName(mat.diffuseMapPath).c_str(), baseName(mat.normalMapPath).c_str(),
                    baseName(mat.bumpMapPath).c_str(), baseName(mat.opacityMapPath).c_str());
    }
    return 0;
}

// ================================================================================================
// --dump
// ================================================================================================
//
// Everything the importer hands on, to the last bit: every field of every material, every bone
// with its frame and limits, every corrective with its formulas, and a hash of each zone's
// vertices and indices. Two dumps of one preset that compare equal are two imports that differ in
// nothing a caller can see — the test for a change that must be NEUTRAL (--report prints what a
// person reads; a moved roughness lobe or a re-ordered vertex does not show there).

// FNV-1a over a byte range, chained through @p hash.
void hashBytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

int dump(const std::string& preset, std::vector<std::string> roots) {
    if (roots.empty()) {
        roots = detectRoots(preset);
    }
    pose::FigureData fig;
    try {
        fig = pose::FigureImporter().load(preset, roots);
    } catch (const std::exception& e) {
        std::printf("FAILED %s\n", e.what());
        return 1;
    }
    const auto num = [](const char* name, double value) { std::printf(" %s %.9g", name, value); };
    const auto vec = [](const char* name, const glm::vec3& v) {
        std::printf(" %s %.9g %.9g %.9g", name, static_cast<double>(v.x), static_cast<double>(v.y),
                    static_cast<double>(v.z));
    };
    const auto vec2 = [](const char* name, const glm::vec2& v) {
        std::printf(" %s %.9g %.9g", name, static_cast<double>(v.x), static_cast<double>(v.y));
    };
    const auto text = [](const char* name, const std::string& value) {
        if (!value.empty()) {
            std::printf("\n      %s %s", name, value.c_str()); // (a map: only where there is one)
        }
    };

    std::printf("scale %.9g baseVertexCount %zu graft %d node '%s' '%s' zonesOnOwnUvSet %d\n",
                static_cast<double>(fig.figureScale), fig.baseVertexCount, fig.graftPopulated ? 1 : 0,
                fig.figureNodeId.c_str(), fig.figureNodeName.c_str(), fig.zonesOnOwnUvSet);

    std::printf("shape %zu morphs, %d joints moved\n", fig.shape.applied.size(), fig.shape.jointsMoved);
    for (const pose::ShapeReport::Morph& m : fig.shape.applied) {
        std::printf("  morph %s weight %.9g deltas %zu\n", m.id.c_str(), static_cast<double>(m.weight), m.deltas);
    }
    for (const std::string& missing : fig.shape.missing) {
        std::printf("  MISSING %s\n", missing.c_str());
    }
    for (const std::string& skipped : fig.skipped) {
        std::printf("  LEFT OUT %s\n", skipped.c_str());
    }

    std::printf("bones %zu\n", fig.bones.size());
    for (const pose::FigureBone& b : fig.bones) {
        std::printf("  bone %s '%s' parent %d order %s", b.name.c_str(), b.nodeName.c_str(), b.parent,
                    b.rotationOrder.c_str());
        vec("origin", b.origin);
        vec("orientation", b.orientation);
        vec("min", b.rotationMin);
        vec("max", b.rotationMax);
        std::printf(" limited %d%d%d\n", b.rotationLimited.x ? 1 : 0, b.rotationLimited.y ? 1 : 0,
                    b.rotationLimited.z ? 1 : 0);
    }

    std::printf("correctives %zu\n", fig.correctives.size());
    for (const pose::PoseCorrective& c : fig.correctives) {
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto& [index, delta] : c.deltas) {
            hashBytes(hash, &index, sizeof(index));
            hashBytes(hash, &delta, sizeof(delta));
        }
        std::printf("  corrective %s gate %.9g clamped %d %.9g %.9g deltas %zu %016llx\n", c.id.c_str(),
                    static_cast<double>(c.gateScale), c.clamped ? 1 : 0, static_cast<double>(c.clampMin),
                    static_cast<double>(c.clampMax), c.deltas.size(), static_cast<unsigned long long>(hash));
        for (const pose::CorrectiveFormula& f : c.sumFormulas) {
            std::printf("    formula");
            for (const pose::CorrectiveOp& op : f.ops) {
                switch (op.kind) {
                    case pose::CorrectiveOp::Kind::PushRotation:
                        std::printf(" rot(%s,%d)", op.bone.c_str(), op.axis);
                        break;
                    case pose::CorrectiveOp::Kind::PushConst:
                        std::printf(" const(%.9g)", static_cast<double>(op.value));
                        break;
                    case pose::CorrectiveOp::Kind::Spline:
                        std::printf(op.linear ? " linear(" : " spline(");
                        for (const pose::CorrectiveKnot& k : op.knots) {
                            std::printf("%.9g:%.9g ", static_cast<double>(k.x), static_cast<double>(k.y));
                        }
                        std::printf(")");
                        break;
                    case pose::CorrectiveOp::Kind::Mult:
                        std::printf(" mult");
                        break;
                    case pose::CorrectiveOp::Kind::Add:
                        std::printf(" add");
                        break;
                }
            }
            std::printf("\n");
        }
    }

    std::printf("meshes %zu\n", fig.meshes.size());
    for (const pose::FigureMesh& m : fig.meshes) {
        std::uint64_t vertexHash = 14695981039346656037ull;
        std::uint64_t indexHash = 14695981039346656037ull;
        std::uint64_t baseHash = 14695981039346656037ull;
        if (!m.vertices.empty()) {
            hashBytes(vertexHash, m.vertices.data(), m.vertices.size() * sizeof(pose::Vertex));
        }
        if (!m.indices.empty()) {
            hashBytes(indexHash, m.indices.data(), m.indices.size() * sizeof(std::uint32_t));
        }
        if (!m.baseVertex.empty()) {
            hashBytes(baseHash, m.baseVertex.data(), m.baseVertex.size() * sizeof(std::uint32_t));
        }
        std::printf("  mesh %s verts %zu %016llx indices %zu %016llx base %zu %016llx\n", m.materialZone.c_str(),
                    m.vertices.size(), static_cast<unsigned long long>(vertexHash), m.indices.size(),
                    static_cast<unsigned long long>(indexHash), m.baseVertex.size(),
                    static_cast<unsigned long long>(baseHash));
        const pose::FigureMaterial& mat = m.material;
        std::printf("    material");
        vec("base", mat.baseColor);
        num("opacity", mat.opacity);
        num("rough", mat.roughness);
        num("spec", mat.specularWeight);
        num("specWithMap", mat.specularWeightWithMap);
        num("metallic", mat.metallic);
        num("lobe1", mat.lobe1Roughness);
        num("lobe2", mat.lobe2Roughness);
        num("lobeRatio", mat.lobeRatio);
        num("normalStrength", mat.normalStrength);
        num("bumpStrength", mat.bumpStrength);
        std::printf("\n     ");
        num("coat", mat.topCoatWeight);
        num("coatOn", mat.topCoatEnabled ? 1.0 : 0.0);
        num("coatRough", mat.topCoatRoughness);
        vec("coatColor", mat.coatColor);
        num("coatIor", mat.coatIor);
        num("coatAniso", mat.coatAnisotropy);
        num("coatAnisoRot", mat.coatAnisotropyRotation);
        num("coatBump", mat.coatBumpStrength);
        std::printf("\n     ");
        num("translucency", mat.translucencyWeight);
        num("detail", mat.detailWeight);
        num("detailTiles", mat.detailTiles);
        num("detailTilesV", mat.detailTilesV);
        vec2("detailOffset", mat.detailOffset);
        num("detailRough", mat.detailRoughness);
        num("disp", mat.displacementStrength);
        num("dispMid", mat.displacementMidLevel);
        num("dispSubdiv", mat.displacementSubdiv);
        std::printf("\n     ");
        vec("emissionColor", mat.emissionColor);
        num("emission", mat.emission);
        num("ior", mat.ior);
        num("transmission", mat.transmission);
        vec("transmissionColor", mat.transmissionColor);
        num("transmissionDepth", mat.transmissionDepth);
        num("transmissionRough", mat.transmissionRoughness);
        num("thinWalled", mat.thinWalled ? 1.0 : 0.0);
        std::printf("\n     ");
        vec("sssColor", mat.subsurfaceColor);
        vec("sssRadiusColor", mat.subsurfaceRadiusColor);
        num("sssRadius", mat.subsurfaceRadius);
        num("sssAniso", mat.subsurfaceAnisotropy);
        num("sheen", mat.sheen);
        vec("sheenColor", mat.sheenColor);
        num("sheenRough", mat.sheenRoughness);
        std::printf("\n     ");
        num("thinFilm", mat.thinFilm);
        num("thinFilmThickness", mat.thinFilmThickness);
        num("thinFilmIor", mat.thinFilmIor);
        num("aniso", mat.anisotropy);
        num("anisoRot", mat.anisotropyRotation);
        num("diffuseRough", mat.diffuseRoughness);
        vec2("uvTiling", mat.uvTiling);
        vec2("uvOffset", mat.uvOffset);
        text("diffuse", mat.diffuseMapPath);
        text("normal", mat.normalMapPath);
        text("bump", mat.bumpMapPath);
        text("cutout", mat.opacityMapPath);
        text("roughness", mat.roughnessMapPath);
        text("specMask", mat.specMaskMapPath);
        text("translucency", mat.translucencyMapPath);
        text("translucencyWeight", mat.translucencyWeightMapPath);
        text("detailNormal", mat.detailNormalMapPath);
        text("detailWeight", mat.detailWeightMapPath);
        text("detailRoughness", mat.detailRoughnessMapPath);
        text("displacement", mat.displacementMapPath);
        text("emission", mat.emissionMapPath);
        text("transmissionColor", mat.transmissionColorMapPath);
        text("transmissionWeight", mat.transmissionWeightMapPath);
        text("coatAnisotropy", mat.coatAnisotropyMapPath);
        text("coatAnisotropyRotation", mat.coatAnisotropyRotationMapPath);
        text("sheenWeight", mat.sheenWeightMapPath);
        text("sheenColor", mat.sheenColorMapPath);
        text("sheenRoughness", mat.sheenRoughnessMapPath);
        text("thinFilmThickness", mat.thinFilmThicknessMapPath);
        text("diffuseRoughness", mat.diffuseRoughnessMapPath);
        text("coatWeight", mat.coatWeightMapPath);
        text("coatColor", mat.coatColorMapPath);
        text("coatRoughness", mat.coatRoughnessMapPath);
        text("coatReflectivity", mat.coatReflectivityMapPath);
        text("coatBump", mat.coatBumpMapPath);
        text("specularColor", mat.specularColorMapPath);
        text("anisotropy", mat.anisotropyMapPath);
        text("anisotropyRotation", mat.anisotropyRotationMapPath);
        text("lobe2Roughness", mat.lobe2RoughnessMapPath);
        text("lobeRatio", mat.lobeRatioMapPath);
        std::printf("\n");
    }
    return 0;
}

// ================================================================================================
// The synthetic library
// ================================================================================================
//
// One base figure — a box of eight vertices, two material zones, three bones — and the morph
// files a character is made of. Every id that CAN have a space in it has one: that is the first
// thing real content does that a reader written against tidy ids gets wrong.

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
}

void writeJson(const fs::path& path, const json& document) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << document.dump(1);
}

// "Some Character Head" -> "Some%20Character%20Head", as a reference spells it.
std::string encoded(const std::string& id) {
    std::string out;
    for (const char c : id) {
        if (c == ' ') {
            out += "%20";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

const char* kFigureFile = "/data/Maker/Figure/Figure.dsf";
const char* kMorphs = "/data/Maker/Figure/Morphs/Maker";

json xyz(double x, double y, double z) {
    return json::array({{{"id", "x"}, {"value", x}}, {{"id", "y"}, {"value", y}}, {{"id", "z"}, {"value", z}}});
}

json bone(const std::string& id, const std::string& type, const std::string& parent, double y) {
    json node = {{"id", id}, {"name", id}, {"type", type}, {"rotation_order", "YZX"},
                 {"center_point", xyz(0, y, 0)}, {"orientation", xyz(0, 0, 0)}};
    if (!parent.empty()) {
        node["parent"] = "#" + encoded(parent);
    }
    return node;
}

// The base figure: a box, x and z within ±10, y from 0 to 100. Five faces of "Skin", the top face
// "Eye Moisture" (a zone whose name has a space in it).
json baseFigure() {
    json vertices = json::array();
    for (const double y : {0.0, 100.0}) {
        for (const auto& [x, z] : std::initializer_list<std::pair<double, double>>{
                 {-10, -10}, {10, -10}, {10, 10}, {-10, 10}}) {
            vertices.push_back({x, y, z});
        }
    }
    const json polylist = json::array({{0, 0, 0, 1, 2, 3}, {0, 0, 0, 4, 5, 1}, {0, 0, 1, 5, 6, 2},
                                       {0, 0, 2, 6, 7, 3}, {0, 0, 3, 7, 4, 0}, {0, 1, 4, 7, 6, 5}});
    json weights = json::array();
    for (int v = 0; v < 8; ++v) {
        weights.push_back({v, 1.0});
    }
    return {
        {"file_version", "0.6.0.0"},
        {"asset_info", {{"id", kFigureFile}, {"type", "figure"}}},
        {"geometry_library",
         json::array({{{"id", "geometry"},
                       {"type", "polygon_mesh"},
                       {"vertices", {{"count", 8}, {"values", vertices}}},
                       {"polygon_groups", {{"count", 1}, {"values", {"All"}}}},
                       {"polygon_material_groups", {{"count", 2}, {"values", {"Skin", "Eye Moisture"}}}},
                       {"polylist", {{"count", 6}, {"values", polylist}}}}})},
        {"node_library", json::array({bone("Figure", "figure", "", 0), bone("hip", "bone", "Figure", 50),
                                      bone("chest", "bone", "hip", 80)})},
        {"modifier_library",
         json::array({{{"id", "SkinBinding"},
                       {"skin",
                        {{"node", "#Figure"},
                         {"geometry", "#geometry"},
                         {"vertex_count", 8},
                         {"joints", json::array({{{"id", "hip"},
                                                  {"node", "#hip"},
                                                  {"node_weights", {{"count", 8}, {"values", weights}}}}})}}}}})},
        {"scene",
         {{"nodes", json::array({{{"id", "Figure"},
                                  {"url", "#Figure"},
                                  {"geometries", json::array({{{"id", "geometry-1"}, {"url", "#geometry"}}})}}})}}}};
}

json push(const std::string& url) {
    return {{"op", "push"}, {"url", url}};
}
json push(double value) {
    return {{"op", "push"}, {"val", value}};
}
const json kMult = {{"op", "mult"}};

// "<output> += <driver's value> x <factor>", in the stage given ("" = the default, sum).
json drives(const std::string& driverId, const std::string& output, double factor,
            const std::string& stage = std::string()) {
    json formula = {{"output", output},
                    {"operations", json::array({push("Figure:#" + encoded(driverId) + "?value"),
                                                push(factor), kMult})}};
    if (!stage.empty()) {
        formula["stage"] = stage;
    }
    return formula;
}

// A reference to another morph file's value channel: "Figure:/data/…/<folder>/<file>.dsf#<id>?value".
std::string valueOf(const std::string& folder, const std::string& file, const std::string& id) {
    return std::string("Figure:") + kMorphs + "/" + encoded(folder) + "/" + encoded(file) + ".dsf#" +
           encoded(id) + "?value";
}

// One modifier file: a channel limited to [lo, hi], a morph moving @p vertex (none when negative),
// and its formulas. @p vertexCount is what the morph says its geometry has.
void writeModifier(const fs::path& library, const std::string& folder, const std::string& file,
                   const std::string& id, int vertex, json formulas = json::array(), double lo = 0.0,
                   double hi = 1.0, int vertexCount = 8) {
    json modifier = {{"id", id},
                     {"name", id},
                     {"parent", std::string(kFigureFile) + "#Figure"},
                     {"channel",
                      {{"id", "value"}, {"type", "float"}, {"value", 0}, {"min", lo}, {"max", hi}, {"clamped", true}}}};
    if (vertex >= 0) {
        modifier["morph"] = {{"vertex_count", vertexCount},
                             {"deltas", {{"count", 1}, {"values", json::array({{vertex, 1.0, 2.0, 3.0}})}}}};
    }
    if (!formulas.empty()) {
        modifier["formulas"] = std::move(formulas);
    }
    writeJson(library / "data/Maker/Figure/Morphs/Maker" / folder / (file + ".dsf"),
              {{"file_version", "0.6.0.0"},
               {"asset_info", {{"id", std::string(kMorphs) + "/" + encoded(folder) + "/" + encoded(file) + ".dsf"},
                               {"type", "modifier"}}},
               {"modifier_library", json::array({modifier})}});
}

// The folder of the synthetic library's pose-corrective pack (under the figure's Morphs/Maker).
const char* kPack = "Base Correctives";

// A corrective's driver: "<id> += the hip's bend x 0.01" (the "sum" stage).
json hipDriven(const std::string& id) {
    return {{"output", "Figure:#" + encoded(id) + "?value"},
            {"operations", json::array({push(std::string("hip:") + kFigureFile + "#hip?rotation/x"), push(0.01), kMult})}};
}

// A corrective's gate: "<id> x= @p operations" (the "mult" stage).
json gatedBy(const std::string& id, json operations) {
    return {{"output", "Figure:#" + encoded(id) + "?value"}, {"stage", "mult"}, {"operations", std::move(operations)}};
}

// A scene modifier: the dial a preset sets on the figure node @p parent.
json dial(const std::string& folder, const std::string& file, const std::string& id, double value,
          const std::string& parent = "Figure") {
    return {{"id", id},
            {"url", std::string(kMorphs) + "/" + encoded(folder) + "/" + encoded(file) + ".dsf#" + encoded(id)},
            {"parent", "#" + encoded(parent)},
            {"channel", {{"id", "value"}, {"type", "float"}, {"current_value", value}}}};
}

json figureNode() {
    return {{"id", "Figure"},
            {"url", std::string(kFigureFile) + "#Figure"},
            {"geometries", json::array({{{"id", "geometry"}, {"url", std::string(kFigureFile) + "#geometry"}}})}};
}

json preset(json nodes, json modifiers) {
    return {{"file_version", "0.6.0.0"},
            {"asset_info", {{"type", "character"}}},
            {"scene", {{"nodes", std::move(nodes)}, {"modifiers", std::move(modifiers)}}}};
}

void writeLibrary(const fs::path& library) {
    writeJson(library / "data/Maker/Figure/Figure.dsf", baseFigure());

    const std::string character = "Some Character";
    // The character's one dial: it drives her head, her body and her feet — and, as a GATE (the
    // "mult" stage), a pose corrective of hers, which must not be added to her shape.
    writeModifier(library, character, "Some Character", "Some Character-0x1a", -1,
                  json::array({drives("Some Character-0x1a", valueOf(character, "Some Character Head", "Some Character Head"), 1.0),
                               drives("Some Character-0x1a", valueOf(character, "Some Character Body", "Some Character Body"), 1.0),
                               drives("Some Character-0x1a", valueOf(character, "Feet", "XTFeet"), 1.0),
                               drives("Some Character-0x1a", valueOf(character, "Corrective", "Corrective"), 1.0, "mult")}));
    // The head: moves the chest joint up 3 with it, takes the base navel OFF (drives it by -1), and
    // drives the toes by id ALONE — no file in the reference.
    writeModifier(library, character, "Some Character Head", "Some Character Head", 4,
                  json::array({drives("Some Character Head", std::string("chest:") + kFigureFile + "#chest?center_point/y", 3.0),
                               drives("Some Character Head", valueOf("Base", "Navel", "Navel"), -1.0),
                               drives("Some Character Head", "Figure:#Toes?value", 0.5)}));
    // The body: also sets the figure's height (scale/general of the figure node, -10%).
    writeModifier(library, character, "Some Character Body", "Some Character Body", 0,
                  json::array({drives("Some Character Body", std::string("Figure:") + kFigureFile + "#Figure?scale/general", -0.1)}));
    writeModifier(library, character, "Feet", "XTFeet", 1);
    // A pose corrective: driven by a joint's rotation, gated by the character.
    writeModifier(library, character, "Corrective", "Corrective", 2,
                  json::array({{{"output", "Figure:#Corrective?value"},
                                {"operations", json::array({push(std::string("hip:") + kFigureFile + "#hip?rotation/x"),
                                                            push(0.01), kMult})}}}));
    writeModifier(library, "Base", "Navel", "Navel", 3);
    writeModifier(library, "Base", "Toes", "Toes", 5);
    // A control that shapes one morph below zero and another above it: two splines.
    writeModifier(library, "Base", "Size", "Size", -1,
                  json::array({{{"output", valueOf("Base", "Small", "Small")},
                                {"operations", json::array({push("Figure:#Size?value"),
                                                            {{"op", "push"}, {"val", {-1, 1, 0, 0, 0}}},
                                                            {{"op", "push"}, {"val", {0, 0, 0, 0, 0}}},
                                                            push(2.0), {{"op", "spline_tcb"}}})}},
                               {{"output", valueOf("Base", "Large", "Large")},
                                {"operations", json::array({push("Figure:#Size?value"),
                                                            {{"op", "push"}, {"val", {0, 0}}},
                                                            {{"op", "push"}, {"val", {1, 1}}},
                                                            push(2.0), {{"op", "spline_linear"}}})}}}),
                  -1.0, 1.0);
    writeModifier(library, "Base", "Small", "Small", 6);
    writeModifier(library, "Base", "Large", "Large", 7);
    // A morph made for a geometry of 99 vertices.
    writeModifier(library, "Base", "Other Geometry", "Other Geometry", 0, json::array(), 0.0, 1.0, 99);

    // A follower figure: one quad, the zone name the figure's own top face has, a root node whose
    // id has a space in it, and a morph of its own.
    writeJson(library / "data/Maker/Lashes/Lashes.dsf",
              {{"file_version", "0.6.0.0"},
               {"asset_info", {{"id", "/data/Maker/Lashes/Lashes.dsf"}, {"type", "figure"}}},
               {"geometry_library",
                json::array({{{"id", "Lashes Shape"},
                              {"vertices", {{"count", 4}, {"values", {{-1, 90, 10}, {1, 90, 10}, {1, 91, 10}, {-1, 91, 10}}}}},
                              {"polygon_groups", {{"count", 1}, {"values", {"All"}}}},
                              {"polygon_material_groups", {{"count", 1}, {"values", {"Eye Moisture"}}}},
                              {"polylist", {{"count", 1}, {"values", {{0, 0, 0, 1, 2, 3}}}}}}})},
               {"node_library", json::array({bone("Lashes Figure", "figure", "", 0),
                                             bone("lash", "bone", "Lashes Figure", 90)})},
               {"scene",
                {{"nodes", json::array({{{"id", "Lashes Figure"},
                                         {"url", "#Lashes%20Figure"},
                                         {"geometries", json::array({{{"id", "Lashes Shape-1"}, {"url", "#Lashes%20Shape"}}})}}})}}}});
    writeJson(library / "data/Maker/Lashes/Morphs/Maker/Base/Lash Length.dsf",
              {{"file_version", "0.6.0.0"},
               {"modifier_library",
                json::array({{{"id", "Lash Length"},
                              {"channel", {{"id", "value"}, {"value", 0}, {"min", 0}, {"max", 1}, {"clamped", true}}},
                              {"morph", {{"vertex_count", 4}, {"deltas", {{"count", 1}, {"values", {{0, 9.0, 9.0, 9.0}}}}}}}}})}});

    // A morph file that is not well typed: a delta whose x is a string, and — beside one formula
    // that drives the body — two entries that are no formulas at all.
    writeJson(library / "data/Maker/Figure/Morphs/Maker/Base/Broken.dsf",
              {{"file_version", "0.6.0.0"},
               {"modifier_library",
                json::array({{{"id", "Broken"},
                              {"channel", {{"id", "value"}, {"value", 0}, {"min", 0}, {"max", 1}, {"clamped", true}}},
                              {"morph", {{"vertex_count", 8},
                                         {"deltas", {{"count", 1}, {"values", json::array({json::array({4, "0.1", 0, 0})})}}}}},
                              {"formulas", json::array({nullptr,
                                                        {{"output", 7}},
                                                        drives("Broken", valueOf(character, "Some Character Body", "Some Character Body"), 1.0)})}}})}});

    // A head that drives, by id ALONE, a morph no file of the library defines.
    writeModifier(library, "Base", "Lone Head", "Lone Head", 4,
                  json::array({drives("Lone Head", "Figure:#Nowhere?value", 0.5)}));

    // A shape that is ON by default (its file's value is 1).
    writeJson(library / "data/Maker/Figure/Morphs/Maker/Base/Default On.dsf",
              {{"file_version", "0.6.0.0"},
               {"modifier_library",
                json::array({{{"id", "Default On"},
                              {"channel", {{"id", "value"}, {"value", 1}, {"min", 0}, {"max", 1}, {"clamped", true}}},
                              {"morph", {{"vertex_count", 8}, {"deltas", {{"count", 1}, {"values", {{6, 1.0, 2.0, 3.0}}}}}}}}})}});

    // A shader file: a base material (its id has a space in it) that scene materials extend.
    writeJson(library / "data/Maker/Shaders/Shader.dsf",
              {{"file_version", "0.6.0.0"},
               {"material_library",
                json::array({{{"id", "Skin Base"},
                              {"diffuse", {{"channel", {{"id", "diffuse"}, {"type", "float_color"}, {"value", {0.9, 0.1, 0.1}}}}}}}})}});

    // A pose-corrective PACK — a folder the scan always reads, by its name — and the switch that
    // gates it: a boolean channel, on by default, in a file of its own.
    writeJson(library / "data/Maker/Figure/Morphs/Maker/Base/Correctives On.dsf",
              {{"file_version", "0.6.0.0"},
               {"modifier_library",
                json::array({{{"id", "Correctives On"},
                              {"channel", {{"id", "value"}, {"type", "bool"}, {"value", true}}}}})}});
    writeModifier(library, kPack, "Gated Corrective", "Gated Corrective", 2,
                  json::array({hipDriven("Gated Corrective"),
                               gatedBy("Gated Corrective", json::array({push(valueOf("Base", "Correctives On", "Correctives On"))}))}));

    // A corrective of the pack that was made for a geometry of 99 vertices.
    writeModifier(library, kPack, "Foreign Corrective", "Foreign Corrective", 2,
                  json::array({hipDriven("Foreign Corrective")}), 0.0, 1.0, 99);

    // A dial (2 by default, 0 to 4) that a corrective's weight is DIVIDED by: a gate of
    // "push <dial>, inv".
    writeJson(library / "data/Maker/Figure/Morphs/Maker/Base/Fit.dsf",
              {{"file_version", "0.6.0.0"},
               {"modifier_library",
                json::array({{{"id", "Fit"},
                              {"channel", {{"id", "value"}, {"value", 2}, {"min", 0}, {"max", 4}, {"clamped", true}}}}})}});
    writeModifier(library, kPack, "Divided Corrective", "Divided Corrective", 2,
                  json::array({hipDriven("Divided Corrective"),
                               gatedBy("Divided Corrective",
                                       json::array({push(valueOf("Base", "Fit", "Fit")), {{"op", "inv"}}}))}));

    // A control whose formulas use the rest of the vocabulary — a reciprocal, a negation, a step
    // between two knots — and one operation nobody knows.
    const std::string ratio = "Figure:#Ratio?value";
    writeModifier(library, "Base", "Ratio", "Ratio", -1,
                  json::array({{{"output", valueOf("Base", "Reciprocal", "Reciprocal")},
                                {"operations", json::array({push(ratio), {{"op", "inv"}}})}},
                               {{"output", valueOf("Base", "Negated", "Negated")},
                                {"operations", json::array({push(ratio), {{"op", "neg"}}})}},
                               {{"output", valueOf("Base", "Stepped", "Stepped")},
                                {"operations", json::array({push(ratio),
                                                            {{"op", "push"}, {"val", {0, 0}}},
                                                            {{"op", "push"}, {"val", {8, 1}}},
                                                            push(2.0), {{"op", "spline_constant"}}})}},
                               {{"output", valueOf("Base", "Mystery", "Mystery")},
                                {"operations", json::array({push(ratio), {{"op", "frobnicate"}}})}}}),
                  0.0, 8.0);
    writeModifier(library, "Base", "Reciprocal", "Reciprocal", 1);
    writeModifier(library, "Base", "Negated", "Negated", 3, json::array(), -8.0, 8.0);
    writeModifier(library, "Base", "Stepped", "Stepped", 5);
    writeModifier(library, "Base", "Mystery", "Mystery", 7);

    // Correctives of the pack that cannot be carried — one whose driver ends in an operation
    // nobody knows, one that takes the reciprocal of a joint's angle — and two that can: a
    // straight ramp (spline_linear) and a smooth one (spline_tcb) over the hip's bend.
    const std::string hipBend = std::string("hip:") + kFigureFile + "#hip?rotation/x";
    json odd = hipDriven("Odd Corrective");
    odd["operations"].push_back({{"op", "frobnicate"}});
    writeModifier(library, kPack, "Odd Corrective", "Odd Corrective", 2, json::array({odd}));
    writeModifier(library, kPack, "Inverse Corrective", "Inverse Corrective", 2,
                  json::array({{{"output", "Figure:#Inverse%20Corrective?value"},
                                {"operations", json::array({push(hipBend), {{"op", "inv"}}})}}}));
    writeModifier(library, kPack, "Linear Corrective", "Linear Corrective", 2,
                  json::array({{{"output", "Figure:#Linear%20Corrective?value"},
                                {"operations", json::array({push(hipBend),
                                                            {{"op", "push"}, {"val", {0, 0}}},
                                                            {{"op", "push"}, {"val", {90, 1}}},
                                                            push(2.0), {{"op", "spline_linear"}}})}}}));
    writeModifier(library, kPack, "Smooth Corrective", "Smooth Corrective", 2,
                  json::array({{{"output", "Figure:#Smooth%20Corrective?value"},
                                {"operations", json::array({push(hipBend),
                                                            {{"op", "push"}, {"val", {0, 0, 0, 0, 0}}},
                                                            {{"op", "push"}, {"val", {90, 1, 0, 0, 0}}},
                                                            push(2.0), {{"op", "spline_tcb"}}})}}}));

    // A figure with a joint whose id has a space in it, and a corrective of its own pack that the
    // joint drives: the driver's url spells the id percent-encoded.
    json spaced = baseFigure();
    spaced["node_library"].push_back(bone("Left Arm", "bone", "chest", 85));
    writeJson(library / "data/Maker/Spaced/Spaced.dsf", spaced);
    writeJson(library / "data/Maker/Spaced/Morphs/Maker/Base Correctives/Arm Corrective.dsf",
              {{"file_version", "0.6.0.0"},
               {"modifier_library",
                json::array({{{"id", "Arm Corrective"},
                              {"channel", {{"id", "value"}, {"value", 0}, {"min", 0}, {"max", 1}, {"clamped", true}}},
                              {"morph", {{"vertex_count", 8}, {"deltas", {{"count", 1}, {"values", {{2, 1.0, 2.0, 3.0}}}}}}},
                              {"formulas",
                               json::array({{{"output", "Figure:#Arm%20Corrective?value"},
                                             {"operations",
                                              json::array({push("Left%20Arm:/data/Maker/Spaced/Spaced.dsf#Left%20Arm?rotation/x"),
                                                           push(0.01), kMult})}}})}}})}});

    // The base figure again, its skeleton listed CHILDREN FIRST.
    json unordered = baseFigure();
    unordered["node_library"] = json::array({bone("chest", "bone", "hip", 80), bone("hip", "bone", "Figure", 50),
                                             bone("Figure", "figure", "", 0)});
    writeJson(library / "data/Maker/Unordered/Unordered.dsf", unordered);

    // The base figure once more, its geometry listing NO material zones (every face is then one
    // unnamed zone), and mapped: a UV per vertex, u from 0.1 to 0.8.
    json untagged = baseFigure();
    untagged["geometry_library"][0].erase("polygon_material_groups");
    untagged["geometry_library"][0]["default_uv_set"] = "/data/Maker/Untagged/UV/Layout.dsf#Layout";
    writeJson(library / "data/Maker/Untagged/Untagged.dsf", untagged);
    json uvs = json::array();
    for (int v = 0; v < 8; ++v) {
        uvs.push_back({0.1 + 0.1 * v, 0.25});
    }
    writeJson(library / "data/Maker/Untagged/UV/Layout.dsf",
              {{"file_version", "0.6.0.0"},
               {"uv_set_library", json::array({{{"id", "Layout"},
                                                {"vertex_count", 8},
                                                {"uvs", {{"count", 8}, {"values", uvs}}}}})}});
}

// ------------------------------------------------------------------------------------------------

pose::FigureData load(const fs::path& library, const fs::path& presetPath, std::string* error = nullptr) {
    try {
        return pose::FigureImporter().load(presetPath.string(), {library.string()});
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return {};
    }
}

// Whether a morph was applied at all, and the weight it was applied at (0 when it was not).
bool wasApplied(const pose::FigureData& fig, const std::string& id) {
    return std::any_of(fig.shape.applied.begin(), fig.shape.applied.end(),
                       [&](const pose::ShapeReport::Morph& m) { return m.id == id; });
}
double applied(const pose::FigureData& fig, const std::string& id) {
    for (const pose::ShapeReport::Morph& m : fig.shape.applied) {
        if (m.id == id) {
            return static_cast<double>(m.weight);
        }
    }
    return 0.0;
}

bool about(double a, double b) {
    return std::abs(a - b) < 1e-4;
}

bool missingMentions(const pose::FigureData& fig, const std::string& text) {
    return std::any_of(fig.shape.missing.begin(), fig.shape.missing.end(),
                       [&](const std::string& m) { return m.find(text) != std::string::npos; });
}

const pose::FigureMesh* zone(const pose::FigureData& fig, const std::string& name) {
    for (const pose::FigureMesh& m : fig.meshes) {
        if (m.materialZone == name) {
            return &m;
        }
    }
    return nullptr;
}

const pose::PoseCorrective* corrective(const pose::FigureData& fig, const std::string& id) {
    for (const pose::PoseCorrective& c : fig.correctives) {
        if (c.id == id) {
            return &c;
        }
    }
    return nullptr;
}

// A corrective's folded gate (what its driven weight is multiplied by); -1 where the figure has no
// such corrective.
double gateOf(const pose::FigureData& fig, const std::string& id) {
    const pose::PoseCorrective* c = corrective(fig, id);
    return c ? static_cast<double>(c->gateScale) : -1.0;
}

const pose::FigureBone* boneNamed(const pose::FigureData& fig, const std::string& name) {
    for (const pose::FigureBone& b : fig.bones) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

int runChecks() {
    std::error_code ec;
    const fs::path library = fs::temp_directory_path() / "posestudio-figure-check";
    fs::remove_all(library, ec);
    writeLibrary(library);
    const fs::path presets = library / "People/Figure/Characters";
    const std::string character = "Some Character";

    // --- 1. A character is ONE dial that drives several shapes --------------------------------
    {
        writeJson(presets / "Some Character.duf",
                  preset(json::array({figureNode()}),
                         json::array({dial(character, "Some Character", "Some Character-0x1a", 1.0)})));
        const pose::FigureData fig = load(library, presets / "Some Character.duf");
        check(about(applied(fig, "Some Character Head"), 1.0),
              "a control whose id has a space in it is found: the head it drives is applied");
        check(about(applied(fig, "Some Character Body"), 1.0), "… and the body");
        check(about(applied(fig, "XTFeet"), 1.0), "… and the feet");
        check(about(applied(fig, "Toes"), 0.5),
              "a morph driven by id alone (no file in the reference) is found by its file's name");
        check(!wasApplied(fig, "Corrective"),
              "a pose corrective the control only GATES (the mult stage) is not added to the shape");
        check(!wasApplied(fig, "Navel"),
              "a morph driven below its range is held at its limit: not applied the other way round");
        const pose::FigureBone* chest = boneNamed(fig, "chest");
        check(chest && about(chest->origin.y, 83.0),
              "the joint a shape moves follows it (a morph whose id has a space in it included)");
        check(about(fig.figureScale, 0.9), "the height the body sets is the figure's scale");
        check(fig.shape.missing.empty(), "nothing is reported missing");
        check(fig.shape.jointsMoved == 1, "one joint is reported moved");
    }

    // --- 2. Dialed AND driven: limited, not doubled ---------------------------------------------
    {
        writeJson(presets / "Dialed Twice.duf",
                  preset(json::array({figureNode()}),
                         json::array({dial(character, "Some Character", "Some Character-0x1a", 1.0),
                                      dial(character, "Some Character Body", "Some Character Body", 1.0),
                                      dial("Base", "Navel", "Navel", 1.0)})));
        const pose::FigureData fig = load(library, presets / "Dialed Twice.duf");
        check(about(applied(fig, "Some Character Body"), 1.0),
              "a body dialed at 1 AND driven by 1 is applied at its limit, 1 — not at 2");
        check(!wasApplied(fig, "Navel"),
              "a navel dialed at 1 that the head takes off again (-1) is off");
    }

    // --- 3. Splines -----------------------------------------------------------------------------
    {
        writeJson(presets / "Larger.duf",
                  preset(json::array({figureNode()}), json::array({dial("Base", "Size", "Size", 0.5)})));
        const pose::FigureData larger = load(library, presets / "Larger.duf");
        check(about(applied(larger, "Large"), 0.5) && !wasApplied(larger, "Small"),
              "a control above zero drives its upper morph through the spline, and not the lower");
        writeJson(presets / "Smaller.duf",
                  preset(json::array({figureNode()}), json::array({dial("Base", "Size", "Size", -0.5)})));
        const pose::FigureData smaller = load(library, presets / "Smaller.duf");
        check(about(applied(smaller, "Small"), 0.5) && !wasApplied(smaller, "Large"),
              "… and below zero the lower, and not the upper");
        writeJson(presets / "Unsized.duf",
                  preset(json::array({figureNode()}), json::array({dial("Base", "Size", "Size", 0.0)})));
        const pose::FigureData unsized = load(library, presets / "Unsized.duf");
        check(unsized.shape.applied.empty(), "… and listed at zero, neither (a spline is not its knot count)");
    }

    // --- 4. A scene of several figures -----------------------------------------------------------
    {
        const json lashes = {{"id", "Lashes Figure"},
                             {"url", "/data/Maker/Lashes/Lashes.dsf#Lashes%20Figure"},
                             {"conform_target", "#Figure"},
                             {"parent", "#Figure"},
                             {"geometries", json::array({{{"id", "Lashes Shape"},
                                                          {"url", "/data/Maker/Lashes/Lashes.dsf#Lashes%20Shape"}}})}};
        json scene = preset(json::array({figureNode(), lashes}),
                            json::array({dial(character, "Some Character", "Some Character-0x1a", 1.0),
                                         {{"id", "Lash Length"},
                                          {"url", "/data/Maker/Lashes/Morphs/Maker/Base/Lash%20Length.dsf#Lash%20Length"},
                                          {"parent", "#Lashes%20Shape"},
                                          {"channel", {{"id", "value"}, {"current_value", 1.0}}}}}));
        // The figure's own "Eye Moisture" extends a base whose id has a space in it and takes its
        // color from that base; the follower's "Eye Moisture", listed after it, is red.
        scene["material_library"] = json::array(
            {{{"id", "Eye Moisture"},
              {"diffuse", {{"channel", {{"id", "diffuse"}, {"type", "float_color"}, {"value", {0.2, 0.4, 0.6}}}}}}}});
        scene["scene"]["materials"] = json::array(
            {{{"id", "Eye Moisture-1"}, {"url", "#Eye%20Moisture"}, {"geometry", "#geometry"}, {"groups", {"Eye Moisture"}}},
             {{"id", "Eye Moisture-2"},
              {"geometry", "#Lashes%20Shape"},
              {"groups", {"Eye Moisture"}},
              {"diffuse", {{"channel", {{"id", "diffuse"}, {"type", "float_color"}, {"current_value", {1.0, 0.0, 0.0}}}}}}}});
        writeJson(presets / "With Lashes.duf", scene);
        const pose::FigureData fig = load(library, presets / "With Lashes.duf");
        check(about(applied(fig, "Some Character Head"), 1.0), "in a scene of two figures the character keeps her dials");
        check(!wasApplied(fig, "Lash Length") && !missingMentions(fig, "Lash Length"),
              "… and is not given her follower's: its morph is neither applied to her nor reported");
        const pose::FigureMesh* moisture = zone(fig, "Eye Moisture");
        check(moisture && about(moisture->material.baseColor.r, 0.2) && about(moisture->material.baseColor.b, 0.6),
              "her zone keeps HER material where the follower has a zone of the same name — and a "
              "material base whose id has a space in it is found");
    }

    // --- 5. A control the preset carries in its own modifier_library ----------------------------
    {
        json embedded = preset(json::array({figureNode()}),
                               json::array({{{"id", "Embedded Control-1"},
                                             {"url", "#Embedded%20Control"},
                                             {"parent", "#Figure"},
                                             {"channel", {{"id", "value"}, {"current_value", 1.0}}}}}));
        embedded["modifier_library"] = json::array(
            {{{"id", "Embedded Control"},
              {"channel", {{"id", "value"}, {"value", 0}, {"min", 0}, {"max", 1}, {"clamped", true}}},
              {"formulas", json::array({drives("Embedded Control",
                                               valueOf(character, "Some Character Body", "Some Character Body"), 1.0)})}}});
        writeJson(presets / "Embedded Control.duf", embedded);
        const pose::FigureData fig = load(library, presets / "Embedded Control.duf");
        check(about(applied(fig, "Some Character Body"), 1.0),
              "a control defined in the preset itself drives the morph it names");
    }

    // --- 6. What cannot be applied is said --------------------------------------------------------
    {
        writeJson(presets / "Missing.duf",
                  preset(json::array({figureNode()}),
                         json::array({dial("Gone", "Gone", "Gone", 1.0),
                                      dial("Base", "Other Geometry", "Other Geometry", 1.0)})));
        const pose::FigureData fig = load(library, presets / "Missing.duf");
        check(fig.shape.applied.empty(), "a morph whose file is gone, and one made for another geometry, are not applied");
        check(missingMentions(fig, "Gone: file not found"), "… the first is reported with its file");
        check(missingMentions(fig, "Other Geometry: made for another geometry"), "… the second with its vertex count");
    }

    // --- 7. A character saved with a prop embedded in the scene file -----------------------------
    {
        json withProp = preset(json::array({figureNode(),
                                            {{"id", "Prop"},
                                             {"url", "#Prop"},
                                             {"parent", "#Figure"},
                                             {"geometries", json::array({{{"id", "Prop Shape-1"}, {"url", "#Prop%20Shape"}}})}}}),
                               json::array({dial(character, "Some Character", "Some Character-0x1a", 1.0)}));
        withProp["geometry_library"] = json::array(
            {{{"id", "Prop Shape"},
              {"vertices", {{"count", 3}, {"values", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}}}},
              {"polygon_material_groups", {{"count", 1}, {"values", {"Prop"}}}},
              {"polylist", {{"count", 1}, {"values", {{0, 0, 0, 1, 2}}}}}}});
        withProp["node_library"] = json::array({bone("Prop", "node", "", 0)});
        writeJson(presets / "With Prop.duf", withProp);
        std::string error;
        const pose::FigureData fig = load(library, presets / "With Prop.duf", &error);
        check(zone(fig, "Skin") != nullptr && zone(fig, "Prop") == nullptr && fig.bones.size() == 3,
              "a character whose file embeds a prop's geometry imports as the character, not the prop" +
                  (error.empty() ? std::string() : " (" + error + ")"));
        check(about(applied(fig, "Some Character Head"), 1.0), "… with her shape");
    }

    // --- 8. A wearable saved "to be added": the selected figure is a placeholder ------------------
    {
        const json placeholder = {{"id", "Figure"},
                                  {"url", "name://@selection/Figure:"},
                                  {"geometries", json::array({{{"id", "geometry"}, {"url", "name://@selection#geometries/Figure:"}}})}};
        const json lashes = {{"id", "Lashes Figure"},
                             {"url", "/data/Maker/Lashes/Lashes.dsf#Lashes%20Figure"},
                             {"conform_target", "name://@selection:"},
                             {"parent", "#Figure"},
                             {"geometries", json::array({{{"id", "Lashes Shape"},
                                                          {"url", "/data/Maker/Lashes/Lashes.dsf#Lashes%20Shape"}}})}};
        writeJson(presets / "Added Lashes.duf", preset(json::array({placeholder, lashes}), json::array()));
        std::string error;
        const pose::FigureData fig = load(library, presets / "Added Lashes.duf", &error);
        check(fig.meshes.size() == 1 && zone(fig, "Eye Moisture") != nullptr && boneNamed(fig, "lash") != nullptr,
              "a wearable listed after a placeholder for the selected figure imports as itself" +
                  (error.empty() ? std::string() : " (" + error + ")"));
        const pose::FigureBone* lash = boneNamed(fig, "lash");
        check(lash && lash->parent >= 0 && fig.bones[static_cast<std::size_t>(lash->parent)].name == "Lashes Figure",
              "… and a bone under a root node whose id has a space in it keeps its parent");
    }

    // --- 9. A morph file that is not well typed costs that morph, never the figure ----------------
    {
        writeJson(presets / "Broken.duf",
                  preset(json::array({figureNode()}), json::array({dial("Base", "Broken", "Broken", 1.0)})));
        std::string error;
        const pose::FigureData fig = load(library, presets / "Broken.duf", &error);
        check(zone(fig, "Skin") != nullptr,
              "a morph file with a wrongly typed delta and entries that are no formulas does not fail the import" +
                  (error.empty() ? std::string() : " (" + error + ")"));
        check(about(applied(fig, "Some Character Body"), 1.0),
              "… the one formula of it that can be read still drives what it names");
        check(!wasApplied(fig, "Broken") && missingMentions(fig, "Broken: its deltas could not be read"),
              "… and the morph itself is left out, and said");
    }

    // --- 10. A skeleton listed children first comes out parents first ------------------------------
    {
        std::string error;
        const pose::FigureData fig = load(library, library / "data/Maker/Unordered/Unordered.dsf", &error);
        bool parentsFirst = fig.bones.size() == 3;
        for (std::size_t i = 0; i < fig.bones.size(); ++i) {
            parentsFirst = parentsFirst && fig.bones[i].parent < static_cast<int>(i);
        }
        check(parentsFirst, "every bone's parent has a lower index than the bone (the engine's contract)" +
                                (error.empty() ? std::string() : " (" + error + ")"));
        const pose::FigureBone* chest = boneNamed(fig, "chest");
        const pose::FigureBone* hip = boneNamed(fig, "hip");
        check(chest && hip && chest->parent >= 0 && fig.bones[static_cast<std::size_t>(chest->parent)].name == "hip" &&
                  hip->parent >= 0 && fig.bones[static_cast<std::size_t>(hip->parent)].name == "Figure",
              "… and each keeps the parent its file names");
    }

    // --- 11. A corrective's gate reads what the PRESET set its switch to ---------------------------
    {
        writeJson(presets / "Plain.duf", preset(json::array({figureNode()}), json::array()));
        const pose::FigureData plain = load(library, presets / "Plain.duf");
        check(about(gateOf(plain, "Gated Corrective"), 1.0),
              "a corrective behind a switch that is on by default (a boolean channel) is live");
        writeJson(presets / "Correctives Off.duf",
                  preset(json::array({figureNode()}),
                         json::array({dial("Base", "Correctives On", "Correctives On", 0.0)})));
        const pose::FigureData off = load(library, presets / "Correctives Off.duf");
        check(corrective(off, "Gated Corrective") == nullptr,
              "… and dropped where the preset switches it OFF: the dial at zero, not the file's default");
    }

    // --- 12. A driven morph that is nowhere is reported — not found in its driver's file -----------
    {
        writeJson(presets / "Lone Head.duf",
                  preset(json::array({figureNode()}), json::array({dial("Base", "Lone Head", "Lone Head", 1.0)})));
        const pose::FigureData fig = load(library, presets / "Lone Head.duf");
        check(about(applied(fig, "Lone Head"), 1.0) && fig.shape.applied.size() == 1,
              "a head that drives a morph no file defines is applied once, and nothing else is");
        check(!wasApplied(fig, "Nowhere") && missingMentions(fig, "Nowhere: named by id alone"),
              "… the missing morph is reported, and not baked from the head's own deltas");
    }

    // --- 13. A scene entry that carries no value is no dial ----------------------------------------
    {
        const json limitsOnly = {{"id", "Default On"},
                                 {"url", std::string(kMorphs) + "/Base/Default%20On.dsf#Default%20On"},
                                 {"parent", "#Figure"},
                                 {"channel", {{"id", "value"}, {"min", -2}, {"max", 2}}}};
        writeJson(presets / "Limits Only.duf", preset(json::array({figureNode()}), json::array({limitsOnly})));
        const pose::FigureData fig = load(library, presets / "Limits Only.duf");
        check(about(applied(fig, "Default On"), 1.0),
              "a channel the scene lists with new limits and no value keeps its file's default (1), not 0");
    }

    // --- 14. A material's base in ANOTHER file is not shadowed by a same-named one of the preset ----
    {
        json shadowed = preset(json::array({figureNode()}), json::array());
        shadowed["material_library"] = json::array(
            {{{"id", "Skin Base"},
              {"diffuse", {{"channel", {{"id", "diffuse"}, {"type", "float_color"}, {"value", {0.1, 0.9, 0.1}}}}}}}});
        shadowed["scene"]["materials"] = json::array(
            {{{"id", "Skin-1"},
              {"url", "/data/Maker/Shaders/Shader.dsf#Skin%20Base"},
              {"geometry", "#geometry"},
              {"groups", {"Skin"}}}});
        writeJson(presets / "Shadowed.duf", shadowed);
        const pose::FigureData fig = load(library, presets / "Shadowed.duf");
        const pose::FigureMesh* skin = zone(fig, "Skin");
        check(skin && about(skin->material.baseColor.r, 0.9) && about(skin->material.baseColor.g, 0.1),
              "a material that extends a base of another file gets THAT base, whatever the preset's own "
              "material_library calls its entries");
    }

    // --- 15. A geometry that lists no material zones keeps its UVs ---------------------------------
    {
        std::string error;
        const pose::FigureData fig = load(library, library / "data/Maker/Untagged/Untagged.dsf", &error);
        bool mapped = fig.meshes.size() == 1 && !fig.meshes[0].vertices.empty();
        if (mapped) {
            // Every corner's u lies between the layout's 0.1 and 0.8, and v is the flipped 0.25:
            // read through no set at all, every vertex sat at (0, 1).
            for (const pose::Vertex& v : fig.meshes[0].vertices) {
                mapped = mapped && v.uv.x > 0.09f && v.uv.x < 0.81f && about(v.uv.y, 0.75);
            }
        }
        check(mapped, "a geometry with no material zones is one zone, read through its default UV set" +
                          (error.empty() ? std::string() : " (" + error + ")"));
    }

    // --- 16. A corrective made for another geometry is refused, and said ---------------------------
    {
        const pose::FigureData plain = load(library, presets / "Plain.duf");
        const bool said = std::any_of(plain.skipped.begin(), plain.skipped.end(), [](const std::string& s) {
            return s.find("Foreign Corrective") != std::string::npos &&
                   s.find("made for another geometry (99 vertices, this one has 8)") != std::string::npos;
        });
        check(corrective(plain, "Foreign Corrective") == nullptr && said &&
                  corrective(plain, "Gated Corrective") != nullptr,
              "a corrective whose morph was made for another geometry is left out and reported; its "
              "neighbour in the pack is carried");
    }

    const auto leftOut = [](const pose::FigureData& fig, const std::string& id, const std::string& why) {
        return std::any_of(fig.skipped.begin(), fig.skipped.end(), [&](const std::string& s) {
            return s.find(id) != std::string::npos && s.find(why) != std::string::npos;
        });
    };

    // --- 17. A corrective's weight DIVIDED by a dial: the gate "push <dial>, inv" ------------------
    {
        const pose::FigureData plain = load(library, presets / "Plain.duf");
        check(about(gateOf(plain, "Divided Corrective"), 0.5),
              "a corrective gated by 'push <dial>, inv' is DIVIDED by the dial: its default of 2 gives a half");
        writeJson(presets / "Fit Four.duf",
                  preset(json::array({figureNode()}), json::array({dial("Base", "Fit", "Fit", 4.0)})));
        check(about(gateOf(load(library, presets / "Fit Four.duf"), "Divided Corrective"), 0.25),
              "… and the dial at 4 a quarter (read without the inv, it was multiplied by 4)");
        writeJson(presets / "Fit Zero.duf",
                  preset(json::array({figureNode()}), json::array({dial("Base", "Fit", "Fit", 0.0)})));
        check(corrective(load(library, presets / "Fit Zero.duf"), "Divided Corrective") == nullptr,
              "… and a divisor of 0 switches the corrective off: the reciprocal of 0 reads 0");
    }

    // --- 18. The walk's inv, neg and step spline; an operation nobody knows -------------------------
    {
        writeJson(presets / "Ratio.duf",
                  preset(json::array({figureNode()}), json::array({dial("Base", "Ratio", "Ratio", 4.0)})));
        const pose::FigureData fig = load(library, presets / "Ratio.duf");
        check(about(applied(fig, "Reciprocal"), 0.25), "a control's 'inv' is the reciprocal: a dial of 4 drives a quarter");
        check(about(applied(fig, "Negated"), -4.0), "… and its 'neg' the negative: -4");
        check(!wasApplied(fig, "Stepped"),
              "a spline_constant holds its earlier knot between two knots: 0 at 4 of 8, not the curve's half");
        check(!wasApplied(fig, "Mystery") && missingMentions(fig, "Mystery: a formula that drives it uses an "
                                                                  "operation this importer does not know (frobnicate)"),
              "a formula with an operation outside the vocabulary is not run: what it drives is left alone, and said");
    }

    // --- 19. A corrective whose driver cannot be read or said is refused whole ----------------------
    {
        const pose::FigureData plain = load(library, presets / "Plain.duf");
        check(corrective(plain, "Odd Corrective") == nullptr &&
                  leftOut(plain, "Odd Corrective", "an operation this importer does not know (frobnicate)"),
              "a corrective whose driver uses an unknown operation is left out and reported, not carried without it");
        check(corrective(plain, "Inverse Corrective") == nullptr &&
                  leftOut(plain, "Inverse Corrective", "applies 'inv' to a joint's angle"),
              "… as is one that takes the reciprocal of a joint's angle, which the pose-time program cannot say");
    }

    // --- 20. A spline_linear driver stays linear at pose time --------------------------------------
    {
        const pose::FigureData plain = load(library, presets / "Plain.duf");
        const auto splineOf = [&](const std::string& id) -> const pose::CorrectiveOp* {
            const pose::PoseCorrective* c = corrective(plain, id);
            if (!c || c->sumFormulas.size() != 1) {
                return nullptr;
            }
            for (const pose::CorrectiveOp& op : c->sumFormulas[0].ops) {
                if (op.kind == pose::CorrectiveOp::Kind::Spline) {
                    return &op;
                }
            }
            return nullptr;
        };
        const pose::CorrectiveOp* linear = splineOf("Linear Corrective");
        const pose::CorrectiveOp* smooth = splineOf("Smooth Corrective");
        check(linear && linear->linear && linear->knots.size() == 2 && smooth && !smooth->linear &&
                  smooth->knots.size() == 2,
              "a corrective driven through spline_linear is marked linear for the engine; a spline_tcb one is not");
    }

    // --- 21. A driver joint whose id has a space in it ---------------------------------------------
    {
        std::string error;
        const pose::FigureData fig = load(library, library / "data/Maker/Spaced/Spaced.dsf", &error);
        const pose::PoseCorrective* arm = corrective(fig, "Arm Corrective");
        check(arm && arm->sumFormulas.size() == 1 && !arm->sumFormulas[0].ops.empty() &&
                  arm->sumFormulas[0].ops[0].kind == pose::CorrectiveOp::Kind::PushRotation &&
                  arm->sumFormulas[0].ops[0].bone == "Left Arm",
              "a corrective driven by a joint whose id has a space in it is carried, the joint named as the "
              "skeleton names it" + (error.empty() ? std::string() : " (" + error + ")"));
    }

    fs::remove_all(library, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && (std::string(argv[1]) == "--report" || std::string(argv[1]) == "--dump")) {
        std::vector<std::string> roots;
        for (int i = 3; i < argc; ++i) {
            roots.emplace_back(argv[i]);
        }
        return std::string(argv[1]) == "--dump" ? dump(argv[2], std::move(roots))
                                                : report(argv[2], std::move(roots));
    }
    if (argc > 1) {
        std::printf("usage: PoseStudioFigureCheck                                   the checks\n"
                    "       PoseStudioFigureCheck --report <preset> [content root ...]\n"
                    "       PoseStudioFigureCheck --dump <preset> [content root ...]\n");
        return 2;
    }
    return runChecks();
}
