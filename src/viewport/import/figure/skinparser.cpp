/**
 * @file skinparser.cpp
 * @brief Implementation of parseSkinWeights. See skinparser.h.
 */

#include "skinparser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace pose {

namespace {
// The sparse weight list of a joint is stored either as {"count":N,"values":[...]} or a bare array.
const nlohmann::json& valuesArray(const nlohmann::json& node) {
    if (node.is_object()) {
        const auto it = node.find("values");
        if (it != node.end()) {
            return *it;
        }
    }
    return node;
}
} // namespace

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
                node = node.substr(1);
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
            if (!weightsNode) {
                continue;
            }
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
