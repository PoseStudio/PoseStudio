// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file skeletongraph.h
 * @brief The FBIK body graph: the figure's BODY joints (the figure-node chain above the pelvis
 *        excluded) as a graph rooted at the pelvis, whose one job today is marking the ACTIVE
 *        subgraph of a drag — the paths joining the effector and every pin to the root
 *        (markActivePaths): what the solve may move; everything else rides.
 *
 * setRoot(node) rebuilds the directed parent view and the BFS traversal order without
 * touching the engine's anatomical hierarchy. The rig always roots it at the PELVIS. (It is
 * re-rootable because the first, position-space solver once walked it from a ground contact;
 * the capability is kept, unused.)
 * Qt-free (std only + no GLM needed — pure topology).
 */

#ifndef SKELETONGRAPH_H
#define SKELETONGRAPH_H

#include <vector>

namespace pose {

/**
 * @class SkeletonGraph
 * @brief Undirected joint graph with a movable root: topology fixed, rooting per-solve.
 */
class SkeletonGraph {
public:
    /// Builds the undirected adjacency from the anatomical hierarchy (@p parents[i] = i's parent
    /// bone index, -1 for the skeleton root) and roots the graph at the hierarchy root initially.
    void build(const std::vector<int>& parents);

    /// Re-roots the graph at @p node: recomputes the per-node parent and the root-first
    /// traversal order via BFS over the undirected adjacency. Returns false if @p node is invalid.
    bool setRoot(int node);

    int root() const { return m_root; }
    int nodeCount() const { return static_cast<int>(m_adjacency.size()); }

    /// @p node's parent under the CURRENT rooting (-1 at the root). Not the anatomical parent.
    int parentOf(int node) const { return m_parent[static_cast<std::size_t>(node)]; }

    /// Root-first (BFS) traversal order under the current rooting.
    const std::vector<int>& traversalOrder() const { return m_order; }

    /// Marks the union of the current-root paths of @p targets: every node on any target's walk to
    /// the root, targets and root included. The solver only moves marked nodes — everything off
    /// those paths (fingers during an arm drag, the face, …) rides along rigidly.
    std::vector<char> markActivePaths(const std::vector<int>& targets) const;

private:
    std::vector<std::vector<int>> m_adjacency; // undirected: hierarchy edges, both directions
    std::vector<int>              m_parent;    // per node, under the current rooting
    std::vector<int>              m_order;     // BFS order from the current root
    int                           m_root = -1;
};

} // namespace pose

#endif // SKELETONGRAPH_H
