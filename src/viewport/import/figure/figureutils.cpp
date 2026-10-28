/**
 * @file figureutils.cpp
 * @brief Implementation of the shared figure-importer helpers. See figureutils.h.
 */

#include "figureutils.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>

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

const nlohmann::json* findFigureNode(const nlohmann::json& root) {
    const auto scene = root.find("scene");
    if (scene == root.end() || !scene->contains("nodes")) {
        return nullptr;
    }
    for (const auto& node : (*scene)["nodes"]) {
        const auto geos = node.find("geometries");
        if (geos != node.end() && geos->is_array() && !geos->empty() &&
            !(*geos)[0].value("url", std::string()).empty()) {
            return &node;
        }
    }
    return nullptr;
}

std::string findGeometryUri(const nlohmann::json& root) {
    const nlohmann::json* node = findFigureNode(root);
    return node ? (*node)["geometries"][0].value("url", std::string()) : std::string();
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
    // Prefer the modifier whose id matches the URI fragment; else the first one carrying a morph.
    if (!fragment.empty()) {
        for (const auto& m : *lib) {
            if (m.value("id", std::string()) == fragment && m.contains("morph")) {
                return &m;
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
