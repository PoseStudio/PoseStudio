/**
 * @file ikrig_constants.h
 * @brief The IkRig's tuning constants, shared by its translation units (ikrig.cpp,
 *        ikrigdrag.cpp, ikrigpolicy.cpp, ikrigstepping.cpp, ikrigcontacts.cpp). PRIVATE to the
 *        rig — not part of the scene/ik interface, so nothing outside those files may include it.
 *
 * Every value here was set by a measured failure in the IK harness (tools/ikharness) and the
 * comment at each definition records which; change one only with the harness re-run. The
 * constants sit in an anonymous namespace so each TU gets its own internal copy, and the
 * function-local trace probes (std::getenv) stay where they are used.
 */

#ifndef IKRIG_CONSTANTS_H
#define IKRIG_CONSTANTS_H

namespace pose {

namespace {

// Contacts within this GRAPH PATH LENGTH (summed bone rest lengths, world units) of the dragged
// effector are released rather than pinned: dragging a foot lifts the foot and its entire toe
// subtree (~0.15m away) instead of fighting its own pins, while a hip/pelvis drag keeps both
// feet planted (~1m away — the crouch). (A KNEE drag no longer frees the foot below it, half a
// metre away: see IkRig::liftedByDrag.)
// METRIC, not hop-count: hop rules break both ways on real rigs — the trunk packs tiny 2cm
// bones (a thigh is only 2 hops from the hip), while a foot's small-toe tip segments are 3-4
// hops from the ankle, and leaving THOSE pinned made a foot drag fight its own toe pins (the
// solver's compromise slid the foot sideways off the user's target).
constexpr float kEffectorLiftReach = 0.65f;

// A USER pin BELOW the dragged joint holds when it is at least this far down the limb from it (tree
// path, world units): a pinned hand under a dragged ELBOW, a pinned foot under a dragged knee —
// the pin is what the joint then swivels about. Nearer than this it sits the drag out, as every
// pin below the effector used to: a pinned toe under a dragged foot, a pinned finger under a
// dragged hand, fight the drag itself (dragging a pinned hand simply moves the pin).
constexpr float kEffectorPinHoldReach = 0.20f;
// ... or this share of the pinned joint's own LIMB (its junction with the trunk down to the pin,
// by bind lengths), whichever is LESS: 20cm is a length of ordinary proportions, and a stylized
// character's forearm is shorter than that - its pinned hand sat out every elbow drag, moving
// 24mm under a pin that holds to microns on every other figure (found by the character sweep).
// On ordinary proportions a hand under its elbow is 0.37 of the limb and a foot under its knee
// 0.42, a fingertip under its hand 0.20 and a toe under its foot 0.14: nothing that sat out at
// 20cm holds now, and everything that held still does.
constexpr float kEffectorPinHoldShare = 0.28f;
/// A grabbed joint within two bones and this tree-path length of the solve root is a PELVIS
/// drag (see the girdle promotion in IkRig::beginDrag). Across the eight generations: the pelvis
/// bone and the first spine bone 2-3cm, the second spine bone 10-12, the thigh sockets 15-16;
/// the third spine bone (18-25cm) is three bones up and leans on the two below it.
constexpr float kGirdleGrabReach = 0.20f;

// (A FOOT-CLASS joint — one that sits low AT BIND — is IkRig::kFootBindHeight / isFootClass.)
// A foot whose LOWEST joint stands this far over its own floor height, while another foot of
// the figure stands, was LIFTED by the user and is not planted at a drag's start (plantContacts).
// Over the residue the ground healing is for (millimetres), under any lift worth the name.
constexpr float kFootLiftedHeight = 0.03f;

// How far inside the support polygon the CoM must sit to count as balanced (world units).
constexpr float kBalanceMargin = 0.03f;

// A node participates as a DANGLING segment (suspension) only with REAL segment mass — the
// balance mass model classifies body parts; face/finger/helper bones get token mass and ride
// rigidly (gravity once pulled the hinged JAW open while the figure dangled). NOT used for
// contact eligibility: individual toe bones split the "toe" mass share far below any threshold,
// and dropping them shrank the support polygon enough to destabilize balance during arm drags.
constexpr float kRealMassThreshold = 0.003f;

// Token-limb promotion thresholds (see beginDrag). A grabbed joint promotes to its limb's first
// real-mass joint only while BOTH hold: its OWN segment mass is token (below kTokenBoneMass —
// fingers/carpals/face bones get the ~0.0015 unclassified mass and split toe shares run ~0.0004,
// while classified end joints like a hand at 0.006 or metatarsals at 0.005 do not qualify), and
// its SUBTREE mass is token too (below kTokenLimbMass — a wrist's subtree carries the whole hand
// ~0.03, so a mid-limb twist-bone grab stays where the user grabbed). The own-mass test is what
// keeps a FINGERLESS rig's leaf hand from promoting up the arm.
constexpr float kTokenBoneMass = 0.005f;
constexpr float kTokenLimbMass = 0.01f;

// Low-pass factor for the smoothed balance correction (see m_balanceCorrection in the header):
// the raw need flickers at tick scale as the CoM dances on the polygon's inset boundary, and
// the engagement blend must never see that flicker.
constexpr float kBalanceSmoothing = 0.25f;

// BALANCE-DRIVEN STEPPING (see updateStepping): when the post-solve balance need stays above
// kStepNeedThreshold for kStepConfirmTicks consecutive ticks — the drag holds the CoM off the
// support polygon harder than leaning can absorb — the figure re-plants a foot at its BALANCED
// position: the drag-start stance shape re-centered on the current CoM. Only standing feet
// step (a pin whose target sits at its bind height, i.e. ground-healed), only with at least
// two of them (stepping the single support would be a fall), and only for a re-plant of at
// least kStepMinDistance. The swing is an eased glide of the pin target with a sine lift arc,
// paced at ~kStepSpeed per tick; sequential steps under a continuing drag read as the figure
// WALKING to follow it.
// 0.025 (was 0.035): recalibrated when the old solver's feet were first held on the floor every
// tick. Before that a hard lateral lean sank the far foot through the floor, and that sink
// inflated the old PIN-STRAIN trigger enough to step; with the feet on the floor a 45cm lateral
// chest drag leaned the hip 19cm on planted feet without ever stepping — a person would have
// stepped long before. The balance-EFFORT path is the principled trigger for that lean (the
// pelvis correction fights the imposed lean every tick), and 0.025 restored the steps while
// every other phase stayed bit-identical. (The strain trigger itself is GONE with the old
// solver: the joint-space solve never drags a foot off its plant, so the signal was always 0.)
/// GROUND HEALING (beginDrag): a contact found within this much of its bind height over the floor
/// (figure-scaled) is pinned AT that height — a hovering figure is pulled back onto the floor;
/// one further off it (a kneeling knee, a deliberately airborne pose) keeps its height.
constexpr float kGroundHealBand = 0.12f;
constexpr float kStepNeedThreshold = 0.025f;
constexpr float kStepStanceThreshold = 0.12f; ///< Explicit-anchor drags: stance error that steps.
constexpr int   kStepConfirmTicks = 12;
/// A foot this many times kStepStanceThreshold from its anchored spot steps with no confirmation
/// at all (the ticks shrink linearly between): a fast drag's steps begin during it, not after.
constexpr float kStepPromptFactor = 2.5f;
constexpr float kStepMinDistance = 0.06f;
// Steps are a STANDING body's: no new step begins while the pelvis (or, under a pelvis drag,
// where the user is taking it) sits this far below its standing height. The older gate — 8cm
// below the DRAG-START pelvis — reads a crouch made within the drag, not one the drag began
// in: a figure dragged up out of a kneel by the hip (its hips a third of a metre ahead of its
// feet, the stance spot centred on them) stepped a foot forward under its own knees while
// still kneeling, and then back again as it stood.
constexpr float kStepLowBody = 0.12f;
constexpr float kShadowMaxRise = 0.10f; ///< A shadow support (a lifted contact kept in the polygon while the body would be unbalanced without it) EXPIRES once its joint has risen this far (m, figure-scaled): nothing leans on a knee 10cm in the air (see updateContacts).
// THE INTENTS (updateIntent; every one figure-scaled by sizeScale, like every geometric
// threshold of the rig — in raw metres until 2026-09-24, so a half-size character's gates were
// twice as coarse as a full-size figure's):
constexpr float kPelvisDownIntentDepth = 0.02f; ///< a pelvis drag this far under its start height is DOWN (while not on its way back up: kRiseAgain)
constexpr float kRiseAgain = 0.03f;             ///< ... and no longer down once the target has come this far back up from its lowest
constexpr float kLimbDownIntentDepth = 0.15f;   ///< a limb pushed this far below itself, TAUT toward the target, is pushing the body down
constexpr float kLimbUpIntentHeight = 0.10f;    ///< a limb pulled this far above itself is UP
constexpr float kCrouchIntentDepth = 0.08f;     ///< a pelvis drag this far below its start is a CROUCH: it keeps its feet (updateStepPolicy)
constexpr float kStepSpeed = 0.015f;
constexpr int   kStepMinTicks = 12;
constexpr int   kStepMaxTicks = 30;
constexpr float kStepHeightFactor = 0.25f;
constexpr float kStepHeightMin = 0.02f;
constexpr float kStepHeightMax = 0.08f;
constexpr float kStepClearance = 0.10f; ///< Min XZ distance between a landing and another pin.

// SUSPENSION (lift-off) entry: the pull must be mostly vertical (y > this fraction of the
// strain), farther beyond reach than kSuspendStrain, with the root hard against its leash, for
// kSuspendConfirmTicks consecutive ticks (~0.25 s) — a deliberate sustained lift, never a fast
// lateral gesture.
constexpr float kSuspendStrain = 0.15f;
// A LIMB's downward drag intent (the crouch drive, the root's vertical yield, the heel lift)
// needs the limb TAUT toward the target: junction-to-effector reach past this fraction of its
// rest span. A folded arm below its cursor has yet to unfold (see IkRig::limbDownIntent).
constexpr float kDownIntentTaut = 0.90f;
constexpr float kSuspendUpFraction = 0.65f;
constexpr int   kSuspendConfirmTicks = 15;
/// THE HIPS AND THE TRUNK LIFT OFF TOO (2026-11-07; the user: "when pulling a figure up by the hip
/// or torso or really anything, she tiptoes and tends to stay auto-pinned to the floor — too hard
/// to get the figure to leave the floor; pulling a bit past the pin limit should at some point
/// release the floor pin"): a drag of the hips or a trunk-role joint (IkRig::trunkEffector) lifts
/// off at this much strain beyond reach — not kSuspendStrain's 15cm, a limb's — and only with the
/// standing legs TAUT: each standing leg's STRETCH toward its pin (the socket's distance from the
/// pin over the thigh-and-shin chain, liveContactStretch's measure: 1 dead straight, 0.985 at a
/// knee bent 20 degrees, 0.94 at 40) at least kSuspendTautShare. A stand-up's root lags its
/// cursor by more than the strain for longer than the confirmation while the knees are still
/// bent — 26cm out of a sit, 16 off all fours — and must not lift off (measured as a share of the
/// bone PATH from the root, which runs sideways to the socket first, a knee bent 40 degrees read
/// taut and both stand-ups lifted off mid-rise, the released legs snapping back into the fold they
/// began in: a toe 67cm in a tick).
constexpr float kSuspendStrainTrunk = 0.08f;
constexpr float kSuspendTautShare = 0.97f;
/// A figure whose every foot stands this far over its floor height at a drag's start is IN THE
/// AIR — lifted off and let go of there, carried up with Ctrl, a pose with the feet up — and
/// nothing is planted: she stays where she floats (IkRig::plantContacts; the user, 2026-11-07:
/// "when the character is floating the foot auto-pinning is still active, probably should not
/// be"). Under it she hovers by residue and is healed onto the floor. The landing's airborne
/// height (ikrigcontacts.cpp) is the same number.
constexpr float kAirborneHeight = 0.05f;

} // namespace

} // namespace pose

#endif // IKRIG_CONSTANTS_H
