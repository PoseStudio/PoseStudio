/**
 * @file materialfile.cpp
 * @brief The `.psm` material file's reader and writer. See materialfile.h.
 */

#include "materialfile.h"

#include <nlohmann/json.hpp>

namespace pose {

namespace {

using nlohmann::json;

constexpr const char* kFormatMarker = "posestudio.material";
constexpr int         kVersion = 1;

} // namespace

std::string materialFileText(const MaterialDocument& doc) {
    MaterialParams params = doc.params;
    params.overlays.clear(); // an overlay is an object's, not a surface's: never in a material file
    // … and so is its smoothing (the Materials tab's Smooth by Angle): how an object's faces are
    // shaded is the object's — a chrome applied from the library must not facet a sphere.
    json material = materialToJson(params, doc.textures);
    material.erase("smoothByAngle");
    material.erase("smoothAngle");
    json root = {
        {"format", kFormatMarker},
        {"version", kVersion},
        {"name", doc.name},
        {"material", std::move(material)},
    };
    return root.dump(2) + "\n";
}

bool readMaterialFileText(const std::string& text, MaterialDocument& doc, std::string& error) {
    doc = MaterialDocument{};
    const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        error = "The file is not a PoseStudio material (it is not valid JSON).";
        return false;
    }
    const auto format = root.find("format");
    if (format == root.end() || !format->is_string() || format->get<std::string>() != kFormatMarker) {
        error = "The file is not a PoseStudio material.";
        return false;
    }
    const auto version = root.find("version");
    if (version == root.end() || !version->is_number_integer() || version->get<int>() < 1) {
        error = "The material file has no version.";
        return false;
    }
    if (version->get<int>() > kVersion) {
        error = "The material was saved by a newer version of PoseStudio (file version " +
                std::to_string(version->get<int>()) + ").";
        return false;
    }
    const auto material = root.find("material");
    MaterialDocument read;
    if (material == root.end() || !materialFromJson(*material, read.params, read.textures)) {
        error = "The material file holds a value that cannot be read.";
        return false;
    }
    read.params.overlays.clear();
    read.params.smoothByAngle = MaterialParams().smoothByAngle; // (never a material file's to say: the zone keeps its own)
    read.params.smoothAngle = MaterialParams().smoothAngle;
    if (const auto name = root.find("name"); name != root.end() && name->is_string()) {
        read.name = name->get<std::string>();
    }
    doc = std::move(read);
    return true;
}

} // namespace pose
