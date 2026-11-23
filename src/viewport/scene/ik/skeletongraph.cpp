// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file skeletongraph.cpp
 * @brief The FBIK skeleton graph: the undirected adjacency built from the anatomical hierarchy,
 *        BFS re-rooting into the parent/traversal view the solver walks, and active-path marking.
 *
 * build() roots at the first parentless node only as a placeholder: the rig re-roots at the
 * PELVIS as soon as it is built, once (the contact-rooted design this class was written for was
 * abandoned — see the header). markActivePaths is what confines a solve to the effector and pin
 * chains; everything else rides along rigidly. Qt-free (std only).
 */
#include "skeletongraph.h"

namespace pose {

void SkeletonGraph::build(const std::vector<int>& parents) {
    const int n = static_cast<int>(parents.size());
    m_adjacency.assign(static_cast<std::size_t>(n), {});
    m_parent.assign(static_cast<std::size_t>(n), -1);
    m_order.clear();
    m_root = -1;

    int hierarchyRoot = -1;
    for (int i = 0; i < n; ++i) {
        const int p = parents[static_cast<std::size_t>(i)];
        if (p >= 0 && p < n) {
            m_adjacency[static_cast<std::size_t>(i)].push_back(p);
            m_adjacency[static_cast<std::size_t>(p)].push_back(i);
        } else if (hierarchyRoot < 0) {
            hierarchyRoot = i;
        }
    }
    if (hierarchyRoot >= 0) {
        // Placeholder rooting only: the rig re-roots at the pelvis as soon as it is built
        // (contact re-rooting was abandoned — see the header).
        setRoot(hierarchyRoot);
    }
}

bool SkeletonGraph::setRoot(int node) {
    const int n = nodeCount();
    if (node < 0 || node >= n) {
        return false;
    }
    m_root = node;
    m_parent.assign(static_cast<std::size_t>(n), -1);
    m_order.clear();
    m_order.reserve(static_cast<std::size_t>(n));

    std::vector<char> seen(static_cast<std::size_t>(n), 0);
    m_order.push_back(node);
    seen[static_cast<std::size_t>(node)] = 1;
    for (std::size_t head = 0; head < m_order.size(); ++head) {
        const int cur = m_order[head];
        for (const int nb : m_adjacency[static_cast<std::size_t>(cur)]) {
            if (!seen[static_cast<std::size_t>(nb)]) {
                seen[static_cast<std::size_t>(nb)] = 1;
                m_parent[static_cast<std::size_t>(nb)] = cur;
                m_order.push_back(nb);
            }
        }
    }
    return true;
}

std::vector<char> SkeletonGraph::markActivePaths(const std::vector<int>& targets) const {
    std::vector<char> active(static_cast<std::size_t>(nodeCount()), 0);
    if (m_root >= 0 && m_root < nodeCount()) {
        active[static_cast<std::size_t>(m_root)] = 1;
    }
    for (const int target : targets) {
        int cur = target;
        while (cur >= 0 && cur < nodeCount() && !active[static_cast<std::size_t>(cur)]) {
            active[static_cast<std::size_t>(cur)] = 1;
            cur = m_parent[static_cast<std::size_t>(cur)];
        }
    }
    return active;
}

} // namespace pose
