/**
 * @file skinparser.cpp
 * @brief Implementation of parseSkinWeights. See skinparser.h.
 */

#include "skinparser.h"

#include "figureutils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace pose {

const nlohmann::json* findSkinBinding(const nlohmann::json& modifierLibrary) {
    if (!modifierLibrary.is_array()) {
        return nullptr;
    }
    for (const auto& mod : modifierLibrary) {
        if (const auto skin = mod.find("skin"); skin != mod.end()) {
            return &(*skin);
        }
    }
    return nullptr;
}

std::vector<VertexSkin> parseSkinWeights(
    const nlohmann::json& skin, int vertexCount,
    const std::unordered_map<std::string, int>& boneNameToIndex) {
    const int count = std::max(vertexCount, 0);

    // Accumulate every (bone, weight) influence per vertex, then reduce to the strongest four.
    std::vector<std::vector<std::pair<int, float>>> perVertex(static_cast<std::size_t>(count));

    if (const auto joints = skin.find("joints"); joints != skin.end() && joints->is_array()) {
        for (const auto& joint : *joints) {
            std::string node = joint.value("node", std::string());
            if (!node.empty() && node[0] == '#') {
                node = urlDecode(node.substr(1)); // the reference is percent-encoded, the bone's id plain
            }
            const auto boneIt = boneNameToIndex.find(node);
            if (boneIt == boneNameToIndex.end()) {
                continue; // influence from a bone we don't have — skip
            }
            const int boneIndex = boneIt->second;

            // Local per-vertex weights (node_weights); some assets use vertex_weights.
            const nlohmann::json* weightsNode = nullptr;
            if (const auto nw = joint.find("node_weights"); nw != joint.end()) {
                weightsNode = &(*nw);
            } else if (const auto vw = joint.find("vertex_weights"); vw != joint.end()) {
                weightsNode = &(*vw);
            }
            if (weightsNode) {
                for (const auto& entry : valuesArray(*weightsNode)) {
                    if (!entry.is_array() || entry.size() < 2) {
                        continue;
                    }
                    const int v = entry[0].get<int>();
                    const float w = entry[1].get<float>();
                    if (v >= 0 && v < count && w > 0.0f) {
                        perVertex[static_cast<std::size_t>(v)].emplace_back(boneIndex, w);
                    }
                }
                continue;
            }
            // The OLDEST generations' skin binding (the format's earlier "TriAx" weighting)
            // carries no general map at all: per joint, three PER-AXIS rotation maps
            // (`local_weights.x/y/z`, each a vertex-weight list — identical on a finger,
            // different on a hip or shoulder, where each axis' bend blends a different region)
            // plus `scale_weights`/`bulge_weights` we do not model. Without them the two oldest
            // figure families imported with NO skin weights: they rendered at bind and never
            // deformed when posed. The general map is the mean of the three axis maps — a
            // vertex absent from a map counts 0 for it — which is also how a general-weighted
            // rig approximates such a binding.
            const auto local = joint.find("local_weights");
            if (local == joint.end() || !local->is_object()) {
                continue;
            }
            std::unordered_map<int, float> summed;
            int maps = 0;
            for (const char* axis : {"x", "y", "z"}) {
                const auto map = local->find(axis);
                if (map == local->end()) {
                    continue;
                }
                ++maps;
                for (const auto& entry : valuesArray(*map)) {
                    if (!entry.is_array() || entry.size() < 2) {
                        continue;
                    }
                    const int v = entry[0].get<int>();
                    const float w = entry[1].get<float>();
                    if (v >= 0 && v < count && w > 0.0f) {
                        summed[v] += w;
                    }
                }
            }
            if (maps == 0) {
                continue;
            }
            for (const auto& [v, w] : summed) {
                const float general = w / static_cast<float>(maps);
                if (general > 0.0f) {
                    perVertex[static_cast<std::size_t>(v)].emplace_back(boneIndex, general);
                }
            }
        }
    }

    std::vector<VertexSkin> out(static_cast<std::size_t>(count));
    for (std::size_t v = 0; v < perVertex.size(); ++v) {
        auto& influences = perVertex[v];
        std::sort(influences.begin(), influences.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        glm::ivec4 joints(0);
        glm::vec4 weights(0.0f);
        float sum = 0.0f;
        for (int k = 0; k < 4 && k < static_cast<int>(influences.size()); ++k) {
            joints[k] = influences[static_cast<std::size_t>(k)].first;
            weights[k] = influences[static_cast<std::size_t>(k)].second;
            sum += weights[k];
        }
        if (sum > 1e-8f) {
            weights /= sum; // renormalize after dropping the tail beyond the top 4
        }
        out[v].joints = joints;
        out[v].weights = weights;
    }

    return out;
}

} // namespace pose
