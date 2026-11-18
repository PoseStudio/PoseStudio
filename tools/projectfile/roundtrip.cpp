/**
 * @file roundtrip.cpp
 * @brief Round-trip and rejection checks for the `.pss` project codec.
 *
 * A standalone guard for the file-format contract: it writes a fully-populated
 * ProjectDocument, reads it back and compares every field bit-for-bit, then
 * feeds the reader the failure cases (bad JSON, wrong marker, higher version,
 * missing file) and a forward-compatibility document with unknown fields. It
 * links only the codec — `scene/projectfile.cpp` with the material's own
 * (`materialjson.cpp`, `materialparams.cpp`) — no Qt, no Vulkan — so it runs
 * anywhere the app builds, and is registered with CTest
 * (`ctest -R project-file-check`).
 *
 * Exit code = number of failed checks (0 = green).
 */

#include "projectfile.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>

using pose::ProjectCamera;
using pose::ProjectDocument;
using pose::ProjectEnvironment;
using pose::ProjectFigure;

namespace {

int failures = 0;

/// One scratch directory for every file this check writes: the OS temp dir
/// (the platform's — no hardcoded /tmp), created up front.
const std::string kDir =
    (std::filesystem::temp_directory_path() / "pss-rt").string();

std::string path(const char* name) { return kDir + "/" + name; }

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

bool vec3Equal(const glm::vec3& a, const glm::vec3& b) { return glm::all(glm::equal(a, b)); }
bool vec4Equal(const glm::vec4& a, const glm::vec4& b) { return glm::all(glm::equal(a, b)); }

/// A fully-populated document: two figures (one rich, one at defaults), a
/// non-default environment and camera.
ProjectDocument makeDoc() {
    ProjectDocument doc;

    ProjectFigure f1;
    f1.source = path("hero.duf");
    f1.pose.emplace_back("lShldrBend", glm::vec3(-40.0f, 0.0f, 3.2f));
    f1.pose.emplace_back("rShldrBend", glm::vec3(-40.0f, 0.0f, -3.2f));
    f1.pins.push_back("lHand");
    f1.rootBone = "hip";
    f1.rootTranslation = glm::vec3(0.0f, -0.31f, 0.12f);
    f1.translation = glm::vec3(0.5f, 0.0f, -1.25f);
    // A material edit on one zone (the Materials tab): every value off its default.
    pose::ProjectMaterial torso;
    torso.zone = "Torso";
    torso.params.baseColor = glm::vec3(0.8f, 0.55f, 0.4f);
    torso.params.opacity = 0.9f;
    torso.params.roughness = 0.35f;
    torso.params.metalness = 0.25f;
    torso.params.specular = 0.6f;
    torso.params.normalStrength = 2.5f;
    torso.params.translucency = 0.75f;
    torso.params.topCoat = 0.4f;
    torso.params.topCoatRoughness = 0.3f;
    torso.params.detailWeight = 0.45f;
    torso.params.detailTiles = 24.0f;
    torso.params.lobe1Roughness = 0.6f;
    torso.params.lobe2Roughness = 0.2f;
    torso.params.lobeRatio = 0.85f;
    // The canonical material's further channels, and two texture slots: a file and a cleared one.
    torso.params.ior = 1.33f;
    torso.params.specularColor = glm::vec3(0.9f, 0.8f, 0.7f);
    torso.params.anisotropy = 0.2f;
    torso.params.transmission = 0.3f;
    torso.params.transmissionColor = glm::vec3(0.2f, 0.9f, 0.3f);
    torso.params.thinWalled = true;
    torso.params.flipNormal = true; // (a DirectX-style normal map: read flipped)
    torso.params.transmissionRoughness = 0.125f;
    torso.params.subsurfaceRadius = 2.5f;
    torso.params.sheen = 0.4f;
    torso.params.coatIor = 1.7f;
    torso.params.thinFilm = 0.6f;
    torso.params.thinFilmThickness = 320.0f;
    torso.params.emission = 2.0f;
    torso.params.emissionColor = glm::vec3(1.0f, 0.5f, 0.25f);
    torso.params.bumpStrength = 1.75f;
    torso.params.uvTiling = glm::vec2(2.0f, 3.0f);
    torso.params.uvOffset = glm::vec2(0.25f, -0.5f);
    // THE SMOOTHING: the zone's faces shaded by angle (the Materials tab's Smooth by Angle).
    torso.params.smoothByAngle = true;
    torso.params.smoothAngle = 42.5f;
    torso.textures.push_back({"basecolor", "C:/textures/skin.png"});
    torso.textures.push_back({"cutout", ""});
    // Two overlays: a tattoo moved from where it was painted, with every color control off its
    // default, and a relief-only scar.
    pose::MaterialOverlay tattoo;
    tattoo.offset = glm::vec2(0.125f, -0.0625f);
    tattoo.rotation = -37.5f;
    tattoo.scale = 1.75f;
    tattoo.color = "C:/overlays/dragon.png";
    tattoo.name = "Dragon, left thigh";
    tattoo.opacityMap = "C:/overlays/dragon_mask.png";
    tattoo.blend = pose::OverlayBlend::SoftLight;
    tattoo.opacity = 0.8f;
    tattoo.tint = glm::vec3(0.9f, 0.5f, 0.25f);
    tattoo.brightness = 0.1f;
    tattoo.contrast = -0.2f;
    tattoo.gamma = 1.4f;
    tattoo.hue = -35.0f;
    tattoo.saturation = 0.3f;
    tattoo.gloss = 0.6f;
    tattoo.metallic = 0.75f;
    tattoo.metallicMap = "C:/overlays/dragon_gold.png";
    pose::MaterialOverlay scar;
    scar.enabled = false;
    scar.bump = "C:/overlays/scar_bump.png";
    scar.bumpStrength = -1.5f;
    scar.normal = "C:/overlays/scar_normal.png";
    scar.normalStrength = 2.0f;
    scar.displacement = "C:/overlays/scar_disp.png";
    scar.displacementStrength = 0.5f;
    // ... and a third as the tab adds one: empty.
    torso.params.overlays = {tattoo, scar, pose::MaterialOverlay()};
    f1.materials.push_back(torso);
    f1.rotation = glm::normalize(glm::vec4(0.0f, 0.7071068f, 0.0f, 0.7071068f));
    f1.scale = glm::vec3(1.5f, 2.25f, 0.75f);
    doc.figures.push_back(f1);

    ProjectFigure f2; // minimal: every field at its default
    f2.source = path("model.obj");
    doc.figures.push_back(f2);

    doc.environment.hdri = path("studio.hdr");
    doc.environment.settings.exposure = 0.9f;
    doc.environment.settings.keyIntensity = 1.4f;
    doc.environment.settings.tonemap = false;
    doc.environment.settings.shadowsEnabled = true;

    // Three light objects: a spotlight with every field off its default, an area light switched
    // off that casts no shadow, and one left as it is made.
    pose::SceneLight spot;
    spot.name = "Key, camera left";
    spot.kind = pose::LightKind::Spot;
    spot.position = glm::vec3(-1.25f, 2.5f, 1.75f);
    spot.turnDeg = -37.5f;
    spot.tiltDeg = 22.25f;
    spot.color = glm::vec3(1.0f, 0.5f, 0.125f);
    spot.intensity = 12.5f;
    spot.radius = 0.125f;
    spot.coneDeg = 35.0f;
    spot.blend = 0.375f;
    pose::SceneLight soft;
    soft.name = "Softbox";
    soft.kind = pose::LightKind::Area;
    soft.enabled = false;
    soft.castShadow = false;
    soft.width = 1.5f;
    soft.height = 0.625f;
    pose::SceneLight sun;
    sun.kind = pose::LightKind::Sun;
    sun.sunAngleDeg = 0.5f;
    doc.lights = {spot, soft, sun, pose::SceneLight()};

    doc.camera.target = glm::vec3(0.1f, 1.2f, -0.3f);
    doc.camera.yaw = 0.61f;
    doc.camera.pitch = -0.22f;
    doc.camera.distance = 4.25f;
    doc.camera.ortho = true;
    return doc;
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

void checkRoundTrip() {
    const ProjectDocument in = makeDoc();
    check(pose::writeProjectFile(path("roundtrip.pss"), in), "write");

    ProjectDocument out;
    std::string error;
    if (!pose::readProjectFile(path("roundtrip.pss"), out, error)) {
        std::printf("  read error: %s\n", error.c_str());
    }
    check(pose::readProjectFile(path("roundtrip.pss"), out, error), "read round-trip");

    check(out.figures.size() == 2, "figure count");
    if (out.figures.size() != 2) {
        return;
    }
    const ProjectFigure& a = in.figures[0];
    const ProjectFigure& b = out.figures[0];
    check(a.source == b.source, "f1 source");
    check(b.pose.size() == 2 && b.pose[0].first == a.pose[0].first &&
              vec3Equal(b.pose[0].second, a.pose[0].second) &&
              b.pose[1].first == a.pose[1].first && vec3Equal(b.pose[1].second, a.pose[1].second),
          "f1 pose rows");
    check(b.pins.size() == 1 && b.pins[0] == "lHand", "f1 pins");
    check(b.rootBone == "hip" && vec3Equal(b.rootTranslation, a.rootTranslation),
          "f1 root bone + translation");
    check(vec3Equal(b.translation, a.translation), "f1 translation");
    check(vec4Equal(b.rotation, a.rotation), "f1 rotation (quaternion)");
    check(vec3Equal(b.scale, a.scale), "f1 scale");
    check(b.materials.size() == 1 && b.materials[0].zone == "Torso" && b.materials[0].params == a.materials[0].params,
          "f1 material override (every value)");
    check(b.materials.size() == 1 && b.materials[0].textures.size() == 2 && b.materials[0].textures[0].slot == "basecolor" &&
              b.materials[0].textures[0].source == "C:/textures/skin.png" && b.materials[0].textures[1].slot == "cutout" &&
              b.materials[0].textures[1].source.empty(),
          "f1 material texture slots (a file and a cleared slot)");
    check(b.materials.size() == 1 && b.materials[0].params.overlays.size() == 3 &&
              b.materials[0].params.overlays[0].name == "Dragon, left thigh" && b.materials[0].params.overlays[1].name.empty() &&
              b.materials[0].params.overlays[0].opacityMap == "C:/overlays/dragon_mask.png" && b.materials[0].params.overlays[1].opacityMap.empty() &&
              b.materials[0].params.overlays[0].metallic == 0.75f && b.materials[0].params.overlays[0].metallicMap == "C:/overlays/dragon_gold.png" &&
              b.materials[0].params.overlays[1].metallic == 0.0f &&
              b.materials[0].params.overlays[2] == pose::MaterialOverlay() &&
              b.materials[0].params.overlays == a.materials[0].params.overlays &&
              b.materials[0].params.overlays[0].blend == pose::OverlayBlend::SoftLight && !b.materials[0].params.overlays[1].enabled &&
              b.materials[0].params.overlays[0].offset == glm::vec2(0.125f, -0.0625f) &&
              b.materials[0].params.overlays[0].rotation == -37.5f && b.materials[0].params.overlays[0].scale == 1.75f &&
              b.materials[0].params.overlays[1].offset == glm::vec2(0.0f) && b.materials[0].params.overlays[1].rotation == 0.0f &&
              b.materials[0].params.overlays[1].scale == 1.0f,
          "f1 overlays (a tattoo's name, opacity map, metal, place, turn, size and color controls, a disabled relief-only scar, an empty one, in order)");

    check(b.materials.size() == 1 && b.materials[0].names("smoothByAngle") && b.materials[0].names("smoothAngle") &&
              b.materials[0].params.smoothByAngle && b.materials[0].params.smoothAngle == 42.5f,
          "f1 smoothing (by angle, 42.5 degrees — and the file SAYS so)");
    check(b.materials.size() == 1 && b.materials[0].params.flipNormal && !pose::MaterialParams().flipNormal,
          "f1 Flip Normal rides the round trip (and is off where a file does not say)");

    const ProjectFigure& c = out.figures[1];
    check(c.source == path("model.obj") && c.pose.empty() && c.pins.empty() && c.materials.empty() &&
              c.rootBone.empty() && vec3Equal(c.translation, glm::vec3(0.0f)) &&
              vec3Equal(c.scale, glm::vec3(1.0f)) &&
              vec4Equal(c.rotation, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
          "f2 defaults");

    check(out.environment.hdri == path("studio.hdr"), "environment hdri");
    const pose::LightingSettings& ls = out.environment.settings;
    check(ls.exposure == 0.9f && ls.keyIntensity == 1.4f && !ls.tonemap && ls.shadowsEnabled,
          "environment settings (incl. the bool dials)");

    check(out.lights.size() == 4 && out.lights == in.lights && out.lights[0].kind == pose::LightKind::Spot &&
              out.lights[0].name == "Key, camera left" && out.lights[0].position == glm::vec3(-1.25f, 2.5f, 1.75f) &&
              out.lights[0].turnDeg == -37.5f && out.lights[0].tiltDeg == 22.25f &&
              out.lights[0].color == glm::vec3(1.0f, 0.5f, 0.125f) && out.lights[0].intensity == 12.5f &&
              out.lights[0].radius == 0.125f && out.lights[0].coneDeg == 35.0f && out.lights[0].blend == 0.375f &&
              out.lights[1].kind == pose::LightKind::Area && !out.lights[1].enabled && !out.lights[1].castShadow &&
              out.lights[1].width == 1.5f && out.lights[1].height == 0.625f &&
              out.lights[2].kind == pose::LightKind::Sun && out.lights[2].sunAngleDeg == 0.5f &&
              out.lights[3] == pose::SceneLight(),
          "lights (a spotlight's every field, an area light switched off, a sun, a default one, in order)");

    const ProjectCamera& cam = out.camera;
    check(vec3Equal(cam.target, glm::vec3(0.1f, 1.2f, -0.3f)) && cam.yaw == 0.61f &&
              cam.pitch == -0.22f && cam.distance == 4.25f && cam.ortho,
          "camera framing");
}

void checkRejections() {
    ProjectDocument d;
    std::string error;

    writeText(path("bad.json"), "{ this is not json");
    check(!pose::readProjectFile(path("bad.json"), d, error) && !error.empty(),
          "bad JSON rejected with an error");

    writeText(path("marker.json"),
              R"({"format":"other.app","version":1,"figures":[],"environment":{"settings":{}},"camera":{}})");
    check(!pose::readProjectFile(path("marker.json"), d, error), "wrong marker rejected");

    writeText(path("v2.json"),
              R"({"format":"posestudio.project","version":2,"figures":[],"environment":{"settings":{}},"camera":{}})");
    check(!pose::readProjectFile(path("v2.json"), d, error), "higher version rejected");

    check(!pose::readProjectFile(path("nosuchfile.pss"), d, error), "missing file rejected");

    // Absent top-level sections are optional (defaults apply) — the rejection list is
    // missing file / bad JSON / wrong marker / higher version / non-finite values.
    writeText(path("bare.json"), R"({"format":"posestudio.project","version":1})");
    d = ProjectDocument();
    check(pose::readProjectFile(path("bare.json"), d, error) && d.figures.empty(),
          "absent sections fall back to defaults");
}

void checkForwardCompatibility() {
    writeText(path("future.json"),
              R"({"format":"posestudio.project","version":1,"figures":[{"source":"/x.duf",
                  "pose":[["hip",1.0,2.0,3.0]],"mysteryField":{"a":1},"pins":[],
                  "materials":[{"zone":"Torso","roughness":0.4,"futureKey":1,"textures":[{"slot":"glow","source":"x.png"},{"slot":"normal","source":"n.png"}]}]}],
                 "environment":{"hdri":"","settings":{"exposure":0.5,"futureDial":42}},
                 "camera":{"target":[0,1,0],"yaw":0.1,"pitch":0.2,"distance":3.0,"orthographic":false,
                           "fovDeg":60},"topLevelUnknown":[1,2,3]})");
    ProjectDocument d;
    std::string error;
    check(pose::readProjectFile(path("future.json"), d, error), "unknown fields ignored");
    check(d.figures.size() == 1 && d.figures[0].pose.size() == 1 &&
              vec3Equal(d.figures[0].pose[0].second, glm::vec3(1.0f, 2.0f, 3.0f)) &&
              d.environment.settings.exposure == 0.5f && d.camera.distance == 3.0f,
          "known values survive");
    // (A zone that says nothing of its smoothing — a project from before there was one — is
    // marked so: the loader then leaves the zone what the import gives it.)
    check(d.figures.size() == 1 && d.figures[0].materials.size() == 1 && !d.figures[0].materials[0].names("smoothByAngle") &&
              !d.figures[0].materials[0].params.smoothByAngle,
          "a zone without a smoothing is read as not saying");
    check(d.figures[0].materials.size() == 1 && d.figures[0].materials[0].zone == "Torso" &&
              d.figures[0].materials[0].params.roughness == 0.4f &&
              d.figures[0].materials[0].params.metalness == 0.0f && // absent: the default
              d.figures[0].materials[0].textures.size() == 1 && d.figures[0].materials[0].textures[0].slot == "normal", // an unknown slot left out
          "a partial material entry reads with defaults");

    // Overlays from a later version: an unknown key is ignored, an unknown blend keeps the
    // default (Multiply), absent controls keep theirs; a malformed one rejects the file.
    writeText(path("overlays.json"),
              R"({"format":"posestudio.project","version":1,"figures":[{"source":"/x.duf",
                  "materials":[{"zone":"Face","overlays":[{"color":"lips.png","blend":"glitter","opacity":0.5,"sparkle":3},
                                                          {"bump":"scar.png"}]}]}]})");
    d = ProjectDocument();
    check(pose::readProjectFile(path("overlays.json"), d, error) && d.figures.size() == 1 && d.figures[0].materials.size() == 1 &&
              d.figures[0].materials[0].params.overlays.size() == 2 &&
              d.figures[0].materials[0].params.overlays[0].color == "lips.png" &&
              d.figures[0].materials[0].params.overlays[0].blend == pose::OverlayBlend::Multiply &&
              d.figures[0].materials[0].params.overlays[0].opacity == 0.5f &&
              d.figures[0].materials[0].params.overlays[0].gamma == 1.0f && d.figures[0].materials[0].params.overlays[0].enabled &&
              d.figures[0].materials[0].params.overlays[0].offset == glm::vec2(0.0f) &&
              d.figures[0].materials[0].params.overlays[0].rotation == 0.0f && d.figures[0].materials[0].params.overlays[0].scale == 1.0f &&
              d.figures[0].materials[0].params.overlays[0].metallic == 0.0f && d.figures[0].materials[0].params.overlays[0].metallicMap.empty() &&
              d.figures[0].materials[0].params.overlays[0].name.empty() && d.figures[0].materials[0].params.overlays[0].opacityMap.empty() &&
              d.figures[0].materials[0].params.overlays[1].bump == "scar.png" &&
              d.figures[0].materials[0].params.overlays[1].bumpStrength == 1.0f,
          "overlays read with defaults (an unknown blend and key ignored)");
    writeText(path("badoverlay.json"),
              R"({"format":"posestudio.project","version":1,"figures":[{"source":"/x.duf",
                  "materials":[{"zone":"Face","overlays":[{"color":"lips.png","opacity":"half"}]}]}]})");
    check(!pose::readProjectFile(path("badoverlay.json"), d, error), "a malformed overlay rejects the file");

    // Lights: absent = none (a project from before them); a kind this build does not know and an
    // unknown key are read past, every absent field is the default; a malformed one rejects the file.
    d = ProjectDocument();
    check(pose::readProjectFile(path("bare.json"), d, error) && d.lights.empty(), "a project without lights has none");
    writeText(path("lights.json"),
              R"({"format":"posestudio.project","version":1,
                  "lights":[{"kind":"laser","intensity":9.0,"gobo":"leaves.png"},{"kind":"sun","tiltDeg":60.0}]})");
    d = ProjectDocument();
    check(pose::readProjectFile(path("lights.json"), d, error) && d.lights.size() == 2 &&
              d.lights[0].kind == pose::LightKind::Point && d.lights[0].intensity == 9.0f && d.lights[0].enabled &&
              d.lights[0].coneDeg == pose::SceneLight().coneDeg && d.lights[0].position == pose::SceneLight().position &&
              d.lights[1].kind == pose::LightKind::Sun && d.lights[1].tiltDeg == 60.0f,
          "lights read with defaults (an unknown kind and key ignored)");
    writeText(path("badlight.json"),
              R"({"format":"posestudio.project","version":1,"lights":[{"kind":"spot","intensity":"bright"}]})");
    check(!pose::readProjectFile(path("badlight.json"), d, error), "a malformed light rejects the file");
    writeText(path("badlights.json"), R"({"format":"posestudio.project","version":1,"lights":{"kind":"spot"}})");
    check(!pose::readProjectFile(path("badlights.json"), d, error), "lights that are no list reject the file");
}

/// A NAME THAT IS NOT UTF-8 (a zone or a bone named by a model file in another encoding: an
/// OBJ's `newmtl M\xE9tal` in Latin-1) must not cost the project: the writer used to open —
/// and so empty — the file and then throw out of the serializer.
void checkForeignNames() {
    ProjectDocument doc;
    ProjectFigure   fig;
    fig.source = path("model.obj");
    fig.pose.emplace_back("Os\xE9", glm::vec3(1.0f, 2.0f, 3.0f)); // (Latin-1: not valid UTF-8)
    pose::ProjectMaterial metal;
    metal.zone = "M\xE9tal";
    metal.params.roughness = 0.25f;
    fig.materials.push_back(metal);
    doc.figures.push_back(fig);
    bool wrote = false;
    try {
        wrote = pose::writeProjectFile(path("foreign.pss"), doc);
    } catch (...) {
        wrote = false;
    }
    ProjectDocument back;
    std::string     error;
    check(wrote && pose::readProjectFile(path("foreign.pss"), back, error) && back.figures.size() == 1 &&
              back.figures[0].materials.size() == 1 && back.figures[0].materials[0].params.roughness == 0.25f &&
              back.figures[0].materials[0].zone == "M\xEF\xBF\xBDtal" && back.figures[0].pose.size() == 1,
          "a name that is not valid UTF-8 is written with the replacement character, and the file reads");
}

/// A PROJECT FROM AN OLDER BUILD names fewer values than this build knows. The reader records
/// which keys a zone's object names; the loader puts only those over the re-imported zone's own
/// material (materialWithNamedValues), so a value the file predates stays the import's instead
/// of becoming the struct's default.
void checkNamedValues() {
    // The file of the round trip names every value of the zone.
    ProjectDocument full;
    std::string     error;
    const bool      read = pose::readProjectFile(path("roundtrip.pss"), full, error) && !full.figures.empty() && full.figures[0].materials.size() == 1;
    check(read, "the round-trip file reads again");
    if (!read) {
        return;
    }
    const pose::ProjectMaterial& all = full.figures[0].materials[0];
    // (An imported zone as an importer might leave it: values off the struct's defaults, an eye's
    // clear shell — its transmission roughness 0, where the default is "the surface's", -1.)
    pose::MaterialParams imported;
    imported.transmissionRoughness = 0.0f;
    imported.roughness = 0.26f;
    imported.ior = 1.38f;
    imported.smoothByAngle = true;
    imported.smoothAngle = 30.0f;
    imported.uvTiling = glm::vec2(3.0f, 3.0f);
    imported.coatColor = glm::vec3(0.5f, 0.6f, 0.7f);
    imported.thinWalled = true;
    imported.maps[0].source = pose::kImportedTexture;
    const pose::MaterialParams whole = pose::materialWithNamedValues(imported, all.params, all.named);
    check(all.names("transmissionRoughness") && all.names("zone") && whole.sameValues(all.params) && whole.maps == imported.maps,
          "a zone that names every value replaces the zone's own, value for value (the maps stay the zone's)");

    // The same zone as a build before the transmission roughness, the smoothing and the UV
    // placement wrote it.
    writeText(path("older.json"),
              R"({"format":"posestudio.project","version":1,"figures":[{"source":"/x.duf",
                  "materials":[{"zone":"Cornea","roughness":0.4,"opacity":0.5,"baseColor":[0.1,0.2,0.3],"thinWalled":false}]}]})");
    ProjectDocument older;
    const bool      readOlder = pose::readProjectFile(path("older.json"), older, error) && older.figures.size() == 1 &&
                                older.figures[0].materials.size() == 1;
    check(readOlder && older.figures[0].materials[0].named.size() == 5 && older.figures[0].materials[0].names("roughness") &&
              !older.figures[0].materials[0].names("transmissionRoughness"),
          "the reader records the keys a zone's object names, and no others");
    if (!readOlder) {
        return;
    }
    const pose::ProjectMaterial& part = older.figures[0].materials[0];
    const pose::MaterialParams   merged = pose::materialWithNamedValues(imported, part.params, part.named);
    check(merged.roughness == 0.4f && merged.opacity == 0.5f && merged.baseColor == glm::vec3(0.1f, 0.2f, 0.3f) && !merged.thinWalled,
          "the values the file names are the file's");
    check(merged.transmissionRoughness == 0.0f && merged.ior == 1.38f && merged.smoothByAngle && merged.smoothAngle == 30.0f &&
              merged.uvTiling == glm::vec2(3.0f, 3.0f) && merged.coatColor == glm::vec3(0.5f, 0.6f, 0.7f) &&
              part.params.transmissionRoughness == -1.0f, // (… which is what the file's own values hold for it: the default)
          "the values it does not name stay the zone's own, not the struct's defaults (an eye shell's transmission roughness)");
}

/// A file edited by hand: a number no float holds rejects it whole (1e39 is a finite double
/// and an infinite float); a number outside the range its value can take is held to the range,
/// and one inside — a zero, a default, an importer's value past the tab's own row — is kept.
void checkValueRanges() {
    ProjectDocument d;
    std::string     error;
    const auto      rejected = [&](const char* name, const char* body) {
        writeText(path(name), std::string(R"({"format":"posestudio.project","version":1,)") + body + "}");
        return !pose::readProjectFile(path(name), d, error);
    };
    check(rejected("huge1.json", R"("figures":[{"source":"/x.duf","materials":[{"zone":"Torso","roughness":1e39}]}])") &&
              rejected("huge2.json", R"("figures":[{"source":"/x.duf","materials":[{"zone":"Torso","uvTiling":[1,1e39]}]}])") &&
              rejected("huge3.json", R"("figures":[{"source":"/x.duf","materials":[{"zone":"Torso","overlays":[{"opacity":-1e39}]}]}])"),
          "a material or overlay value past a float's range rejects the file");
    check(rejected("huge4.json", R"("environment":{"settings":{"exposure":1e39}})") &&
              rejected("huge5.json", R"("environment":{"settings":{"backdropMode":1e30}})") &&
              rejected("huge6.json", R"("camera":{"distance":1e39})") && rejected("huge7.json", R"("lights":[{"intensity":1e39}])"),
          "a lighting dial, a camera value or a light value past its type's range rejects the file");

    writeText(path("ranges.json"),
              R"({"format":"posestudio.project","version":1,"figures":[{"source":"/x.duf","materials":[{"zone":"Torso",
                  "ior":-1,"displacementStrength":1e30,"opacity":3,"metalness":-0.5,"thinFilmIor":40,"smoothAngle":720,
                  "detailTiles":0,"detailTilesV":0,"normalStrength":8,"bumpStrength":-2,"transmissionRoughness":-1,
                  "uvTiling":[-1,2],"emission":50,"specular":2.5,"baseColor":[-1,0.5,2],
                  "overlays":[{"offsetX":1e30,"offsetY":-0.25,"rotation":-1e30,"scale":0,"gamma":100,"bumpStrength":-9,"tint":[2,0.5,-1]}]}]}]})");
    d = ProjectDocument();
    const bool read = pose::readProjectFile(path("ranges.json"), d, error) && d.figures.size() == 1 && d.figures[0].materials.size() == 1 &&
                      d.figures[0].materials[0].params.overlays.size() == 1;
    check(read, "a file with values outside their ranges reads");
    if (read) {
        const pose::MaterialParams& p = d.figures[0].materials[0].params;
        check(p.ior == 1.0f && p.displacementStrength == 100.0f && p.opacity == 1.0f && p.metalness == 0.0f && p.thinFilmIor == 3.0f &&
                  p.smoothAngle == 180.0f && vec3Equal(p.baseColor, glm::vec3(0.0f, 0.5f, 2.0f)),
              "... each held to the range its value can take");
        check(p.detailTiles == 0.0f && p.detailTilesV == 0.0f && p.normalStrength == 8.0f && p.bumpStrength == -2.0f &&
                  p.transmissionRoughness == -1.0f && p.uvTiling == glm::vec2(-1.0f, 2.0f) && p.emission == 50.0f && p.specular == 2.5f,
              "... and what the tab's rows do not reach but a default or an importer holds is kept as it is");
        const pose::MaterialOverlay& o = p.overlays[0];
        check(o.offset == glm::vec2(1.0f, -0.25f) && o.rotation == -180.0f && o.scale == pose::kOverlayScaleMin && o.gamma == 5.0f &&
                  o.bumpStrength == -4.0f && vec3Equal(o.tint, glm::vec3(1.0f, 0.5f, 0.0f)),
              "... an overlay's controls to their fields' ranges");
    }
}

} // namespace

int main() {
    std::filesystem::create_directories(kDir);
    checkRoundTrip();
    checkRejections();
    checkForwardCompatibility();
    checkForeignNames();
    checkNamedValues();
    checkValueRanges();
    std::printf("\n%d failure(s)\n", failures);
    return failures;
}
