// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file runmode.h
 * @brief What kind of run this is: a person's, or one that a script or a probe drives.
 *
 * Several parts of the shell behave differently when nobody sits in front of the window — no
 * boot splash to click away, no What's New window over the frames a run reads, preferences that
 * stay the run's own. They all ask HERE which kind of run it is, so the rule is said once and a
 * new lever is added in one place. (The viewport's own tests of POSESTUDIO_IK_SCRIPT start and
 * drive the script; they are not about the kind of run.)
 */

#ifndef RUNMODE_H
#define RUNMODE_H

#include <QtGlobal>

namespace RunMode {

/// A SCRIPTED run — POSESTUDIO_IK_SCRIPT=<file>, the in-app test of tools/ikscripts: the script
/// drives the application through its own gestures and reads its frames back, usually on a
/// desktop someone else is working at.
inline bool scripted() {
    return qEnvironmentVariableIsSet("POSESTUDIO_IK_SCRIPT");
}

/// A PROBING run: a scripted one, or one of the other runs that drive the application
/// themselves and exit — the in-app bench (POSESTUDIO_IK_BENCH), a manual screenshot
/// (POSESTUDIO_MANUAL_SHOT), a skeleton dump (POSESTUDIO_DUMP_SKELETON). Nothing may come up
/// over the window in one, and nothing waits for an answer.
inline bool probing() {
    return scripted() || qEnvironmentVariableIsSet("POSESTUDIO_IK_BENCH") ||
           qEnvironmentVariableIsSet("POSESTUDIO_MANUAL_SHOT") || qEnvironmentVariableIsSet("POSESTUDIO_DUMP_SKELETON");
}

} // namespace RunMode

#endif // RUNMODE_H
