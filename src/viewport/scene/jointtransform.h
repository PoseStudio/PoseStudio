/**
 * @file jointtransform.h
 * @brief What the Transform tab shows for the selected joint: its name and its three dials.
 *
 * A plain-data contract between the scene and the properties panel (like LightingSettings): the
 * scene fills one from the active figure's selected joint (Scene::selectedJointTransform), the
 * viewport hands it out, the panel mirrors it into its rows. The dials themselves — which channel
 * each turns, its scale and its range — are the armature's (Armature::jointDial).
 */

#ifndef JOINTTRANSFORM_H
#define JOINTTRANSFORM_H

#include <string>

namespace pose {

struct JointTransform {
    /// One row: a dial on the 0-100 scale of its joint's limit (see JointDial). `enabled` is
    /// false for a motion the joint does not have (a locked channel).
    struct Dial {
        bool  enabled = false;
        float value = 0.0f;
        float minValue = 0.0f;
        float maxValue = 0.0f;
        bool operator==(const Dial& o) const {
            return enabled == o.enabled && value == o.value && minValue == o.minValue && maxValue == o.maxValue;
        }
    };

    /// THE FIGURE (2026-11-05; the user: "we have to be able to easily position the character (up
    /// down left right) and twist the entire character"): where the active figure stands and which
    /// way she faces — the rig root's pose translation in centimetres (world axes: x her left,
    /// y up, z the way an unturned figure faces) and the heading of its rotation in degrees (0 =
    /// facing +z as imported, positive = turned toward her left). Both are POSE: a pose file and a
    /// project carry them, Reset Pose takes them back, undo steps over them.
    struct Figure {
        bool  valid = false; ///< A posable figure is active (a joint need not be selected).
        float x = 0.0f, y = 0.0f, z = 0.0f; ///< cm
        float turnDeg = 0.0f;
        bool  operator==(const Figure& o) const { return valid == o.valid && x == o.x && y == o.y && z == o.z && turnDeg == o.turnDeg; }
    };

    bool        valid = false; ///< A joint is selected on the posable figure.
    std::string name;          ///< The joint's name (the bend bone's, for a selected twist bone).
    Dial        dials[3];      ///< Indexed by JointDialKind: BendForward, BendSideways, Twist (the tab labels them Bend, Side-Side, Twist).
    Figure      figure;        ///< The active figure's place and heading, valid with any posable figure.

    bool operator==(const JointTransform& o) const {
        return valid == o.valid && name == o.name && dials[0] == o.dials[0] && dials[1] == o.dials[1] &&
               dials[2] == o.dials[2] && figure == o.figure;
    }
    bool operator!=(const JointTransform& o) const { return !(*this == o); }
};

} // namespace pose

#endif // JOINTTRANSFORM_H
