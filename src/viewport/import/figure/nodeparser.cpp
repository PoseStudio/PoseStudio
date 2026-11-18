/**
 * @file nodeparser.cpp
 * @brief Implementation of parseSkeleton. See nodeparser.h.
 */

#include "nodeparser.h"

#include "figureutils.h" // restVec3Channels — the REST-value reader (bind transforms, not dialed state)

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <utility>

namespace pose {

namespace {

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
        const int a = rotationAxisOf(stringOf(ch, "id"));
        if (a < 0) {
            continue;
        }
        if (boolOf(ch, "locked", false)) {
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
            boolOf(ch, "clamped", false) && hasMin && hasMax && bone.rotationMax[a] >= bone.rotationMin[a];
    }
}

// PARENTS FIRST is the engine's contract for a bone list (the armature walks it once, in order,
// and takes a bone whose parent has a HIGHER index for a root — its bind then set against a
// parent it does not follow). The format writes hierarchies in that order and every file seen
// does, in which case this returns without touching anything; a list that does not is re-ordered
// here — stably: each pass appends, in file order, the bones whose parent is already placed — and
// a parent cycle, which no pass can place, is cut at its first bone (that bone becomes a root).
void orderParentsFirst(std::vector<FigureBone>& bones) {
    const int count = static_cast<int>(bones.size());
    bool      ordered = true;
    for (int i = 0; i < count && ordered; ++i) {
        ordered = bones[static_cast<std::size_t>(i)].parent < i;
    }
    if (ordered) {
        return;
    }
    std::vector<int> newIndex(bones.size(), -1);
    std::vector<int> order;
    order.reserve(bones.size());
    while (order.size() < bones.size()) {
        bool placedAny = false;
        for (int i = 0; i < count; ++i) {
            const int parent = bones[static_cast<std::size_t>(i)].parent;
            if (newIndex[static_cast<std::size_t>(i)] < 0 &&
                (parent < 0 || parent >= count || newIndex[static_cast<std::size_t>(parent)] >= 0)) {
                newIndex[static_cast<std::size_t>(i)] = static_cast<int>(order.size());
                order.push_back(i);
                placedAny = true;
            }
        }
        if (!placedAny) {
            // A cycle: its first bone is cut loose; the next pass places it and what hangs from it.
            for (int i = 0; i < count; ++i) {
                if (newIndex[static_cast<std::size_t>(i)] < 0) {
                    bones[static_cast<std::size_t>(i)].parent = -1;
                    break;
                }
            }
        }
    }
    std::vector<FigureBone> sorted;
    sorted.reserve(bones.size());
    for (const int old : order) {
        FigureBone bone = std::move(bones[static_cast<std::size_t>(old)]);
        bone.parent = (bone.parent >= 0 && bone.parent < count)
                          ? newIndex[static_cast<std::size_t>(bone.parent)]
                          : -1;
        sorted.push_back(std::move(bone));
    }
    bones = std::move(sorted);
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
        const std::string type = stringOf(node, "type");
        if (type != "figure" && type != "bone") {
            continue; // skeletal nodes only (skip cameras/lights/props)
        }
        FigureBone bone;
        bone.name = stringOf(node, "id", stringOf(node, "name"));
        bone.nodeName = stringOf(node, "name", bone.name); // the cross-file identity (figuredata.h)
        if (bone.nodeName.empty()) {
            bone.nodeName = bone.name;
        }
        // Rest transforms read "value" first (see restVec3Channels): a scene's dialed
        // current_value is the posed state, and the bind must come from rest.
        if (const auto cp = node.find("center_point"); cp != node.end()) {
            bone.origin = restVec3Channels(*cp);
        }
        if (const auto orient = node.find("orientation"); orient != node.end()) {
            bone.orientation = restVec3Channels(*orient);
        }
        if (const auto rot = node.find("rotation"); rot != node.end()) {
            readRotationLimits(*rot, bone);
        }
        bone.rotationOrder = stringOf(node, "rotation_order", "XYZ");

        // A reference spells an id percent-encoded ("#Some%20Hair_1234"), the node it names plain:
        // compared as written, every bone under a figure node with a space in its id lost its parent.
        std::string parent = referencedId(stringOf(node, "parent"));

        idToIndex.emplace(bone.name, static_cast<int>(bones.size()));
        bones.push_back(std::move(bone));
        parentIds.push_back(std::move(parent));
    }

    for (std::size_t i = 0; i < bones.size(); ++i) {
        const auto it = idToIndex.find(parentIds[i]);
        bones[i].parent = (it != idToIndex.end()) ? it->second : -1;
    }

    orderParentsFirst(bones);
    return bones;
}

} // namespace pose
