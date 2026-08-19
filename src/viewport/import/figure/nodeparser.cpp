/**
 * @file nodeparser.cpp
 * @brief Implementation of parseSkeleton. See nodeparser.h.
 */

#include "nodeparser.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <utility>

namespace pose {

namespace {

// Reads an array of three channel_float objects ([{id:x,value:..},{id:y,..},{id:z,..}]) into a vec3,
// taking each channel's "value" (its rest value), falling back to "current_value". This is the
// deliberate INVERSE of the project-wide "current_value wins" convention (see channelScalar in
// materialparser.cpp): these are REST transforms — center/end points, rest orientation — where a
// scene's dialed current_value is the posed state, and the bind must come from the rest state.
glm::vec3 readVec3Channels(const nlohmann::json& arr) {
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

// Maps a rotation channel's id to an axis index (0/1/2), matching by id rather than array position
// (the node's rotation array is ordered per rotation_order, not necessarily x,y,z).
int rotationAxisOf(const std::string& id) {
    if (id == "x" || id == "XRotate") return 0;
    if (id == "y" || id == "YRotate") return 1;
    if (id == "z" || id == "ZRotate") return 2;
    return -1;
}

// Reads each rotation channel's [min,max] range and `clamped`/`locked` flags into the bone's
// per-axis limits. Two distinct constraints in the format:
//  - `locked` — the channel cannot be posed at all. This is how figures forbid anatomically
//    impossible motion: a mid-limb twist bone (which exists only to spread axial twist across the
//    skin) has its two bend axes locked so the limb can never fold where there is no joint — e.g.
//    the mid-forearm bone ("lWrist" in some generations, "l_forearmtwist1" in others) locks y/z
//    and leaves only the twist axis free. Honoured here as a hard [0,0] clamp on that axis.
//  - `clamped` — the channel moves within [min,max] (the joint's anatomical range of motion).
// Axes with neither flag stay unconstrained.
void readRotationLimits(const nlohmann::json& rotationArr, FigureBone& bone) {
    if (!rotationArr.is_array()) {
        return;
    }
    for (const auto& ch : rotationArr) {
        const int a = rotationAxisOf(ch.value("id", std::string()));
        if (a < 0) {
            continue;
        }
        if (ch.value("locked", false)) {
            bone.rotationMin[a] = 0.0f; // locked = pinned at rest (our pose Eulers are rest-relative)
            bone.rotationMax[a] = 0.0f;
            bone.rotationLimited[a] = true;
            continue;
        }
        const auto mn = ch.find("min");
        const auto mx = ch.find("max");
        const bool hasMin = mn != ch.end() && mn->is_number();
        const bool hasMax = mx != ch.end() && mx->is_number();
        if (hasMin) bone.rotationMin[a] = mn->get<float>();
        if (hasMax) bone.rotationMax[a] = mx->get<float>();
        bone.rotationLimited[a] =
            ch.value("clamped", false) && hasMin && hasMax && bone.rotationMax[a] >= bone.rotationMin[a];
    }
}

} // namespace

std::vector<FigureBone> parseSkeleton(const nlohmann::json& nodeLibrary) {
    std::vector<FigureBone> bones;
    if (!nodeLibrary.is_array()) {
        return bones;
    }

    std::vector<std::string> parentIds;
    std::unordered_map<std::string, int> idToIndex;

    for (const auto& node : nodeLibrary) {
        const std::string type = node.value("type", std::string());
        if (type != "figure" && type != "bone") {
            continue; // skeletal nodes only (skip cameras/lights/props)
        }
        FigureBone bone;
        bone.name = node.value("id", node.value("name", std::string()));
        if (const auto cp = node.find("center_point"); cp != node.end()) {
            bone.origin = readVec3Channels(*cp);
        }
        if (const auto orient = node.find("orientation"); orient != node.end()) {
            bone.orientation = readVec3Channels(*orient);
        }
        if (const auto rot = node.find("rotation"); rot != node.end()) {
            readRotationLimits(*rot, bone);
        }
        bone.rotationOrder = node.value("rotation_order", std::string("XYZ"));

        std::string parent = node.value("parent", std::string());
        if (!parent.empty() && parent[0] == '#') {
            parent = parent.substr(1);
        }

        idToIndex.emplace(bone.name, static_cast<int>(bones.size()));
        bones.push_back(std::move(bone));
        parentIds.push_back(std::move(parent));
    }

    for (std::size_t i = 0; i < bones.size(); ++i) {
        const auto it = idToIndex.find(parentIds[i]);
        bones[i].parent = (it != idToIndex.end()) ? it->second : -1;
    }

    return bones;
}

} // namespace pose
