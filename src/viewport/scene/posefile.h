// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file posefile.h
 * @brief The plain-text `.pose` file: the pose snapshot's rows written out and read back.
 *
 * A pose file is the pose SNAPSHOT (Armature::capturePose) serialized one row per line — a
 * joint's Euler rotation as `boneName rx ry rz` (degrees), plus the snapshot's `@trans:<bone>`
 * rows (a bone's pose translation, written by full-body IK for the skeleton root) and
 * `@pin:<bone>` rows (the user's joint pins).
 *
 * A ROW IS ITS LINE'S LAST THREE PARTS, AS NUMBERS, AND EVERYTHING BEFORE THEM AS ITS NAME. So
 * a name may hold blanks — a rig imported from another naming convention keeps bones called
 * `Bip001 L UpperArm` — but no line break, and blanks at a name's two ends are not kept. Blank
 * lines are passed over. Readers ignore names they don't know, so an older build skips the
 * @-rows and an older file simply loads without them.
 *
 * This file owns the codec only — it never touches a figure — and is Qt-free (the standard
 * streams alone), so the Scene's save/load are two one-line forwarders and the format has
 * exactly one reader and one writer.
 */

#ifndef POSEFILE_H
#define POSEFILE_H

#include <glm/glm.hpp>

#include <string>
#include <utility>
#include <vector>

namespace pose {

/// The pose snapshot's rows: (bone name or `@trans:`/`@pin:` key, value).
using PoseRows = std::vector<std::pair<std::string, glm::vec3>>;

/// Writes @p rows to @p path. Returns false if the file can't be opened or written. A row whose
/// name the format cannot hold (empty, or with a line break in it) is left out.
bool writePoseFile(const std::string& path, const PoseRows& rows);

/// Reads @p path into @p rows. Returns false — leaving @p rows empty — if the file can't be
/// opened or is malformed: EVERY line that is not blank must be a name and three numbers (a
/// truncated or garbled row — the last one of a file cut short included — rejects the whole
/// file rather than applying the part before it), and no value may be non-finite. Rotation
/// values (rows not starting with '@') are wrapped into [-360, 360] so a hand-edited file can't
/// feed an absurd angle to the limit clamp; translation rows are sanitized by
/// Armature::applyPose itself.
bool readPoseFile(const std::string& path, PoseRows& rows);

} // namespace pose

#endif // POSEFILE_H
