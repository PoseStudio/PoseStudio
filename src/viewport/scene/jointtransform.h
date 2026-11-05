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
    /// y up, z the way an unturned figure faces) and its rotation as three angles in degrees: a
    /// TURN about the vertical (0 = facing +z as imported, positive = toward her left), then a TIP
    /// about her own lateral axis (positive = forward, face down), then a TILT about her own
    /// forward axis (positive = toward her left). All of it is POSE: a pose file and a project
    /// carry it, Reset Pose takes it back, undo steps over it.
    /// A STATIC model selected instead (an OBJ, a Gaussian splat scene) shows the same rows off
    /// its MODEL TRANSFORM — plus Scale, in percent of the size it was imported at — with
    /// isStatic set; those edits are not pose edits (no undo entry, not a pose file's), though a
    /// project carries the transform.
    struct Figure {
        bool  valid = false; ///< A posable figure is active (a joint need not be selected), or a static model is selected.
        float x = 0.0f, y = 0.0f, z = 0.0f; ///< cm
        float turnDeg = 0.0f, tipDeg = 0.0f, tiltDeg = 0.0f;
        float scalePct = 100.0f; ///< A static model's uniform scale (100 = as imported); a figure's reads 100 and is not editable.
        bool  isStatic = false;  ///< The rows read and set a static model's transform.
        bool  operator==(const Figure& o) const {
            return valid == o.valid && x == o.x && y == o.y && z == o.z && turnDeg == o.turnDeg && tipDeg == o.tipDeg &&
                   tiltDeg == o.tiltDeg && scalePct == o.scalePct && isStatic == o.isStatic;
        }
    };

    bool        valid = false; ///< A joint is selected on the posable figure.
    std::string name;          ///< The joint's name (the bend bone's, for a selected twist bone).
    Dial        dials[3];      ///< Indexed by JointDialKind: BendForward, BendSideways, Twist (the tab labels them Bend, Side-Side, Twist).
    Figure      figure;        ///< The active figure's place and orientation, valid with any posable figure.

    bool operator==(const JointTransform& o) const {
        return valid == o.valid && name == o.name && dials[0] == o.dials[0] && dials[1] == o.dials[1] &&
               dials[2] == o.dials[2] && figure == o.figure;
    }
    bool operator!=(const JointTransform& o) const { return !(*this == o); }
};

} // namespace pose

#endif // JOINTTRANSFORM_H
