// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vulkanwindow_script_detail.h
 * @brief What the translation units of the scripted in-app test share: the log line, the bone
 *        lookup through the alias layer, and the step driver's vocabulary.
 *
 * Private to the runner's three files — vulkanwindow_script.cpp (the step driver),
 * vulkanwindow_script_commands.cpp (the command families) and vulkanwindow_script_metrics.cpp
 * (the metrics); see ikscript.h for the runner as a whole. Nothing else includes it.
 */

#ifndef VULKANWINDOW_SCRIPT_DETAIL_H
#define VULKANWINDOW_SCRIPT_DETAIL_H

#include "scene/armature.h"
#include "scene/ik/bonealiases.h"

#include <QStringList>

#include <glm/glm.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace pose {

/// What a command family tells the step driver (VulkanWindow::scriptStep) about a script line.
enum class ScriptFlow {
    NotMine, ///< None of this family's commands (or one with too few words): the next family is asked.
    Next,    ///< Done: the next line runs.
    Wait,    ///< The line waits for the app — a drag's ticks, a release's settle, a shot's frame:
             ///< scriptStep returns, and the event that ends the wait calls it again.
};

namespace scriptdetail {

/// One line of the run's log: stderr, flushed, so the runner sees it as it is written. The logs
/// of two runs are compared byte for byte — a line's text and its number formats are a baseline.
inline void say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

/// A script names bones the way the reference figure does; the other generations call the same
/// joints something else: the alias table the IK harness shares (scene/ik/bonealiases.h). The
/// name as given if the figure has it, else the first alternative it has, else the name.
inline std::string figureBoneName(const Armature* arm, const std::string& canonical) {
    return arm != nullptr ? aliasedBoneName(*arm, canonical) : canonical;
}
/// The index of the bone a script calls @p canonical on this figure (figureBoneName), or -1.
inline int scriptBone(const Armature* arm, const std::string& canonical) {
    return arm != nullptr ? arm->boneIndex(figureBoneName(arm, canonical)) : -1;
}
/// Every joint's world position (empty without a figure).
inline std::vector<glm::vec3> jointPositions(const Armature* arm) {
    std::vector<glm::vec3> out;
    if (arm != nullptr) {
        out.resize(arm->boneCount());
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = arm->boneWorldPosition(i);
        }
    }
    return out;
}

/// `num(i)`: word @p i of a script line as a number — 0 where the line has no such word.
struct WordNumber {
    const QStringList& w;
    float operator()(int i) const { return i < w.size() ? w[i].toFloat() : 0.0f; }
};

} // namespace scriptdetail

} // namespace pose

#endif // VULKANWINDOW_SCRIPT_DETAIL_H
