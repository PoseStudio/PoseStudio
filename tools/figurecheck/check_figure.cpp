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
 */

#include "figureimporter.h"

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
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

// The folder above @p file that directly contains "data" — the content root a preset's
// "/data/…" references resolve against (what the application detects for an imported file).
std::vector<std::string> detectRoots(const std::string& file) {
    std::vector<std::string> roots;
    std::error_code ec;
    for (fs::path dir = fs::absolute(fs::path(file), ec).parent_path(); !dir.empty();
         dir = dir.parent_path()) {
        if (fs::is_directory(dir / "data", ec)) {
            roots.push_back(dir.string());
            break;
        }
        if (dir == dir.root_path()) {
            break;
        }
    }
    return roots;
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
        // colour from that base; the follower's "Eye Moisture", listed after it, is red.
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

    fs::remove_all(library, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--report") {
        std::vector<std::string> roots;
        for (int i = 3; i < argc; ++i) {
            roots.emplace_back(argv[i]);
        }
        return report(argv[2], std::move(roots));
    }
    if (argc > 1) {
        std::printf("usage: PoseStudioFigureCheck                                   the checks\n"
                    "       PoseStudioFigureCheck --report <preset> [content root ...]\n");
        return 2;
    }
    return runChecks();
}
