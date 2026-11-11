/**
 * @file figureutils.cpp
 * @brief Implementation of the shared figure-importer helpers. See figureutils.h.
 */

#include "figureutils.h"

#include "figuredocument.h" // isGzip / gunzip (figureFileKind)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <string_view>

namespace pose {

ChannelRef parseChannelRef(const std::string& ref) {
    ChannelRef out;
    std::string rest = ref;
    // Strip a leading "NodeName:" scope — but only when the colon precedes any '/', so a colon
    // inside a path or fragment ("/data/x.dsf#Some:Channel") isn't mistaken for a scope
    // separator.
    if (const std::size_t colon = rest.find(':'); colon != std::string::npos) {
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos || colon < slash) {
            out.scope = rest.substr(0, colon);
            rest = rest.substr(colon + 1);
        }
    }
    const std::size_t q = rest.find('?');
    out.fileUrl = (q == std::string::npos) ? rest : rest.substr(0, q);
    out.property = (q == std::string::npos) ? std::string() : rest.substr(q + 1);
    const std::size_t hash = out.fileUrl.find('#');
    out.rawKey = (hash == std::string::npos) ? out.fileUrl : out.fileUrl.substr(hash + 1);
    out.decodedKey = urlDecode(out.rawKey);
    return out;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hexVal(s[i + 1]);
            const int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string directoryOf(const std::string& path) {
    return std::filesystem::path(path).parent_path().string();
}

const nlohmann::json& valuesArray(const nlohmann::json& node) {
    if (node.is_object()) {
        const auto it = node.find("values");
        if (it != node.end()) {
            return *it;
        }
    }
    return node;
}

const nlohmann::json* childOf(const nlohmann::json* node, const char* key) {
    if (!node) {
        return nullptr;
    }
    const auto it = node->find(key);
    return it == node->end() ? nullptr : &(*it);
}

const nlohmann::json* findGeometryEntry(const nlohmann::json& doc, const std::string& fragment) {
    const auto it = doc.find("geometry_library");
    if (it == doc.end() || !it->is_array() || it->empty()) {
        return nullptr;
    }
    if (!fragment.empty()) {
        for (const auto& g : *it) {
            if (g.value("id", std::string()) == fragment) {
                return &g;
            }
        }
    }
    return &(*it)[0];
}

std::size_t geometryVertexCount(const nlohmann::json& doc) {
    const auto gl = doc.find("geometry_library");
    if (gl == doc.end() || !gl->is_array() || gl->empty()) {
        return 0;
    }
    const auto verts = gl->front().find("vertices");
    if (verts == gl->front().end()) {
        return 0;
    }
    if (const auto count = verts->find("count"); count != verts->end() && count->is_number()) {
        return count->get<std::size_t>();
    }
    const auto values = verts->find("values");
    return (values != verts->end() && values->is_array()) ? values->size() : 0;
}

namespace {

// True for a scene node that IS a figure of the file: it carries a geometry, by a reference that
// leads somewhere. A wearable saved to be ADDED to a figure lists that figure first as a
// placeholder — "name://@selection…", whatever is selected when it is loaded — which is no figure
// of the file: taken for one, such a preset could not be imported at all (its "base geometry" is
// the selection), and read as a character where its wearable has bones enough.
bool isFigureInstance(const nlohmann::json& node) {
    if (!node.is_object()) {
        return false;
    }
    const auto geos = node.find("geometries");
    if (geos == node.end() || !geos->is_array() || geos->empty() || !(*geos)[0].is_object()) {
        return false;
    }
    const std::string url = (*geos)[0].value("url", std::string());
    return !url.empty() && url.rfind("name:", 0) != 0;
}

} // namespace

const nlohmann::json* findFigureNode(const nlohmann::json& root) {
    const auto scene = root.find("scene");
    if (scene == root.end() || !scene->contains("nodes")) {
        return nullptr;
    }
    for (const auto& node : (*scene)["nodes"]) {
        if (isFigureInstance(node)) {
            return &node;
        }
    }
    return nullptr;
}

std::string findGeometryUri(const nlohmann::json& root) {
    const nlohmann::json* node = findFigureNode(root);
    return node ? (*node)["geometries"][0].value("url", std::string()) : std::string();
}

bool FigureScope::owns(const std::string& reference) const {
    if (!scoped || reference.empty()) {
        return true;
    }
    const std::size_t hash = reference.find('#');
    const std::string id = urlDecode(hash == std::string::npos ? reference : reference.substr(hash + 1));
    return id == nodeId || std::find(geometryIds.begin(), geometryIds.end(), id) != geometryIds.end();
}

bool FigureScope::namesAnotherFigure(const std::string& name) const {
    return scoped && !name.empty() &&
           std::find(otherFigures.begin(), otherFigures.end(), name) != otherFigures.end();
}

FigureScope figureScopeOf(const nlohmann::json& root, const nlohmann::json* figureNode) {
    FigureScope scope;
    if (!figureNode) {
        figureNode = findFigureNode(root);
    }
    if (!figureNode) {
        return scope;
    }
    const ChannelRef asset = parseChannelRef(figureNode->value("url", std::string()));
    scope.nodeId = figureNode->value("id", std::string());
    scope.assetId = asset.decodedKey;
    if (const std::size_t hash = asset.fileUrl.find('#'); hash != std::string::npos && hash > 0) {
        scope.assetFile = asset.fileUrl.substr(0, hash);
    }
    if (const auto geos = figureNode->find("geometries"); geos != figureNode->end() && geos->is_array()) {
        for (const auto& geo : *geos) {
            if (geo.is_object()) {
                scope.geometryIds.push_back(geo.value("id", std::string()));
            }
        }
    }
    // One figure in the scene: nothing to tell apart, and a preset that names its parents some
    // other way keeps importing as it always did.
    int figures = 0;
    if (const auto scene = root.find("scene"); scene != root.end() && scene->contains("nodes")) {
        for (const auto& node : (*scene)["nodes"]) {
            if (!isFigureInstance(node)) {
                continue;
            }
            ++figures;
            if (&node == figureNode) {
                continue;
            }
            // A second instance of the SAME figure shares the asset id (two characters on one
            // base): only what tells the two apart is recorded.
            for (const std::string& name : {node.value("id", std::string()),
                                            parseChannelRef(node.value("url", std::string())).decodedKey}) {
                if (!name.empty() && name != scope.nodeId && name != scope.assetId) {
                    scope.otherFigures.push_back(name);
                }
            }
        }
    }
    scope.scoped = figures > 1;
    return scope;
}

bool isFigureRoot(const nlohmann::json& root) {
    if (!root.is_object()) {
        return false;
    }
    if (const auto gl = root.find("geometry_library"); gl != root.end()) {
        return gl->is_array() && !gl->empty();
    }
    return findFigureNode(root) != nullptr;
}

FigureFileKind figureFileKind(std::vector<std::uint8_t> bytes) {
    try {
        if (isGzip(bytes)) {
            bytes = gunzip(bytes.data(), bytes.size());
        }
    } catch (const std::exception&) {
        return FigureFileKind::NotAFigure;
    }
    // A figure names its geometry: a base file its "geometry_library", a preset a scene node's
    // "geometries". A pose, material or shape preset names neither (a material preset's key is
    // the singular "geometry"), so most of a content folder never reaches the parse.
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.find("\"geometries\"") == std::string_view::npos &&
        text.find("\"geometry_library\"") == std::string_view::npos) {
        return FigureFileKind::NotAFigure;
    }
    const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (root.is_discarded() || !isFigureRoot(root)) {
        return FigureFileKind::NotAFigure;
    }
    // A wearable's figure node is fitted onto another figure ("conform_target": the figure it
    // was saved on, or the scene's selection) or hangs from one of its bones ("parent": a prop
    // attached to "name://@selection/hip:") — a character's figure node is neither.
    const nlohmann::json* node = findFigureNode(root);
    if (!node) {
        return FigureFileKind::Character; // a base figure file (its geometry_library)
    }
    if (node->contains("conform_target") || node->contains("parent")) {
        return FigureFileKind::Wearable;
    }
    // Its skeleton: the scene nodes the file parents within itself ("#hip" under "#Figure" …).
    // Every human figure generation lists a hundred and more; a prop, none or a hinge or two.
    constexpr std::size_t kCharacterMinBones = 20;
    std::size_t bones = 0;
    for (const auto& n : root.at("scene").at("nodes")) {
        const auto parent = n.find("parent");
        if (parent != n.end() && parent->is_string() && parent->get_ref<const std::string&>().rfind('#', 0) == 0) {
            ++bones;
        }
    }
    return bones >= kCharacterMinBones ? FigureFileKind::Character : FigureFileKind::Prop;
}

const nlohmann::json* findModifierById(const nlohmann::json& doc, const std::string& id) {
    const auto lib = doc.find("modifier_library");
    if (lib == doc.end() || !lib->is_array()) {
        return nullptr;
    }
    for (const auto& m : *lib) {
        if (m.value("id", std::string()) == id) {
            return &m;
        }
    }
    return nullptr;
}

const nlohmann::json* findMorphModifier(const nlohmann::json& doc, const std::string& fragment) {
    const auto lib = doc.find("modifier_library");
    if (lib == doc.end() || !lib->is_array()) {
        return nullptr;
    }
    // The modifier the fragment names, when the file defines it: its morph — or nothing, for a
    // control or a scale (which must not borrow the shape of whatever else the file holds). Only a
    // reference that names no modifier of this file falls back to the first one carrying a morph.
    if (!fragment.empty()) {
        for (const auto& m : *lib) {
            if (m.value("id", std::string()) == fragment) {
                return m.contains("morph") ? &m : nullptr;
            }
        }
    }
    for (const auto& m : *lib) {
        if (m.contains("morph")) {
            return &m;
        }
    }
    return nullptr;
}

float channelScalar(const nlohmann::json& channel, float fallback) {
    for (const char* key : {"current_value", "value"}) {
        const auto it = channel.find(key);
        if (it != channel.end() && it->is_number()) {
            return it->get<float>();
        }
    }
    return fallback;
}

double channelDouble(const nlohmann::json& channel, double fallback) {
    for (const char* key : {"current_value", "value"}) {
        const auto it = channel.find(key);
        if (it != channel.end() && it->is_number()) {
            return it->get<double>();
        }
    }
    return fallback;
}

bool channelBool(const nlohmann::json& channel, bool fallback) {
    for (const char* key : {"current_value", "value"}) {
        const auto it = channel.find(key);
        if (it != channel.end()) {
            if (it->is_boolean()) {
                return it->get<bool>();
            }
            if (it->is_number()) {
                return it->get<float>() != 0.0f;
            }
        }
    }
    return fallback;
}

bool channelColor(const nlohmann::json& channel, glm::vec3& out) {
    for (const char* key : {"current_value", "value"}) {
        const auto it = channel.find(key);
        if (it != channel.end() && it->is_array() && it->size() >= 3) {
            out = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
            return true;
        }
    }
    return false;
}

glm::vec3 restVec3Channels(const nlohmann::json& arr) {
    glm::vec3 out(0.0f);
    if (!arr.is_array()) {
        return out;
    }
    for (int i = 0; i < 3 && i < static_cast<int>(arr.size()); ++i) {
        const nlohmann::json& ch = arr[i];
        if (const auto v = ch.find("value"); v != ch.end() && v->is_number()) {
            out[i] = v->get<float>();
        } else if (const auto cv = ch.find("current_value"); cv != ch.end() && cv->is_number()) {
            out[i] = cv->get<float>();
        }
    }
    return out;
}

} // namespace pose
