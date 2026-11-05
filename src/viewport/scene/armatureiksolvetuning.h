/**
 * @file armatureiksolvetuning.h
 * @brief Every tuning constant of the Armature's per-tick IK solve (armatureiksolve*.cpp), with
 *        the reasoning each was set by, and the env probes (A/B levers) more than one TU reads.
 *
 * The numbers are the model: weights per m^2 / rad^2, stiffness classes, the shares and ranges
 * every gesture is gated by. Each is annotated with what it was measured against (the IK harness
 * on the eight reference rigs, the character sweep, the in-app galleries) — change one, and the
 * comment says which phases will tell you what it cost. Private to the solve: only
 * armatureiksolve*.cpp, armatureikpost.cpp, armatureikdigit.cpp and armaturelanding.cpp include it.
 * Qt-free (std + GLM).
 */

#ifndef ARMATURE_IK_SOLVE_TUNING_H
#define ARMATURE_IK_SOLVE_TUNING_H

#include "armature.h"

#include <cstdlib>

namespace pose {

/// The cursor row: per m^2 — hard while the cursor is REACHABLE (met to a few hundredths of a
/// millimetre) — with a bounded PULL: past a miss of pull / (2 x weight) the row's cost grows
/// linearly, so its force on the body stops at kEffectorPull however far the cursor goes. That
/// force is the arbiter of how far the body follows a cursor it cannot reach: a channel gives
/// until its posture gradient (2 x stiffness x deflection / lever) matches it — the supple
/// dragged limb entirely, each stiff trunk joint some ten degrees, the pelvis under a
/// centimetre, the centre of mass a centimetre past its support — and no further: the hand
/// lets the cursor go instead of folding the figure over after it (at 1e4 an out-of-reach
/// forward pull leaned the body to its joint limits and stepped it; a head pulled up 15cm
/// walked).
constexpr double kEffectorWeight = 1.0e6;
constexpr double kEffectorPull = 200.0;
constexpr double kRootDragPullScale = 5.0;
/// The suspended root's hang below the cursor (see the SUSPENDED note in solveIk).
constexpr double kSuspendHangWeight = 1.0e4;
/// The solve root's ROTATION under a pelvis drag, per rad^2 (see rootTurns). 80, from 20: at 20
/// a kneel pitched the whole torso 56 degrees forward and splayed the thighs 29 (rotating the
/// pelvis was cheaper than anything the legs could do), the head at 0.81m; at 80 it pitches 14,
/// the thighs stay in line, the head at 1.07m, and the sit, the all-fours and the deep crouch
/// keep what they need of it. (80 had been tried and rejected before — while the foot's
/// orientation rows were mis-linearized at a large pitch (jointsolver.cpp: lateralAxisError) and
/// every kneel crawled through a different path; that verdict was the solver's, not the number's.)
constexpr double kRootRotationStiffness = 80.0;
/// Pins are harder still: the saturated pull moves a pin by pull / (2 x pinWeight), a quarter
/// of a micron.
/// A user pin outranks a contact sharing its chain.
constexpr double kPinWeight = 1.0e8;
constexpr double kUserPinScale = 4.0;
/// A pin's held orientation, in metres per radian against its position rows: PLACE wins a
/// limit-forced compromise (1 degree weighs like 3.5mm).
constexpr double kPinOrientScale = 0.2;
/// Balance: the centre of mass kept over the support. Its pull grows with the distance outside
/// the (inset) polygon and meets the cursor's bounded pull at a centimetre (pull / (2 x weight)):
/// an out-of-reach hand pull leans the body to the edge of its support and no further (at 2e3
/// the lean ran 5cm past it and the balance stepper answered a plain forward hand pull with a
/// step). Steps are for where the user takes the BODY — a pelvis or trunk drag's own trigger.
constexpr double kBalanceWeight = 1.0e4;
/// The balance requirement ramps in at this pace after the support changes (see
/// m_jsBalanceSlack).
constexpr float kBalanceRampStep = 0.01f;
/// ... and a support that GROWS is grown into at this pace, metres a tick (see m_jsGrowFrom in
/// solveIk): the slack's own, so a landing gives a lean back no faster than a lift-off asks for one.
constexpr float kSupportGrowStep = 0.01f;
/// The posture stiffness classes (see the file note) and the range normalization's bounds: a
/// channel narrower than kMinRangeDeg is priced as that (a +-5 degree knee play is not 1000x a
/// shoulder), an unlimited one as kFreeRangeDeg.
constexpr double kLimbStiffness = 1.0;
constexpr double kPinLimbStiffness = 0.25;
constexpr double kTrunkStiffness = 40.0;
constexpr double kPinLimbTwistScale = 256.0;
constexpr double kRootStiffness = 4500.0; // per m^2: a hand pull barely moves the pelvis (under 1cm on every rig)
/// The pelvis gives VERTICALLY far more readily than sideways: a push down on the chest or a
/// hand folds the knees (a crouch), where a sideways pull must not slide the hips.
constexpr double kRootStiffnessVertical = 1000.0;
/// ... and gives WAY under a push DOWN. The vertical price above meets the cursor's bounded pull
/// at pull / (2 x 1000) = 10cm, so however far the chest (or a hand) was pushed down, the hips
/// sank 10cm and stopped — "she crouches a bit and gets stuck". It cannot simply be cheap (at
/// 300 a gentle SIDEWAYS pull sank the hips 2cm to lengthen its reach), so it yields with the
/// GESTURE: as the target goes below where the drag began (from 5cm, fully by 20cm,
/// figure-scaled) and mostly vertically (its downward share of the whole displacement from 60%,
/// fully by 90%), the price blends geometrically down to this — a smooth function of the
/// target alone, so the pose stays a function of the target (no intent flipping on and off
/// under a held cursor) and bringing the cursor back up stands her back up.
///
/// The same going UP, for a drag that BEGINS crouched: let go of in a crouch, the crouch is the
/// next drag's posture reference, and pulling the chest (or the head, or a hand past the arm's
/// reach) back up met the same cap from below — the hips rose 10cm and stopped, the spine
/// stretching after the cursor ("pulling back up does not cause her to stand back up"). So the
/// price yields upward too, by the same travel and share, scaled by the HEIGHT THE LEGS HAD
/// LEFT when the drag began (m_jsRootRiseRoom: nothing under 2cm, fully from 8cm,
/// figure-scaled). A drag that begins standing has none, so its upward pulls are untouched —
/// an arm raise must not buy tiptoe with a cheap pelvis — and the room is a constant of the
/// drag, so the pose is still a function of the target.
///
/// RISING also yields the pelvis's HORIZONTAL price (m_jsRootRise), by the same factor: getting
/// up out of a KNEEL (the hips a third of a metre ahead of the feet) or a SIT (half a metre
/// behind them) carries the hips back over the feet, and the horizontal price meets the pull
/// at 200 / (2 x 4500) = 2cm — pulled up by the chest or the head, a kneeling figure did not
/// rise at all. Balance (its own row) still says how far the hips may go.
constexpr double kRootYieldStiffness = 30.0;
/// REACHING DOWN (ikTrunkPolicy): under a HAND pushed down the hips' vertical yield is this share of
/// a chest push's (the exponent kRootYieldStiffness is reached by), so the trunk's hinge can compete
/// with the squat — at 0.5 the vertical price bottoms at 173/m^2 instead of 30: a hand to the knee is
/// a bow over softened knees, a hand to the floor a fold at the hips AND a bend of the knees.
constexpr double kReachSquatShare = 0.5;
/// ... and the pelvis's coupling "range" under a hand's reach down (degrees; the bow's is
/// kTrunkHingeRangeDeg): the spine follows the pelvis's pitch at the same share of its range, so a
/// larger range here is a fold that lives more at the hips and less in a curling spine.
constexpr double kReachHingeRangeDeg = 220.0;
/// ... the pelvis's pitch price under a hand's reach down (per rad^2, over the reach's share): firm
/// enough that its folding REFERENCE decides the fold, soft enough that the cursor's bounded pull can
/// bend it 20 degrees; and the reference's fold — degrees a metre of the hand's downward travel,
/// capped. A hand brought to the knee (30cm) folds ~30 degrees over softened knees; to the floor,
/// the cap, over bent knees.
constexpr double kReachHingeStiffness = 300.0;
constexpr double kReachFoldPerMetre = 125.0;
/// THE KNEES BEND ALIKE under a hand's reach down (ikSpineCoupling): a soft coupling between the two
/// standing legs' fold channels, per rad^2 of the difference in their range shares, by the reach's
/// share. A deep one-handed reach leaves one knee straight and the other bent 20-30 degrees. OFF
/// (0): tried at 30 on 2026-09-25 — a straight knee is a fold BOUND, so the coupling could only pull
/// the bent knee straight to match it, and a character rig's floor reach then missed by 24mm with
/// both legs straight under a fold at its cap. IK_JS_REACHKNEECOUPLING=30 is the A/B.
constexpr double kReachKneeCoupling = 0.0;
constexpr double kReachFoldMaxDeg = 55.0;
/// The pelvis's price, every way, while a dragged FOOT is on the floor (the SLIDE, see solveIk):
/// per m^2. A stance cannot widen or lengthen without the hips coming between the feet and down —
/// that is geometry, not preference (a hand pull is the opposite case: there the pelvis must
/// stay put, which is what the prices above are for). At those prices a foot dragged 55cm forward
/// stopped 4cm short, 40cm sideways 7cm short, the hips having followed 2cm: a single-leg squat
/// over the standing foot instead of a stride. (What takes the hips between the feet is their
/// REFERENCE, which goes with the stance's centre; the price only says how far they may stray
/// from it. At 60 they strayed: crossing one leg in front of the other, the oldest generations
/// resolved the legs' volumes by carrying the hips 17cm forward. 250 halves that, and a stride,
/// a back step and a side step are the same to the millimetre.)
constexpr double kLegDragRootStiffness = 250.0;
/// THE WEIGHT SHIFT (ikPostureModel): how much of the way from the centre of mass, as a drag found
/// it, to the standing foot's footprint centre the hips' HOME moves as the other foot lifts. 1 puts
/// the weight over the middle of the standing foot — a person on one leg stands over it, the
/// standing leg sloping in from the foot to a pelvis that has come across it; a smaller share would
/// leave the rest to the trunk (balance, blind to what a drag began with, does not ask for it).
/// The home moves at the pins' pace (kPinEaseStep) toward lift x share x the shift.
constexpr double kWeightShiftShare = 1.0;
/// CONTRAPPOSTO (ikPostureModel): standing on one leg under the weight shift, the pelvis ROLLS the
/// free side down by this many degrees at the full lift (the standing hip hikes), and the chest is
/// kept level by the sway's row — the spine curving back between them. A relaxed one-leg stance
/// drops the unloaded hip 5-8 degrees; a dead-level pelvis over one foot reads as a mannequin.
constexpr double kContrappostoDeg = 8.0;
/// ... and the share the roll's price is divided by while nothing is lifted (the pelvis frozen at
/// kRootRotationStiffness x kRootSquareness / this): the roll is an unknown from the drag's first tick.
constexpr float  kContrappostoFloor = 0.02f;
/// THE STRIDE TURNS THE HIPS (ikPosturePolicy, 2026-11-02): a foot slid AHEAD of the other along the
/// floor turns the pelvis toward the lead leg — the lead hip forward, as a walker's is — by a REFERENCE
/// on the root's heading channel (the contrapposto's idiom: built on the drag-start rotation, the
/// change in the stance's fore-aft split since the press, scaled by the slide so a lifted foot turns
/// nothing), while the CHEST keeps its heading (the sway row's heading axis) and the spine's twist
/// channels take the turn up: the shoulders stay square to the way she faces. A walker's pelvis
/// rotates 4-5 degrees each way at a normal step and more at a long one; a static stride with the
/// pelvis square reads as a mannequin's.
constexpr double kStrideYawPerMetre = 0.35;        ///< radians of pelvis yaw per metre of split beyond the dead zone (a 50cm stride: ~7 degrees)
constexpr double kStrideYawMaxDeg = 10.0;          ///< ... saturating toward this (tanh)
constexpr float  kStrideYawDeadZone = 0.04f;       ///< of split (figure-scaled), the arm swing's: a hair of split turns nothing
constexpr double kStrideYawStiffness = 1000.0;     ///< the heading channel's price over the slide's share: firm enough that the reference decides the turn (the foot's bounded pull could buy 6 degrees more at full stretch), frozen at kContrappostoFloor while the foot is in the air
/// (The chest's heading is held by REFERENCE, not by a row: each spine link's twist channel counters the yaw by its share
/// of the chain's twist range. A 1000/rad^2 heading row on the chest tipped the male rig's standing knee off its
/// saddle in a step back — the solver's fold retry found a 16-degree fold cheaper under it, a 67mm pop — where the
/// references, which the coupling rows already expect, hold the shoulders square to a quarter of a degree.)
/// A sliding foot's SOLE: pitch, heading, level (per rad^2, x the slide). Soft next to the
/// cursor's pull (200), two decades over the foot's own posture term — the sole stays on the floor
/// as the leg swings out under it, and gives way before the cursor does.
constexpr double kSlideSolePitchWeight = 20.0;
/// ... and the sole's flat target is approached at this many degrees a tick from the rotation the
/// slide found the foot at (ikCursorRows): a foot set down from a tilted carry rolls flat over its
/// descent rather than flipping in two ticks. The landing hand's palm turns at kPalmTurnStepDeg.
constexpr float  kSoleTurnStepDeg = 5.0f;
constexpr double kSlideSoleHeadingWeight = 200.0;
constexpr double kSlideSoleRollWeight = 20.0;
/// A KNEE dragged over its planted foot (see the knee drag in solveIk): the foot lets go as the
/// knee's target is pulled UP past where the drag began — from kKneeLiftFrom, gone by
/// kKneeLiftFull (m, figure-scaled) — its rows falling by kKneeLiftDecades on the way (a pin is
/// 1e8: a linear fade does nothing, see the live contacts'); and a leg that hangs from a dragged
/// knee hangs its ANKLE under the knee (per m^2, in the floor's plane, offset as the drag found
/// it: two decades over the knee's own posture term, nothing beside the cursor's pull), so a knee
/// raise bends the knee and the shin hangs, where riding the thigh rigidly it kicked forward like
/// a goose step. (A POSITION row, not the shin's world orientation held: a trailing shin is 30-40
/// degrees off that, where a uniform orientation row's small-error Jacobian is a third wrong —
/// IK_JS_LM_TRACE showed the wall of REJECTs, three ticks of the peel did not converge, and the
/// toes jumped 10cm on the tick that did.)
constexpr float  kKneeLiftFrom = 0.01f;
constexpr float  kKneeLiftFull = 0.08f;
constexpr double kKneeLiftDecades = 8.0;
/// ... all but the ankle's PLACE on the floor's plane, which goes in two stages: hard to a soft
/// spring (1e8 to 1e2: softer than the pelvis's own price, or the hips share the stretch) with the rest, then the spring itself over the rest of the raise (to 1 by
/// kKneeTrailFull of lift, then gone). A lifted foot TRAILS: it comes up off its spot and swings
/// under the knee as the thigh comes up — let go of all at once it had 25cm to travel (8cm of
/// lift is 35 degrees of thigh, and a hanging shin puts the foot under the knee) while the
/// cursor moved two: a whip, 8cm a tick at the toes.
constexpr float  kKneeTrailFull = 0.35f;
/// ... and a letting-go foot's PLACE spring (its floor spot, still strong at 20cm of lift) swings
/// toward the hang's target over this much lift (m, figure-scaled), by as much as the shin the
/// drag found was NOT vertical (see the knee-drag foot's rows in ikPinRowsEnd). WITH the pin's
/// release (kKneeLiftFull), no faster and no slower: at 3cm the merged row's target was under the
/// knee while its weight was still a planted pin's (161mm yanks); at 10-15cm the foot had followed
/// its found spot UP behind the rising knee first, folded to the knee's limit, and sat in that
/// basin until it flipped 25-34cm in a tick (three rigs and two custom characters).
constexpr float  kKneeUnderLift = 0.08f;
constexpr float  kKneeUnderReachFrom = 0.25f; ///< The hang asks the ankle UNDER the knee from this share of the shin's length of reach ...
constexpr float  kKneeUnderReachTo = 0.90f;   ///< ... and the floor's reach point beyond this (blended between).
constexpr float  kKneeGoalRaiseShare = 0.30f; ///< The knee's goal rises to where a vertical shin stands on the floor, by at most this share of the shin (ramped by the reach).
constexpr float  kTwistAlongFrom = 0.8f; ///< A channel whose axis lies this far along its bone (cosine) begins to carry the twist price ...
constexpr float  kTwistAlongFull = 0.95f; ///< ... and carries it in full from this.
constexpr double kKneeTrailLevelShare = 0.05;
constexpr float  kKneelSeatUprightDeg = 30.0f; ///< A kneel-up's hips are home at the seat height once the trunk's target stands within this of straight up about it ...
constexpr float  kKneelSeatLevelDeg = 80.0f;   ///< ... and where they were found while it lies within this of level (blended between).  ///< The share of the foot's orientation holds kept once its trail is complete (the toes' 4e4 -> 2000).
/// ... pulling this much harder than the standing hang for a shin found lying flat (a kneel's).
constexpr float  kKneeSwingHangScale = 3.0f;
constexpr float  kKneeLiftEase = 0.15f; ///< a knee coming off a planted foot: its height eases in over this much lift
constexpr double kKneePlaceSpringDecades = 6.0;
constexpr double kKneeAnkleHangWeight = 100.0;
/// Under a PELVIS drag the body still BALANCES — with its trunk: the hips are where the cursor
/// puts them, so what takes the centre of mass back over the feet is the spine and the pelvis's
/// own pitch (a hip HINGE: hips back, chest forward). It was simply off there, and a figure whose
/// hips were taken 10cm back stood bolt upright with her head 7cm behind her heels; a half squat
/// sat back the same way. It comes in with the pelvis's HEIGHT (the cursor's, as a share of the
/// standing height: none under kPelvisBalanceFrom, in full from kPelvisBalanceFull): it is the
/// standing body's — a hinge, a half squat. Lower than that the hips are on their way to the
/// floor, where they are sat on, not balanced over (at 0.30-0.50 one generation's kneel still
/// caught it on the way down and jumped 3cm in its still hold).
constexpr float kPelvisBalanceFrom = 0.70f;
constexpr float kPelvisBalanceFull = 0.85f;
constexpr float kPelvisBalanceDownFrom = 0.55f;
constexpr float kPelvisBalanceDownGone = 0.80f;
constexpr float kTrunkFollowDownFrom = 0.30f; ///< The trunk follow stands down for a drag whose downward share of travel is over this ...
constexpr float kTrunkFollowDownGone = 0.55f; ///< ... and is gone from this (a bow is a fold, not a walk).
constexpr float kUnfoldFromDeg = 10.0f; ///< A trunk found tilted more than this that comes upright brings its hips under its target ...
constexpr float kUnfoldFullDeg = 25.0f; ///< ... in full from this much of found tilt.
/// A TRUNK joint dragged ACROSS the floor (the chest, the neck, the head) takes the BODY with it:
/// the first of the travel is a LEAN (the spine's), and past it the hips FOLLOW — their
/// REFERENCE moves, which is the only way a body part goes anywhere (a price can only prefer
/// less travel). The lean's share is sized by the grab's height over the hips, its lever: none
/// of the travel follows under kTrunkLeanFree of it, and past that the lean saturates at
/// kTrunkLeanWidth more (travel - w tanh(travel / w)). Left to the prices, the hips stood
/// still (4500/m^2 against the cursor's bounded pull is 2cm) and BALANCE walled the lean in: a
/// chest pulled 10cm back stopped 5cm short, 20cm back 12 short with the lower spine at its
/// limits and the upper curled forward to counterweight it, 20cm forward 5 short — and no
/// step ever answered, the rig's trigger for trunk drags being a miss of 12cm.
constexpr float kTrunkLeanFree = 0.10f;
constexpr float kTrunkLeanWidth = 0.20f;
/// ... and no more of a lean than BALANCE has room for: a lean of the grabbed joint by l carries
/// the centre of mass l x (the upper body's mass moment over the hips / the body's mass x the
/// lever), and the support has only so much room the way the drag goes — 4cm behind a standing
/// figure's weight, 10 ahead of it. Past this share of that room the hips follow instead (a
/// neck pulled 20cm back leaned into the support's edge and stopped 27mm short, the follow
/// still waiting for a lean that could not come).
constexpr float kTrunkLeanBalanced = 0.8f;
/// The sway's roll answers a leg that has RUN OUT (see the sway in solveIk): in full while the
/// shortest-handed standing leg has under kSwayLegInHandFrom of length in hand where the cursor
/// takes the hips, none from kSwayLegInHandGone.
constexpr float kSwayLegInHandFrom = 0.03f;
constexpr float kSwayLegInHandGone = 0.10f;
/// The pelvis's rotation price is its PITCH's (kRootRotationStiffness); its heading and its roll
/// cost this many times as much (see the root's channels in solveIk).
constexpr double kRootSquareness = 5.0;
/// The spine's neighbours are held to the same SHARE of their ranges (see the couplings in
/// solveIk), per unit of share^2 — the order of a spine channel's own posture price in those
/// units (40), so an S-curve of half a range each way costs as much as the bends themselves.
/// A knee coming down over its planted foot holds its PLACE this softly (per m^2) until it has
/// landed (solveIk's hard live pins); its height is held in full.
constexpr double kKneeLandingHold = 1.0e4;
/// With her knees planted (hard live pins) a pelvis drag pays this many times the root's
/// rotation price for the pelvis's tilt (solveIk).
constexpr double kKneelTiltScale = 5.0;
/// The foot below a knee that is on the floor holds its PLACE this softly (per m^2): solveIk's
/// ball row.
constexpr double kKneelFootHold = 1.0e4;
/// A KNEELING FOOT LIES FLAT (2026-09-26; the user, with a photo of a kneel-sit: "feet get
/// unrealistically stretched ... way more common to flatten feet out against the floor"). The foot
/// behind a knee that is ON the floor stood on its tucked toes — the ball pinned hard at the floor,
/// the toes held level, the ankle at its dorsiflexion limit — where a kneeling person's foot lies
/// on its top, toes pointing back. Once the knee has landed (its height over its planted height
/// under kKneelFlatLandFrom .. To, figure-scaled) and while its hold stands, the foot's share
/// m_jsFootFlat eases in at kKneelFlatEaseStep a tick: the ball's row falls by kKneelFlatRowDecades
/// over the share (and its target tracks the ball, so the returning row starts from where the ball
/// lies), the toes' hold with it, the floor's rows come onto the foot, and the ankle's pitch channel
/// gets a REFERENCE at its plantar limit — advanced from where the channel stands at
/// kKneelFlatTurnStepDeg a tick, at kKneelFlatStiffness per rad^2 over the share — so the foot rolls
/// over its toes onto its top and the floor stops it there (a price only prefers less travel; the
/// pose is given), and every toe joint below the ankle eases straight the same way. The lying foot's
/// ball and heel offsets are re-seated on it (m_jsFlatBallOffset / m_jsFlatHeelOffset). Let go of by
/// its knee — the knee's hold fading with the body's rise; a knee held down by its ceiling keeps its
/// foot lying — the foot UNROLLS back onto its toes in place at kKneelFlatUnrollStepDeg, its ball
/// held by a soft spring where it lay and the knee above it under a ceiling that rises with the roll
/// (kKneeUnrollLift); at the handover the standing model returns from the tucked foot. A foot found
/// within kKneelFlatNearLimitDeg of its plantar limit under a planted knee at a press is flat from
/// the first tick. IK_JS_NO_KNEEL_FLAT restores the tucked-toe kneel; IK_JS_KNEELFLAT is the
/// stiffness; IK_JS_KNEEL_FLAT_TRACE prints each foot's share, reference and offsets per tick.
constexpr float  kKneelFlatEaseStep = 0.1f;
constexpr float  kKneelFlatUnrollStepDeg = 4.0f; ///< ... and back onto its toes (the UNROLL, under a body getting up, whose knee lifts with it) at this pace: 100 degrees in 0.4s
constexpr float  kKneelFlatTurnStepDeg = 2.5f; ///< (150 degrees a second: a foot on tucked toes rolls onto its top in three quarters of a second; at 4 the roll ran into a kneel's still hold at 26mm a tick at the toes)
constexpr double kKneelFlatStiffness = 300.0;
constexpr float  kKneelFlatLandFrom = 0.005f;
constexpr float  kKneelFlatLandTo = 0.02f;
constexpr double kKneelFlatRowDecades = 8.0;
constexpr float  kKneelFlatNearLimitDeg = 20.0f;
/// A knee whose lying foot is rolling back onto its toes may lift this far (m, figure-scaled) times
/// the roll's progress: it comes up as the toes tuck under, no faster. Held down until the handover
/// the rise stalled and let go in one tick (260mm); free, the knee lifted with the hips while the foot
/// hung in the air on its soft spring, and the returning rows brought it down in a tick (52-275mm).
constexpr float  kKneeUnrollLift = 0.25f;
/// ... and the FOOT comes off the floor to turn over: through the unroll the ball's soft spring also
/// asks its height up, to this far over its floor height by the roll's end (figure-scaled; the
/// handover then lands it at kKneelFlatLandStep). 0.12-0.25 measured alike on the eight rigs.
constexpr float  kKneelUnrollBallLift = 0.15f;
/// A LYING FOOT UNDER A DRAWN-UP KNEE (ikPinRows, ikPinRowsEnd) hangs with its lowest joint this far
/// (m, figure-scaled) clear of the floor while it still points down past level by more than
/// kLyingFootHangClearToDeg, none from kLyingFootHangClearFromDeg: hung exactly at the height that
/// put its lowest joint AT the floor, the toes dragged, the floor's rows bent them up to their
/// limits tick by tick (the hang read off the shape the rows had just bent — a ratchet), and they
/// sprang back 100mm at a tip when the turning foot lifted them off. The clearance fades as the
/// foot comes level so the landing is a landing.
constexpr float  kLyingFootHangClear = 0.03f;
constexpr float  kLyingFootHangClearFromDeg = 10.0f;
constexpr float  kLyingFootHangClearToDeg = 40.0f;
constexpr float  kKneelFlatTrackFrom = 0.3f; ///< the share from which the ball's target tracks the ball (the row is soft enough by then)
constexpr float  kKneelFlatLandStep = 0.01f;  ///< a foot rolled back to standing LANDS at this pace (m a tick), not a step's 2.5cm: hauled at the pins' pace onto a spot 16cm ahead with the row hardening, the leg shot straight (219mm at the knee in a tick)
/// ... and it is let go of altogether as the HIPS go ahead of that knee under a pelvis drag (a body
/// lowered onto its belly folds its shins up behind it): the foot's rows fade over this travel.
constexpr float kProneFootFreeFrom = 0.08f;
constexpr float kProneFootFreeFull = 0.25f;
/// ... and a planted knee is unloaded by a RISE only once the hips stand this share of the thigh's
/// length over it (getting up off the belly comes onto all fours first). The share is the cosine of
/// the thigh's lean from vertical AS THE DRAG FOUND IT: all fours puts the socket 0.94-0.97 of the
/// thigh over the knee on every reference rig (a hip drag up from there STANDS her), the base rig's
/// flat prone 0.44 - and a heavy character's half-flat prone, his arms as struts holding the
/// shoulders up, 0.70: past the 0.6 this was, so his knees lifted as the hips rose and he stood up
/// into a bear crawl where the base comes onto all fours (2026-09-26). 0.8 is a thigh 37 degrees
/// from vertical, a tenth of the thigh clear of both.
constexpr float kKneeUnloadHeight = 0.8f;
/// ... and such a knee is LET GO OF once the drag's TARGET rises past what a planted knee can serve
/// (2026-10-28; the knee-hold block of ikPinRows): the hold fades as the hips' height the target asks
/// for, over the knee's planted height, goes from this share of the ROOT'S STANDING HEIGHT OVER THE
/// KNEE (1.0 = a tall kneel, the thigh vertical) to the next. A function of the target: held for the
/// whole drag, a figure sat back on her heels could not be pulled up off her knees at all — the hips
/// rose to a tall kneel, 15-20cm, against a pull of 40-75cm, and stopped (the user's report, with a
/// picture). The belly's prone-up asks the hips to 0.75 of it on the base figure and on the heavy
/// character whose half-flat prone set kKneeUnloadHeight: it still comes up onto all fours and no
/// further. (A first cut sized the band by the parent bone's length beyond its deficit — the thigh's
/// TWIST bone on this generation, 25cm — and released the prone-up's knees at 25cm of rise.)
/// IK_JS_KNEE_HOLD_FOR_DRAG keeps the hold for the whole drag, as before.
constexpr float kKneeHoldReleaseFrom = 1.0f;
constexpr float kKneeHoldReleaseTo = 1.3f;
/// After the unroll's handover the knee's ceiling goes on rising at kPinEaseStep a tick for this much
/// more of lift (m, figure-scaled) before it is dropped: gone with the handover it let a knee catch
/// up with a far cursor in one tick (a 70cm yank out of a kneel: 375mm at the shin).
constexpr float kKneeUnrollExtra = 0.25f;
/// THE TRUNK COMES UP WITH THE RISE (2026-10-28; ikPostureModel, ikSpineCoupling, rightIdleHead): the
/// spine's references turn from the fold the drag found to upright by this share, which follows the
/// rise's progress at no more than kTrunkHomeStep a tick (a third of a second for the whole fold) and
/// WAITS while a hand is still planted (m_jsTrunkHomeShare). Pulled home against planted hands the
/// spine stored its strain and released it the tick the hands let go: 302mm at a finger, 152 at the
/// brow, standing up off all fours by the hips. IK_JS_NO_RISE_TRUNK_HOME is the A/B.
constexpr float kTrunkHomeStep = 0.03f;
constexpr double kSpineCoupling = 80.0;
/// ... and the NECK's links to it (see the couplings in solveIk): the neck is the spine's top.
constexpr double kNeckCoupling = 80.0;
/// The pelvis's horizontal price under a trunk drag whose stance cannot walk and whose lean has
/// run out of balance room (see m_jsTrunkCounter in solveIk): low enough that the balance row
/// and the cursor place the hips between them.
constexpr double kTrunkCounterStiffness = 250.0;
/// The spine's twist and side-bend channels under a trunk drag that stays in the sagittal plane
/// (see sagittalLock in solveIk): this many times their price.
constexpr double kSagittalLock = 20.0;
/// The bow's arc is a drag's that goes FORWARD: none of it under this cosine of the travel's angle
/// off the figure's heading, all of it from the second.
constexpr float kBowArcSagittalFrom = 0.70f;
constexpr float kBowArcSagittalFull = 0.92f;
/// How much of a rigid trunk's arc (see leanArc in solveIk) a spine that CURLS as it folds reaches.
constexpr double kBowArcShare = 0.75;
/// The pelvis pitch's nominal RANGE, degrees: what its share of the bend is measured in where it is
/// coupled to the first spine bone's (see kSpineCoupling) — a hip folds about 90 degrees.
constexpr double kTrunkHingeRangeDeg = 90.0;
/// ... and ON HER KNEES (m_jsKneelStart) the fold is the HIPS': the pelvis's pitch costs this, and
/// counts as this many degrees of range in the spine's coupling chain. Standing, a bow is shared
/// between the hips and a curling spine, and looks it; over planted knees the same sharing took a
/// figure going onto all fours into a cat stretch - the spine at its limits under a pelvis pitched
/// 50 degrees, the head tucked under between her arms - where a body folds 90 degrees at the hips
/// under a back that stays all but flat, its thighs upright over its knees.
constexpr double kKneelFoldStiffness = 5.0;
/// A HAND ON THE FLOOR lies on its PALM (see the live contacts in solveIk): the soft pulls, per
/// rad^2, that roll it flat about its fingertip (pitch, and level about the fingers' line) and
/// keep the fingers' heading. Soft beside everything that places the arm: a wrist a hand's length
/// over the floor cannot lie flat, and must not drag the shoulder down to try.
constexpr double kPalmPitchWeight = 30.0;
constexpr double kPalmLevelWeight = 30.0;
constexpr double kPalmHeadingWeight = 3.0;
/// ... and the palm's TARGET turns from the rotation the hand landed with toward flat at this pace,
/// degrees a tick: the pull is soft, but so is a wrist, and asked for flat at once a hand that
/// landed on its fingertips snapped down 16cm in the tick after.
constexpr float kPalmTurnStepDeg = 4.0f;
/// ... and the eased target never LEADS the hand by more than this (degrees; IK_JS_PALMLEAD): every palm
/// ease — a landed hand's rows, the lay on the body, the slide along the floor — turns its target toward
/// the lie at kPalmTurnStepDeg a tick whether or not the hand can follow, and a hand whose arm is folded
/// under a prone chest with its wrist pinned to the floor cannot turn at all: the target ran 78 degrees
/// ahead of it (3.7 a tick, the js-cost trace) and the pull, 30/rad^2 at 1.4 rad, finally snapped the arm
/// into the basin where the palm lies flat — the folded arm's ONE-TICK UNFOLD as the hips rise off the
/// belly (124-177mm at the forearm, a soft spot since 2026-09-23). Capped, the pull is bounded (30 x
/// 0.35^2), the hand turns as fast as it is able and no faster than 4 degrees a tick, and a hand that
/// cannot turn is left alone until the body's rise gives its arm the slack.
constexpr float kPalmLeadDeg = 20.0f;
/// A HAND LAID ON THE BODY (ikCursorRows): a dragged wrist whose target comes to rest against a body
/// volume — the hip, the chest, the head, a knee, the other forearm — lies on its PALM there as a
/// hand on the floor does. The lay comes in as the target's clearance of the nearest capsule closes
/// on the wrist's own volume clearance (IkRig::volumeClearance, 3cm for a hand: the drag clamp puts a
/// hand pressed to the hip exactly there): in full within kHandLayNear of it, none from kHandLayFar
/// (metres, figure-scaled). A function of the target, like the foot slide's band.
constexpr float kHandLayNear = 0.01f;
constexpr float kHandLayFar = 0.06f;
/// ... at the floor palm's weights (per rad^2, x the lay): the palm's tilt off the surface about the
/// two tangent axes, and the fingers' heading ACROSS it — soft, so the arm chooses (a hand on the
/// hip points its fingers down or forward as the elbow finds cheapest; the user turns it further).
constexpr double kHandLayPitchWeight = 30.0;
constexpr double kHandLayLevelWeight = 30.0;
constexpr double kHandLayHeadingWeight = 3.0;
/// ... and the lay's target turns from the rotation the hand had where its target entered the band
/// toward the lie at this pace, degrees a tick (the floor palm's kPalmTurnStepDeg). IK_JS_HANDLAYSTEP.
constexpr float kHandLayTurnStepDeg = 4.0f;
/// A HAND LIES ON WHAT IT RESTS AGAINST, NOT ON WHAT IT PASSES: the lay comes in as the target SLOWS -
/// its followed move per tick under kHandLaySpeedFrom in full, none from kHandLaySpeedTo (metres a
/// tick, figure-scaled: 4mm a tick is 0.24 m/s). Engaged at any speed, a hand dragged past the body -
/// the bench path along the hip, a seated hand reaching forward over its thigh, a hand rising past the
/// chest to the head - was turned onto each surface it brushed, and where the forearm's twist ran out
/// mid-pass the arm flipped basins (65-107mm in a tick on the oldest rigs, whose T-pose arms travel
/// furthest). A hand placed and held lies as before, and the pose at rest is the target's: only the
/// transient is the path's, as the ease's already was. IK_JS_HANDLAYSPEEDFROM/TO, IK_JS_HAND_LAY_ANY_SPEED.
constexpr float kHandLaySpeedFrom = 0.004f;
constexpr float kHandLaySpeedTo = 0.010f;
/// ... and the lie's target eases from the rotation the hand HAS at kHandLayTurnStepDeg × the lay a tick
/// — while the lay is under this much, the start rotation and the side are re-read every tick. Run at
/// full pace from the tick the target entered the band while the speed gate held the rows at nothing
/// through the move, the eased target stood at the full lie by the stop, the rows came in within two
/// ticks, and a hand still 40-55 degrees off was asked round at once (35-53mm hold jumps on four heavy
/// characters of the sweep).
constexpr float kHandLayEngage = 0.05f;
/// (A fade of the lay with the limb's STRETCH — a hand at full reach touching with its fingertips — was
/// tried and reverted: measured over the whole limb from the collar's root it read 0.5 on the head it
/// was made for, and a seated hand laid on the knee IS at the arm's full reach, the trunk leaning to it,
/// so the fade took its lay away on every rig. A hand at its joint LIMITS is another thing than a hand
/// at its length; the one case — a short-armed character's hand barely reaching the crown of its head,
/// the upper arm at 110 and the collar at its caps, the palm 41 degrees from flat and hopping 42mm in
/// the hold — stands as a soft spot.)
/// ... and WHICH SIDE of the hand lies on the surface — the palm, or the BACK of the hand — is
/// decided by the forearm's TWIST ROOM where the target enters the band: for each side, the rotation
/// from where the hand is to its lie, its part about the forearm's axis, against the room the twist
/// channels between the wrist and the elbow have in that direction (the pronation left). The PALM is
/// preferred in front of the body and the BACK of the hand behind it (a hand behind the back rests on
/// its back: a forearm cannot pronate that far round), and the other side is taken only when the
/// preferred one is short of twist by this many degrees more. Read off the wrist's own rotation where
/// the target entered the band the side was a coin flip — a hand arriving with its palm 79 degrees
/// off the sacrum was laid palm-in by the forearm twisted to its -90 limit — and the forearm's neutral
/// palm is the same coin (the wrist is unturned at entry). IK_JS_HANDLAYBACKMARGIN.
constexpr double kHandLayBackMarginDeg = 15.0;
/// AKIMBO: a hand laid on the HIP (the laid surface's normal lateral, the hand at the hips' height) asks
/// its ELBOW out — to where the upper arm stands kHandLayElbowOutDeg from straight down, out to the
/// side, at its own length from the socket — at kHandLayElbowWeight (per m^2) x the lay. A hand pressed
/// to the hip's side with the arm hanging straight is what the lay made before; nobody stands so.
constexpr double kHandLayElbowStiffness = 20.0; ///< per rad^2 x the lay, on the shoulder's abduction channel (a REFERENCE: a position row on the elbow leaned the trunk 9-18cm)
constexpr float  kHandLayElbowOutDeg = 45.0f;
constexpr float  kHandLayHipBand = 0.15f; ///< the laid hand within this of the hips' height (m, figure-scaled)
/// The reference never asks more abduction than the FOREARM can give with the hand on the hip: the
/// upper arm may stand out as far as the forearm still reaches the hand's target (the law of cosines
/// about the socket), less this margin. A fixed 45 on a broad-shouldered, narrow-hipped body - the hand
/// under the socket - was 3cm beyond the forearm, and the TRUNK paid it (head 71mm, knees 25 degrees).
constexpr float  kHandLayElbowReachMarginDeg = 5.0f;
/// The akimbo's share eases in time (a tick's step; it decays at the same pace when the lay lets go):
/// a hand grabbed beside the hip at rest is in the lay's band for a tick or two before it moves.
constexpr float  kHandLayElbowEaseStep = 0.05f;

/// THE DANGLE (2026-11-02): a figure lifted off the floor by a hand hangs from it — the legs come
/// together and hang plumb from the hips (the thighs kDangleThighForwardDeg ahead of plumb), the
/// knees soften to kDangleKneeDeg, the feet point kDangleFootDeg, the free arm hangs plumb (the hang's
/// plumb share reads the dangle). References at kDangleStiffness x the share, which eases in at
/// kDangleStep a tick while the rig has her suspended and out again at the same pace once she lands.
/// Before it she kept her STANDING pose in the air: the legs apart in the stance, the free arm out at
/// the A-pose angle, the feet level — a mannequin on a hook (`28_side_lifted_by_hand` from the front).
constexpr float  kDangleStep = 0.03f;
constexpr double kDangleStiffness = 20.0;
constexpr float  kDangleThighForwardDeg = 5.0f;
constexpr float  kDangleKneeDeg = 10.0f;
constexpr float  kDangleFootDeg = 25.0f;
/// ... and the hold of the FINGERTIP's height on the floor, per m^2: the contact's own, faded as
/// the contact is. (Softened to 2000 for a day so that a figure taken back up off all fours in the
/// same drag would let go of the floor - which was the contact's UNLOAD being read over the height
/// the hand touched at, not the height it lies at: see the settle in IkRig::updateContacts and
/// m_jsContactRise. Soft, it let the hands slide and cross under a body coming down: the male
/// rig's landed 20cm lopsided, the chest 16cm off its cursor.)
/// A hand contact's arm is SLACK — the hand held down, a ceiling on it — while the wrist's reach from
/// its limb junction is under this share of the arm's span; above it the rise unloads the hand. Kept
/// ABOVE the rig's lift-off test (kLiveContactTaut 0.95), so a hand hanging from its straight arm
/// reads taut there every tick and lifts off (solveIk, the live-contact block).
constexpr float kHandSlackShare = 0.97f;
/// ... and the hold FADES over the arm's last few percent of straightening, never at once (2026-10-27;
/// handTautShare in armatureiksolverows.cpp): by the limb's own stretch from kHandSlackShare to dead
/// straight (1.0), and by the junction's reach over this band below kHandSlackShare. "Slack" had been
/// a switch, and thrown with the body's rise already complete it took a hand's hold from 1e8 to
/// nothing in ONE solve — the arm the pin had stretched to its limits snapped back toward the fold the
/// drag began in: 219mm at a thumb, the prone-up's pop (the collar 13 degrees, the shoulder 15, the
/// forearm 20 in a tick). The arm's references are re-based where the unload finds them as well.
/// IK_JS_HAND_UNLOAD_SWITCH restores the switch.
constexpr float kHandTautReachBand = 0.03f;
/// ... and EASED IN TIME: a hand's unload (the rise's share times the taut share) may move by this
/// much a tick, so the hold fades over half a second however fast the arm crosses the taut band — at
/// a drag's pace the band is two ticks, and the trunk the hand had been holding down sprang 122mm at
/// the brow in the one tick the hold went. A hard row releases what it held in its LAST decades
/// (half of 1e8 is as rigid as all of it), so the pace of the fade is what spreads the trunk's
/// release: at a tenth a tick the brow still moved 83mm in one tick, at a twentieth 77, at a
/// thirtieth 31 (m_jsHandUnload; IK_JS_HANDUNLOADSTEP is the lever, the switch probe disables it).
constexpr float kHandUnloadStep = 0.03f;
/// THE TRAILING HAND LETS GO UNDER A DESCENT (2026-10-29; the live-contact block): under a pelvis drag
/// going DOWN (m_jsTargetDown), a hand contact whose arm is TAUT and whose wrist lies BEHIND its
/// shoulder along the figure's heading — the body slid forward OVER it — is a leash, not a support: in
/// the prone from all fours both arms ended dead straight and horizontal, trailing the shoulders by
/// their whole length, and held the chest back (the hips 109mm from their cursor on the base rig,
/// 235 on the heavy male with the hips 10cm above his belly; the hands hovering at hip height). It is
/// released as a rise releases a taut hand — the eased unload, the arm re-based, the rig's lift-off,
/// the descent's no-lift-off rule standing down for it — and the arm hangs along the body. The
/// unload's share rises over the wrist's trail behind the shoulder, as a share of the arm's span.
/// IK_JS_NO_HAND_TRAIL is the A/B.
constexpr float kHandTrailFrom = 0.45f;
constexpr float kHandTrailFull = 0.75f;
/// THE FACE LIFTS OFF THE FLOOR (2026-10-29; ikChooseUnknowns, ikPostureDofs): under a pelvis drag going
/// DOWN, the NECK chain joins the unknowns as the trunk tilts past the head righting's fade (the same
/// 70-110 degrees: on all fours or lying down the neck is not held up against the trunk), priced as the
/// trunk over the tilt's share and frozen (the floor below) under it — no lever for the balance row in
/// a crouch. A prone trunk came down onto its FACE: the brow's floor rows held the chest up by the
/// neck's length — the body on its face, its knees and its toes, the hips 19-22cm up on three rigs —
/// since nothing in the solve could turn the neck. Freed, the neck extends as the chest comes down:
/// the chin up, the chest on the floor. IK_JS_NO_FACE_LIFT is the A/B.
/// A TRAILING hand's hold under a pelvis descent is a FRICTION contact: its pull is bounded at this
/// (per metre of miss over the Huber knee, the cursor's units; a pelvis drag pulls at 1000) while
/// its weight fades. Released by weight alone the leash let go geometrically — the third
/// generation's hips stood dead for thirty ticks against a cursor 29cm ahead and paid the backlog
/// off in one 112mm tick. Bounded, the hand slides along the floor behind the body from the
/// trail's first tick and no backlog forms.
constexpr double kHandTrailPull = 300.0;
/// THE PRONE's share (ikChooseUnknowns): a kneel-start pelvis drag's target this far of the way from
/// the hips at the press down to the hip joint's lying clearance (none .. all), while ahead of the
/// planted knees (kProneFootFreeFrom/Full). From 0.4-0.8 the hands' release came after the body
/// had stuck against the pelvis's dear tilt price, and the backlog went in one tick: 139mm on the
/// third generation, 101 on the base's variant; at 0.2-0.5 they read 98 and 80 with the lying
/// pose unchanged (0.1-0.5 and 0.15-0.45 measure alike: the `ahead` share saturates first).
constexpr float kProneLieFrom = 0.20f;
constexpr float kProneLieFull = 0.50f;
constexpr float kFaceLiftFloor = 0.02f;
/// ... and a hand the RISE has unloaded (its arm not slack) is TAUT to the rig's lift-off once the rise
/// term has faded its hold this far along the six-decade fade (0.9: 1e8 x 10^-5.4 = 400/m^2, under the
/// pelvis's own price — a spring that holds nothing; m_jsContactRiseUnloaded, IK_LIFT_TAUT_ONLY). At
/// 0.999 the flag came two ticks before the pop it was made for, and the lift-off's three-tick
/// confirmation put the release IN the pop tick (2026-09-26).
constexpr float kRiseUnloadedShare = 0.9f;
/// ... and past this share of the fade (a hold under 200/m2) the hold is taken OUTRIGHT: the rig lets the
/// contact go at once, whatever its height (2026-10-27; IkRig::riseUnloadedOutright, IK_LIFT_UNLOADED_WAITS).
/// At 0.999 the flag came in the tick whose solve threw the arm.
constexpr float kRiseUnloadedOutrightShare = 0.95f;
/// A kneel-start trunk drag unloads its HAND contacts over this much upward travel of the target
/// (metres, figure-scaled): the trunk leaving its hands, which come off the floor at once.
constexpr float kKneelUpUnloadCm = 0.015f;
constexpr double kFingertipHold = 1.0e8;
/// A HANGING arm's elbow SOFTENS as its hand nears the floor (hangIdleArms): this much flexion,
/// eased in over the fingertip's last kArmLandSoftenFrom of height, at kArmLandSoftenStepDeg a
/// tick. An arm that lands dead straight with its hand straight below it is a fully extended
/// chain - the straight limb's saddle: shortening it is invisible to a first-order model - and
/// the tick such hands touched down the chest lost its cursor by 3-5cm, the solver's fold retry
/// kicked both elbows, and the pose hopped to the basin it found: the trunk 13 degrees back up,
/// 15cm at the brow. A relaxed arm is not a ramrod either.
constexpr float kArmLandSoftenDeg = 10.0f;
constexpr float kArmLandSoftenFrom = 0.15f;
constexpr float kArmLandSoftenStepDeg = 1.5f;
constexpr double kKneelFoldRangeDeg = 300.0;
/// THE SEAT ROLLS (solveIk): the share of the target's swept angle about the seat that the
/// pelvis's pitch reference takes, and the most it takes either way (degrees).
constexpr double kSeatRollShare = 1.0;
constexpr double kSeatRollMaxDeg = 110.0;
/// THE BOW: under an upper-body trunk drag that began standing the pelvis's PITCH joins the
/// unknowns, per rad^2 (see m_jsTrunkHinge in solveIk).
constexpr double kTrunkHingeStiffness = 20.0;
/// The hinge's release once a pelvis drag WALKS: slack per tick, and the slack at which the row is
/// dropped for the rest of the drag (no imbalance a standing figure can have is that large).
constexpr float kPelvisBalanceReleaseStep = 0.005f;
constexpr float kPelvisBalanceReleaseGone = 0.40f;
/// ... and as the hips LEAVE THE STANCE (their horizontal distance from where the stance, as the
/// rig has walked it, puts them): past kPelvisBalanceStanceFrom the row's slack grows
/// kPelvisBalanceStanceGain for every metre further — the imbalance grows one for one, so the
/// lean is given back at half the hips' pace and is gone 12cm on. (As a WEIGHT, fading over
/// 12-18cm, it did nothing for nine tenths of its range and let the whole lean go in the last:
/// a hard row's pose barely reads its weight until the posture terms rival it.)
constexpr float kPelvisBalanceStanceFrom = 0.12f;
constexpr float kPelvisBalanceStanceGain = 1.5f;
/// Under a hip sway (see the sway's roll in solveIk) the CHEST stays LEVEL: its roll about the
/// figure's fore axis held as the drag found it, per rad^2 — against four or five spine joints
/// at the trunk's price (~150 each) that is 97% of the pelvis's roll taken up in the spine.
/// Riding the rolled pelvis rigidly, the whole torso leaned away from the sway like a mast: 8
/// degrees for 8cm, the head back over where the hips had been.
constexpr double kSwayChestLevelWeight = 1000.0;
/// A dragged ELBOW's hand tends to STAY where it is (see the elbow drag in solveIk): its wrist held
/// at its place as the drag found it, per m^2 — two decades over the arm's own posture terms, so
/// the elbow swivels about a hand that stays to the millimetre where the arm's geometry lets it
/// (elbows out, in, tucked: what an elbow is grabbed FOR), and well under the cursor's pull
/// (200 / (2 x 300) = 33cm of stretch before the cursor gives any ground), so the hand follows
/// wherever the forearm's length says it must. It goes as the elbow's target leaves the forearm's
/// length of that place (from kElbowHoldFrom, gone by kElbowHoldGone): an elbow taken right away
/// carries its arm, as it always did.
constexpr double kElbowHandHoldWeight = 300.0;
constexpr float  kElbowHoldFrom = 0.10f;
constexpr float  kElbowHoldGone = 0.30f;
/// ... and it comes WITH THE ARM'S BEND as the drag found it (degrees of elbow flexion: none under
/// kElbowHoldBendFrom, in full from kElbowHoldBendFull): a STRAIGHT arm has no swivel — its elbow
/// lies on the line from shoulder to hand, and cannot leave it with both ends where they are —
/// so there the elbow carries its arm, as it always did. (Held, a straight arm's elbow dragged
/// 10cm inward went nowhere — the fold is bounded at straight — and 15cm forward ran the collar
/// to two of its limits and the shoulder's twist to a third, the girdle making the room the arm
/// could not.)
constexpr float  kElbowHoldBendFrom = 10.0f;
constexpr float  kElbowHoldBendFull = 40.0f;
constexpr float  kElbowReachEase = 0.10f; ///< a target inside the upper arm's reach: its depth eases in over this much
constexpr float  kElbowReachBand = 0.06f; ///< a target this far beyond it still comes back onto it
/// Under an elbow drag the shoulder GIRDLE (what lies between the arm's socket and the chest: the
/// collar) costs this many times its range-normalized price: an elbow is placed by the SHOULDER.
/// A collar's range is a third of a shoulder's, but so is its lever's reach — at its plain price
/// it did a third of every elbow move, carrying the socket 6cm as an elbow swung 8.
constexpr double kElbowGirdleStiffness = 20.0;
// THE COLLAR ELEVATES THIS MUCH AT MOST under a hand drag (2026-09-25; ikStiffnessClasses finds the
// dragged arm's collar and its elevation channel — the unlocked channel about the fore axis — and
// ikPostureModel narrows that channel's box to +-this about the bind, never tighter than where the
// collar already stands: the fold bound's idiom; IK_JS_COLLARMAX). The rigs author 55 degrees; a
// clavicle rises about 30, and a hand raised overhead takes exactly that on the base rig. At the
// limb's price a collar with a 65-degree range is the cheapest joint in the arm, and a hand dragged
// straight to the nape from the rest pose had it at all three of its limits (twist 30, swing -17,
// elevation -55), the socket hauled 15cm up onto the neck; the cap halves the hoist. THREE MORE WERE
// TRIED AND REJECTED THE SAME DAY, each measured on the eight rigs: (1) the collar's twist and swing
// PRICED like the trunk (x4-x100) — those two channels are what carry an arm smoothly from one basin
// to the next (a hand raised beside the head from the rest pose goes up the EXTENSION route first,
// the arm swung back, and comes round into flexion as it nears the head, the collar's swing bridging
// the two): priced x10 and more the male rig's arm FLIPPED 136mm in one tick where it had moved 19,
// and every other rig's move doubled; (2) a SCAPULOHUMERAL RHYTHM coupling (the collar's elevation
// held to half the arm's, 200/rad^2) — it subsidized elevation through the collar, sent the same
// gesture up the extension route to the -40 limit and flipped it 174mm on the base rig; (3) a
// SCAPULAR PLANE bias (the arm's swing reference 30 degrees forward for a target above the socket)
// — a tie-breaker at the limb's price that moved reachable poses (a phone to the ear from 41 to 77
// degrees of flexion) and nothing out of reach.
constexpr double kCollarElevationMaxDeg = 30.0;
// ... and its SWING and TWIST this much (IK_JS_COLLAROTHERMAX): a clavicle protracts or retracts
// about 15 degrees and turns about as much on its axis. With the elevation alone capped, a hand
// pulled straight up beyond the arm's reach jammed the base rig's collar twist at -30 and its swing
// at +26 — their authored limits — for the reach the elevation no longer gave: the pull takes
// whatever collar freedom is left. A BOX, not a price: the collar's swing and twist within these
// bounds are what carry an arm smoothly from one basin to the next (the rejected price above), and
// a box is inert until reached.
constexpr double kCollarOtherMaxDeg = 15.0;
// (Both are a STANDING body's: a body hanging by its hand has its girdle stretched to the authored
// limits — with the box the two oldest generations' rigid hang rested 81-87mm short of the cursor,
// 52-66 before — so the box stands down while suspended.)
/// ... and the planted foot below a dragged knee may TURN on its spot (its heading, per rad^2:
/// every other planted foot's is 4e6): a knee swung out or in over a foot that must keep pointing
/// ahead is a knee that cannot go — the leg turns out from the hip, foot and all, or not at all.
/// Held hard, a knee dragged 15cm outward went 4cm, the shin's side bend and the ankle's turn at
/// their limits.
constexpr double kKneeFootHeadingWeight = 5.0;
/// What a planted leg's channels gain, per rad^2 at the full rise, toward the rest stance (see
/// the RISING note at the unknowns). A TIE-BREAKER, and it must stay one: at 2 the legs
/// straightened eagerly enough to push the hips up and forward ahead of the chest the user held,
/// the torso was crushed between them and the spine arched to its limits and twisted; the final
/// stance is as clean at 0.02 as at 2 (the upright pelvis is what makes it).
constexpr double kRiseHomeStiffness = 0.2;
/// The slack-hand CEILING (a hand held DOWN at its planted height while its arm has length to
/// give: a body pushing up off its belly keeps its hands on the floor as the arms straighten)
/// stands down for a body COMING DOWN — over this much of the target's downward travel since
/// the drag began (m, figure-scaled). See the live-contact block in solveIk.
constexpr float  kHandCeilingDownFrom = 0.02f;
constexpr float  kHandCeilingDownFull = 0.08f;
/// ... by RISING: the ceiling stands this far over the planted height (m, figure-scaled) at the
/// last of that travel, and is gone from there. A hard row is let go of through its slack, never
/// its weight (the hip hinge's lesson): faded by weight it let the hand go all at once.
constexpr float  kHandCeilingSlack = 0.06f;
constexpr float  kRootYieldDownFrom = 0.05f;
constexpr float  kRootYieldDownFull = 0.20f;
constexpr float  kRootYieldShareFrom = 0.60f;
constexpr float  kRootYieldShareFull = 0.90f;
constexpr float  kRootYieldRoomFrom = 0.02f;
constexpr float  kRootYieldRoomFull = 0.08f;
constexpr double kMinRangeDeg = 20.0;
constexpr double kFreeRangeDeg = 360.0;
/// The floor's and the body volumes' one-sided rows: harder than the cursor (its saturated pull
/// sinks a joint 0.5mm into a volume), softer than a pin. Rows are raised for joints within
/// kPlaneMargin of a surface, so a trial step INTO it is seen by the cost that judges the step.
constexpr double kPlaneWeight = 1.0e7;
constexpr double kPlaneMargin = 0.02;
/// A PLANTED LEG'S THIGH ABDUCTS THIS MUCH AT MOST under IK (2026-10-27; degrees about the bind,
/// never tighter than where the thigh stands — the fold bound's and the collar box's idiom). With
/// the rotation order composed the format's way a flexed thigh ABDUCTS where it used to roll,
/// and every deep floor gesture found the cheapest way down: both knees splayed out to 37
/// degrees — a frog — 12-24cm outboard of the feet in every kneel, all fours, prone and lunge on
/// every rig, the hips reaching a cursor rigid thighs over planted knees should have stopped
/// them short of. The rigs author 85 degrees of abduction (a split); a standing person keeps it
/// under about 20, and lowers a kneel by sitting back, not by splaying. A knee band — the knee
/// held within a hand of its foot's line — was tried first and blocked every sideways gesture:
/// a leg DOES lean over its planted foot, the ankle everting and the sole rolling. A LIFTED leg
/// (no pin on its foot) abducts to the rig's own limit: a side kick, a knee out to the side.
/// IK_JS_THIGHABDUCT is the box in degrees (0 = none). (20 first: the male rig's kneel rested
/// its knees 7cm outside the feet's line, 4 at 15.) THE BOX TIGHTENS WITH THE THIGH'S FLEXION: a
/// straight leg LEANS over its planted foot — the trailing leg of a side step, a lateral walk,
/// a sway — and boxed at 20 those walks bought the lean with 17-29 degrees of knee twist; a
/// FLEXED thigh is a body lowering itself, whose knees track its feet. kThighAbductBoxStraightDeg
/// under kThighAbductFlexFromDeg of flexion, kThighAbductBoxDeg from kThighAbductFlexFullDeg,
/// smooth between (IK_JS_THIGHABDUCT_STRAIGHT) — and the tight box under a pelvis drag going
/// DOWN (m_jsTargetDown) whatever the flexion: a kneel spent its abduction in the descent's
/// first centimetres, the thigh still near straight, and a box never tighter than where the
/// thigh stands could not take it back.
constexpr float  kThighAbductBoxDeg = 15.0f;
constexpr float  kThighAbductBoxStraightDeg = 30.0f;
constexpr float  kThighAbductFlexFromDeg = 30.0f;
constexpr float  kThighAbductFlexFullDeg = 60.0f;
/// THE FOOT CONTACT MODEL. A standing foot is held where a foot really bears: at the BALL (the
/// first floor-level joint under the ankle), hard; the HEEL (the ankle, the rig's pin) only
/// softly at its spot and never below it (the floor row); the sole's yaw and roll hard, its
/// pitch about the ball nearly free; the toes' own orientation held, so they stay flat as the
/// foot pitches over them. A heel lift is then not a policy but the cheapest pose when the
/// ankle's dorsiflexion runs out under a deep crouch or a kneel — or the legs' length under a
/// high reach (tiptoe) — and costs kHeelHoldWeight x rise^2 against the cursor's bounded pull:
/// 200 / (2 x 2000) allows a hand or head pull 5cm of it, a pelvis drag's stronger pull the
/// ankle's whole range, and nothing lifts while the pose has any cheaper way.
constexpr double kHeelHoldWeight = 2000.0;
constexpr double kSolePitchWeight = 20.0;
constexpr double kToeHoldWeight = 4.0e4;
constexpr double kSoleRollWeight = 5.0;
/// The ball is looked for among the pin's descendants whose bind height is under this fraction
/// of the pin's own (an ankle 8cm up: under 3.6cm) and which carry children (not a toe tip).
constexpr float kBallHeightFraction = 0.45f;
/// (A FOLD channel and the HINGE test are the Armature's own: kFoldRangeDeg, kFoldNarrowFraction,
/// kFoldHingeOtherDeg and isFoldChannel / foldAxisOf in armature.h.)
/// (Channels with under Armature::kLockedRangeDeg of range are locked: not unknowns.)
/// A pin target the rig MOVED (ground healing, a heel lift, a landing, a slide) is approached
/// at this pace rather than jumped to; faster than the stepper's own 1.5cm/tick swing.
constexpr float kPinEaseStep = 0.025f;
/// A limb asks the BODY up only by what lies beyond this share of its reach (see the rise in
/// dragIkTick): a straight arm is not a comfortable one, so a little short of the whole.
constexpr float kLimbReachShare = 0.95f;
// THE TIPTOE (dragIkTick, ikPostureModel, ikPinRows; 2026-09-25): a hand pulled UP beyond the
// arm's reach lifts the HEELS — the body rises onto the balls of the feet before the lift-off
// takes her off the floor. The root's home rises by what the limb cannot give upward (the
// limb rise's own measure, less the height the legs had left), up to the heel lift this much
// plantar flexion about the ball gives: d sin t + h0 (cos t - 1) for an ankle d behind the
// ball and h0 over it — 6cm on the base rig (d 12.5cm, h0 5cm). Real tiptoe is 35-45 degrees
// of plantar flexion beyond standing; the ankle's own limit is the box, and an over-ask rests
// there. Before it a hand pulled 10cm beyond reach lifted the heels 15mm (the soft heel rows'
// 2 x 2000 and the root's 1000 against the cursor's bounded pull) and rested 97mm short.
// IK_JS_NO_TIPTOE is the A/B, IK_JS_TIPTOE_DEG the pitch.
constexpr double kTiptoeDeg = 35.0;
// ... and it begins where the arm's OWN reach ends — the target beyond the limb's full length
// from its socket, not the limb rise's kLimbReachShare (0.95): at 0.95 a hand that could reach
// flat-footed (+86cm on the base rig, 0.04mm short) went 4.6cm onto its toes.
constexpr float kTiptoeReachShare = 1.0f;
// THE BONE CLASSES AND THE DIGIT DRAG (armatureikdigit.cpp; 2026-09-28, the user: "the face rig
// joints are specifically for expressions and should not be part of the IK ... only the head itself
// should be selectable ... IK in the fingers and toes should only apply to that digit, stopping at
// the hand or foot"). A DIGIT's joint hangs below a wrist or an ankle and BENDS: its widest channel
// LIMITED channel spans this much or more (degrees). A carpal's does not (7-20 degrees on the eight
// reference rigs): it is the hand's own flesh, and a grab of one is the limb's. (The idle hands'
// relax reads the same line between a finger and a carpal.)
constexpr float kDigitMinRangeDeg = 30.0f;
// ... and it stands OUT in its hand or foot: this share or more of the hand's or the foot's reach
// (its farthest joint from the wrist or the ankle, at bind) from that wrist or ankle. The range
// alone took the helper bones at the ankle for toes — a mid-foot bone 3-9mm from the ankle with
// 45-60 degrees of range, a heel bone — 0.02-0.05 of the foot's reach out; a thumb's base, the
// digit joint nearest its limb's end, stands 0.15-0.17 out on every generation.
constexpr float kDigitMinReachShare = 0.10f;
// (... and a digit's joint is FOOT-CLASS where it is a toe's: IkRig::isFootClass, with a foot-class
// parent — the ankle is the foot's most proximal such joint.)
// THE LANDING BOUNCE (armaturelanding.cpp; 2026-09-28, the user: "if she lands on her feet ... add
// a natural bounce to her legs ... greater the more acceleration there is in the drop ... purely
// aesthetic"). A figure the Ground button dropped onto her FEET absorbs the landing: the hips dip
// over the planted feet, the knees folding, and come back up. The DIP is the impact speed's:
// kLandingDipPerSpeed metres for every m/s the fall landed at (sqrt(2 g h): 6cm for half a metre's
// drop, 2cm for 5cm), saturating toward kLandingDipMax of the figure's scale (a quarter squat:
// tanh, so a higher drop always dips deeper), and under kLandingDipMin there is no bounce at all
// (a drop of a millimetre). (0.030 first: a 5cm drop then folded the knees 30 degrees — near
// straight a leg's height goes as the SQUARE of the knee's angle, so a shallow dip is a deep bend.)
constexpr float kLandingDipPerSpeed = 0.028f; // (0.020 until 2026-11-05: the user, of a 50cm drop, "the bounce seems too slight")
constexpr float kLandingDipMax = 0.24f;
constexpr float kLandingDipMin = 0.003f;
// ... its SHAPE: down over the first kLandingDownShare of its time (sin^2: deepest a third of
// the way in, a landing's absorption) and back up over the rest (cos^2), so the dip begins and
// ends as the SQUARE of the time — and the knee, whose angle goes as the dip's square root,
// starts and stops at a steady pace. A damped spring's half wave, e^(-zeta omega t) sin(omega_d
// t), was the first shape: it starts at full speed, and the knee of a straight leg then travelled
// 8-12cm in the landing's second tick (28mm of dip is 28 degrees of knee). And it LASTS
// kLandingSeconds for a dip at the cap, kLandingShortest of that for a slight one: a harder
// landing takes longer to absorb.
constexpr float kLandingSeconds = 0.50f;
constexpr float kLandingShortest = 0.70f;
constexpr float kLandingDownShare = 0.32f;
// ... and over the last of it the pose is BLENDED back to the pose she landed in (from this share
// of the duration, smoothstep to 1 at its end), which the bounce's end restores exactly: what the
// solve leaves different at a dip of zero — a foot the rig healed flat, a heel brought down — eases
// home instead of snapping there. Only the LAST of it, where the dip is a tenth of itself and the
// two poses all but one: a blend of poses in joint space slides planted feet (from 0.6 of the
// duration the toes slid 4mm on the floor under a 50cm drop's bounce, 7 under a 2m drop's).
constexpr float kLandingBlendFrom = 0.85f;
/// ... and the pace a LIFTED body comes up to its cursor at (see dragIkTick): per tick.
constexpr float kLiftRiseStep = 0.025f;
inline const bool kNoLiftEase = std::getenv("IK_JS_NO_LIFT_EASE") != nullptr; // A/B probe
inline const bool kNoRiseTrunkHome = std::getenv("IK_JS_NO_RISE_TRUNK_HOME") != nullptr;   // A/B probe: the trunk keeps its fold through a rise (ikPostureModel, ikSpineCoupling, rightIdleHead)
inline const bool kHandUnloadSwitch = std::getenv("IK_JS_HAND_UNLOAD_SWITCH") != nullptr;  // A/B probe: a hand's hold let go of at once, as before 2026-10-27 (ikPinRows)
inline const bool kKneeLandingHeld = std::getenv("IK_JS_KNEE_LANDING_HELD") != nullptr;    // A/B probe: a landing knee nailed to the spot it hovered over (ikPinRows, dragIkTick)
/// A cursor jump is solved through intermediate targets this far apart (continuation keeps
/// the solve on its branch; it costs iterations, never lag).
constexpr float kTargetSubstep = 0.04f;

template <typename T> T envOr(const char* name, T fallback) {
    const char* env = std::getenv(name);
    return env != nullptr ? static_cast<T>(std::atof(env)) : fallback;
}

// Env probes read by more than one stage of the solve (a stage-local static would be one stage's).
inline const double kRootRot = envOr("IK_JS_ROOTROT", kRootRotationStiffness);
inline const bool kHangAsFound = std::getenv("IK_JS_KNEE_HANG_AS_FOUND") != nullptr;   // A/B probe: the knee hang's found offset
inline const bool kNoUnderEase = std::getenv("IK_JS_KNEE_UNDER_NO_EASE") != nullptr;  // A/B probe: the hanging foot's target unEASED

/// Per damping level: the posture easing kappa (each tick the redundant posture covers
/// 1 / (1 + kappa) of what separates it from its solution: 95% in 0 / 4 / 7 / 13 ticks) and the
/// target follower's natural frequency in rad/s (critically damped; it trails a steadily moving
/// cursor by 2v/omega — one tick of cursor travel at 120 — and 0 = no follower).
struct DampingLevel {
    double kappa;
    float  followOmega;
};
constexpr DampingLevel kDampingLevels[Armature::kIkDampingLevels] = {
    {0.0, 0.0f}, {1.0, 120.0f}, {2.0, 80.0f}, {4.0, 50.0f}};
/// The drag tick (VulkanWindow's 60 Hz timer; the harness's tick is the same unit).
constexpr float kTickSeconds = 1.0f / 60.0f;

} // namespace pose

#endif // ARMATURE_IK_SOLVE_TUNING_H
