// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file bodymesh.cpp
 * @brief The body mesh sample's text sidecar (see bodymesh.h).
 */

#include "bodymesh.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace pose {

bool saveBodyMesh(const std::string& path, const std::vector<BodyMeshPoint>& mesh) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << "# posestudio body mesh v1 " << mesh.size() << '\n';
    char line[96];
    for (const BodyMeshPoint& p : mesh) {
        std::snprintf(line, sizeof(line), "%.4f %.4f %.4f %d\n", static_cast<double>(p.pos.x),
                      static_cast<double>(p.pos.y), static_cast<double>(p.pos.z), p.bone);
        out << line;
    }
    return static_cast<bool>(out);
}

bool loadBodyMesh(const std::string& path, std::vector<BodyMeshPoint>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ss(line);
        BodyMeshPoint p;
        if (!(ss >> p.pos.x >> p.pos.y >> p.pos.z >> p.bone)) {
            out.clear();
            return false;
        }
        out.push_back(p);
    }
    return !out.empty();
}

} // namespace pose
