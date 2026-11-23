// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file posefile.cpp
 * @brief The `.pose` file reader/writer. See posefile.h.
 */

#include "posefile.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace pose {

namespace {

constexpr const char* kBlanks = " \t\r";

/// @p token as one finite float, all of it (the classic locale's notation, whatever the
/// program's is). False for anything else: "12abc", "nan", a number past a float's range.
bool parseNumber(const std::string& token, float& out) {
    std::istringstream in(token);
    in.imbue(std::locale::classic());
    float value = 0.0f;
    if (!(in >> value) || in.peek() != std::istringstream::traits_type::eof() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

/// Whether a row named @p name can be read back: the format has no way to write an empty
/// name, one of blanks alone, or one that holds a line break.
bool writable(const std::string& name) {
    return name.find_first_not_of(kBlanks) != std::string::npos && name.find('\n') == std::string::npos;
}

} // namespace

bool writePoseFile(const std::string& path, const PoseRows& rows) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    // One line per row: "name x y z" — a posed joint's Euler degrees, or an @trans:/@pin: row
    // (see the header). Nine significant digits are what a float needs to come back as itself
    // (the stream's default six moved every angle of a saved pose by up to 5e-4 degrees).
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    for (const auto& [name, value] : rows) {
        if (!writable(name)) {
            continue; // (a row no reader could find again; written, it would garble the file)
        }
        out << name << ' ' << value.x << ' ' << value.y << ' ' << value.z << '\n';
    }
    return static_cast<bool>(out);
}

bool readPoseFile(const std::string& path, PoseRows& rows) {
    rows.clear();
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    // Read by LINE, into rows of its own that are handed over whole: a row is its line's last
    // three parts as numbers and everything before them as its name, and a line that is not
    // that — cut short, garbled — rejects the file. (Read as one stream of tokens, a last row
    // cut short simply ended the stream: the one damage a truncated file has went unseen.)
    PoseRows    read;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t last = line.find_last_not_of(kBlanks);
        if (last == std::string::npos) {
            continue; // a blank line
        }
        float       numbers[3] = {0.0f, 0.0f, 0.0f};
        std::size_t end = last + 1; // one past the part being read, from the line's end backwards
        for (int k = 2; k >= 0; --k) {
            const std::size_t gap = line.find_last_of(kBlanks, end - 1);
            if (gap == std::string::npos || !parseNumber(line.substr(gap + 1, end - gap - 1), numbers[k])) {
                return false; // fewer than a name and three numbers, or a part that is no number
            }
            const std::size_t before = line.find_last_not_of(kBlanks, gap);
            if (before == std::string::npos) {
                return false; // three numbers and no name
            }
            end = before + 1;
        }
        const std::size_t first = line.find_first_not_of(kBlanks);
        const std::string name = line.substr(first, end - first);
        glm::vec3         value(numbers[0], numbers[1], numbers[2]);
        if (name[0] != '@') {
            // A rotation row: wrap into [-360, 360] (fmod keeps the sign) — the per-axis limit
            // clamp downstream then sees a sane angle whatever the file said.
            value = glm::vec3(std::fmod(value.x, 360.0f), std::fmod(value.y, 360.0f),
                              std::fmod(value.z, 360.0f));
        }
        read.emplace_back(name, value);
    }
    if (in.bad()) {
        return false; // the file could not be read to its end
    }
    rows = std::move(read);
    return true;
}

} // namespace pose
