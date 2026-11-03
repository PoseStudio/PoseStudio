/**
 * @file namerules.cpp
 * @brief The one reading of a bone name (see namerules.h).
 */

#include "namerules.h"

#include <cctype>
#include <cstring>

namespace pose {
namespace namerules {

namespace {

bool isSep(char c) {
    return c == '_' || c == '.' || c == '-' || c == ':' || c == ' ';
}
bool isUpper(char c) {
    return c >= 'A' && c <= 'Z';
}
bool isLowerOrDigit(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
bool isDigit(char c) {
    return c >= '0' && c <= '9';
}
char lowerChar(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// The side words, each with its mirror, in the cases the rigs seen write them.
const char* const kSideWords[][2] = {{"Left", "Right"}, {"Right", "Left"}, {"left", "right"},
                                     {"right", "left"}, {"LEFT", "RIGHT"}, {"RIGHT", "LEFT"}};

} // namespace

std::string lettersLower(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            out.push_back(lowerChar(c));
        }
    }
    return out;
}

std::string lower(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        out.push_back(lowerChar(c));
    }
    return out;
}

bool hasToken(const std::string& normalized, const char* token) {
    return normalized.find(token) != std::string::npos;
}

int digitByToken(const std::string& normalized, bool toe) {
    if (hasToken(normalized, toe ? "big" : "thumb")) return 1;
    if (hasToken(normalized, "index") || hasToken(normalized, "second")) return 2;
    if (hasToken(normalized, "mid") || hasToken(normalized, "third")) return 3;
    if (hasToken(normalized, "ring") || hasToken(normalized, "fourth")) return 4;
    if (hasToken(normalized, "pinky") || hasToken(normalized, "little") || hasToken(normalized, "fifth")) return 5;
    return 0;
}

bool saysTwist(const std::string& name) {
    const std::string l = lower(name);
    return l.find("twist") != std::string::npos || l.find("roll") != std::string::npos;
}

int segmentWord(const std::string& name) {
    const std::string s = lower(name);
    const auto        has = [&](const char* w) { return s.find(w) != std::string::npos; };
    if (has("thigh") || has("upleg") || has("upperleg") || has("up_leg") || has("upper_leg")) return 1;
    if (has("shin") || has("calf") || has("lowerleg") || has("lower_leg") || has("lowleg") || has("leg")) return 2;
    if (has("forearm") || has("fore_arm") || has("lowerarm") || has("lower_arm") || has("lowarm")) return 4;
    if (has("upperarm") || has("upper_arm") || has("uparm") || has("shldr") || has("arm")) return 3;
    return 0;
}

SideToken sideToken(const std::string& name) {
    const std::size_t n = name.size();
    // The prefix letter: `lShin`, `l_thigh`.
    if (n >= 2 && (name[0] == 'l' || name[0] == 'r')) {
        const char next = name[1];
        if (isUpper(next) || next == '_') {
            return {0, 1, name[0]};
        }
    }
    // The prefix word: `LeftFoot`, `leftfoot`.
    for (const auto& pair : kSideWords) {
        const std::size_t len = std::strlen(pair[0]);
        if (n > len && name.compare(0, len, pair[0]) == 0 && !isUpper(pair[0][1])) {
            return {0, len, lowerChar(pair[0][0])};
        }
    }
    // A lone letter token, bounded by separators or the ends.
    for (std::size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (c != 'L' && c != 'R' && c != 'l' && c != 'r') {
            continue;
        }
        const bool startOk = i == 0 || isSep(name[i - 1]);
        const bool endOk = i + 1 == n || isSep(name[i + 1]);
        if (startOk && endOk) {
            return {i, 1, lowerChar(c)};
        }
    }
    // A word token anywhere: bounded by a separator, the name's ends, a change of case
    // (`LeftArm`, `armLeft`), a digit after it, or an uppercase letter after an all-caps word.
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto& pair : kSideWords) {
            const std::size_t len = std::strlen(pair[0]);
            if (name.compare(i, len, pair[0]) != 0) {
                continue;
            }
            const std::size_t end = i + len;
            const bool        startOk = i == 0 || isSep(name[i - 1]) || (isLowerOrDigit(name[i - 1]) && isUpper(name[i]));
            const bool        endOk = end == n || isSep(name[end]) || isUpper(name[end]) || isDigit(name[end]) ||
                               (isLowerOrDigit(name[end]) && isUpper(pair[0][len - 1]));
            if (startOk && endOk) {
                return {i, len, lowerChar(pair[0][0])};
            }
        }
    }
    return {};
}

std::string mirroredName(const std::string& name) {
    const SideToken t = sideToken(name);
    if (t.side == 0) {
        return {};
    }
    if (t.len == 1) {
        std::string other = name;
        const char  c = name[t.pos];
        other[t.pos] = c == 'L' ? 'R' : c == 'R' ? 'L' : c == 'l' ? 'r' : 'l';
        return other;
    }
    for (const auto& pair : kSideWords) {
        if (name.compare(t.pos, t.len, pair[0]) == 0) {
            return name.substr(0, t.pos) + pair[1] + name.substr(t.pos + t.len);
        }
    }
    return {};
}

} // namespace namerules
} // namespace pose
