/**
 * @file asciilower.h
 * @brief ASCII lower-casing of a string (pure std): what a file extension or a file name is
 *        compared by.
 *
 * Only A-Z change; every other byte — the bytes of a UTF-8 sequence included — is left as it is,
 * whatever the process locale. Lives in viewport/util because it belongs to no layer: the
 * importer registry, the rigged readers' texture finder and the splat reader all need it, and
 * none of them should include another's header for it.
 */

#ifndef ASCIILOWER_H
#define ASCIILOWER_H

#include <string>

namespace pose {

/// @p s with its ASCII capitals lowered.
inline std::string asciiLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

} // namespace pose

#endif // ASCIILOWER_H
