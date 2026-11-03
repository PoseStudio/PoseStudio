# The PoseStudio Rig Standard

PoseStudio poses every figure through one full-body IK engine. The engine was written against
one family of figures and, for a long time, knew the body's parts by the names those figures give
their bones. The rig standard replaces that with a vocabulary of **roles**: what each bone *is*
to the engine, whatever the file called it. An imported figure is conformed to the standard at
load; a figure authored for PoseStudio is built to it.

This document is the standard's definition. It describes what exists in the code today
(`src/viewport/scene/ik/rigroles.{h,cpp}`) — the roles, how a figure is conformed, the names —
and, in the last section, what the standard will grow to cover. It names no vendor's products or figure lines: the roles are anatomy.

## What the standard is, and is not

The standard is a **schema of roles over a figure's own skeleton**. A figure keeps its bones: its
skin weights, its pose correctives, its face rig and its pose files are all authored against
those exact bones at those exact joint positions, and nothing about conforming changes them. What
conforming adds is a role on every bone, so that the engine can ask for "the left thigh" or "a
twist helper of the forearm" and get the bone that plays that part.

The standard is **not** a replacement skeleton. No bone is created, removed or re-skinned.

## The roles

A bone has exactly one role. The roles are the complete set a human rig needs to pose every part
of the body; a figure may lack some of them (no twist helpers, no toes, no neck bone) and still
pose, with those roles empty.

| Role | Side | Index / joint | Required | What it is |
|---|---|---|---|---|
| Hips | centre | | yes | The pelvis **joint** the whole body hangs from: the solve root. |
| Pelvis | centre | | no | The pelvis **bone** both legs hang from, on a rig that has one beside the hips. |
| Spine | centre | link 1..N | yes (1 or more) | The spine chain from just above the hips to the chest top, the link the collars and the neck leave. |
| Neck | centre | link 1..N | no | The neck chain from the chest top to under the head. |
| Head | centre | | yes | The head joint. |
| Jaw | centre | | no | The lower jaw. |
| Eye | L / R | | no | An eye. |
| Face | any | | no | Every other bone of the face rig: everything under the head. |
| Pectoral | L / R | | no | A chest rider (a breast bone): token mass, no limb hangs from it. |
| Collar | L / R | | no | The clavicle: the bone the arm's socket hangs from. |
| UpperArm | L / R | | yes | The shoulder joint's bone. |
| UpperArmTwist | L / R | 1.. | no | A twist helper of the upper arm, on the chain or a leaf beside it. |
| Forearm | L / R | | yes | The elbow's bone. |
| ForearmTwist | L / R | 1.. | no | A twist helper of the forearm. |
| Hand | L / R | | yes | The wrist's bone. |
| Carpal | L / R | 1.. | no | A bone of the hand that carries a finger or two: a carpal, a metacarpal. |
| Finger | L / R | digit 1..5, joint 1.. | no | A finger joint. Digit 1 is the thumb, 5 the little finger; the joints count from the hand. |
| Thigh | L / R | | yes | The hip socket's bone. |
| ThighTwist | L / R | 1.. | no | A twist helper of the thigh. |
| Shin | L / R | | yes | The knee's bone. |
| ShinTwist | L / R | 1.. | no | A twist helper of the shin. |
| Foot | L / R | | yes | The ankle's bone: the leg's most proximal foot-class joint. |
| Heel | L / R | | no | A heel bone under the ankle. |
| Metatarsal | L / R | 1.. | no | A mid-foot bone between the ankle and the ball, or beside the ankle. |
| Ball | L / R | | no | The ball of the foot the toes fan from. |
| Toe | L / R | digit 1..5, joint 1.. | no | A toe joint. Digit 1 is the big toe, 5 the little toe. |
| Helper | any | | no | A rig helper with no joint of its own: an anchor, a control bone, a skinless leaf. |
| Other | any | | no | A bone of the body the standard has no role for. The conformance report lists these. |

A twist helper is a bone with two locked rotation channels whose free channel lies along the
bone. It exists to spread a limb's axial twist across the skin and must never bend; the engine
treats it as part of the rigid segment it sits in.

### Sides

The figure faces +z at bind. Her **left is +x**. A bone's side is read from its bind position
against the sagittal plane through the hips; bones of a limb inherit the limb's side. Centre
bones stand within a centimetre of the plane (the hips, the spine, the neck, the head do, to a
tenth of a millimetre).

Every side role is expected to have a twin on the other side with the same index and joint.

### The standard's names

A figure authored to the standard names its bones by their roles, and conforms with no inference
at all. The grammar: **lowercase words, the part first, its number next, the side last.**

| Part | Names |
|---|---|
| Trunk | `hips`, `pelvis`, `spine1` … `spineN` (lumbar first), `neck1` … `neckN`, `head`, `jaw` |
| Face | the eyes `eye_l`, `eye_r`; every other face bone keeps the name its rig gave it |
| Chest riders | `pectoral_l`, `pectoral_r` |
| Arm | `collar_l`, `upper_arm_l`, `upper_arm_twist1_l` …, `forearm_l`, `forearm_twist1_l` …, `hand_l` |
| Hand | `carpal1_l` …; the fingers `thumb1_l` … `thumb3_l`, `index1_l` …, `middle1_l` …, `ring1_l` …, `pinky1_l` …, the joints counted from the hand |
| Leg | `thigh_l`, `thigh_twist1_l` …, `shin_l`, `shin_twist1_l` …, `foot_l` |
| Foot | `heel_l`, `metatarsal1_l` …, `ball_l`; the toes `bigtoe1_l` …, `indextoe1_l` …, `middletoe1_l` …, `ringtoe1_l` …, `pinkytoe1_l` …, the joints counted from the ball |
| Helpers | keep the names their rig gave them |

The right side ends in `_r`. Centre bones carry no side. Numbers are not padded (`spine3`, not
`spine03`).

Why this grammar: the part comes first, so a sorted bone list keeps the two sides of a part
together; lowercase words with underscores read without a convention to learn and pass every file
format's identifier rules; the side as a short suffix keeps the names short and the two sides a
glance apart.

In the code, `RigRoles::standardName` renders a tag to its name and `RigRoles::canonicalTag` reads
it back — the standard's grammar first, then the reference generation's. The IK harness checks the
round trip on every bone of every reference rig, and the test tools accept the standard's names
beside the reference generation's.

**The names are applied on import** (2026-11-03): the FBX conformer's last step renames every bone
the standard names to that name and leaves the rest — the face rig, the helpers, a bone no role was
found for — as the file had them (a standard name a kept name already uses is not applied, and one
that two bones would carry — two riders of a side both read as a pectoral — is applied to neither;
the import log lists such collisions). The skin and the drivers bind by index, so nothing moves; what changes is what a pose file carries:
every conformed rig shares the standard's frames, rotation orders and ranges, so with its names a
pose saved on one imported character loads on every other. The native figures are never renamed —
their correctives, pose files and the content around them are keyed to their own names, which the
standard reads as its first convention. A **standard-named rig** (every bone the standard names
carrying that name, a name two bones share excepted) is also weighed **by role**: the balance masses come from each bone's role —
the class's share split among the bones of that class on that side, the spine's lower half the
abdomen's fraction and its upper half the chest's — where the name table, classing by fragments of
the reference's names, put every `spineN` in one class and gave a conformed rig a third of the
reference's trunk. The reference rigs carry their own names and keep the table, bit for bit.

## How a figure is conformed

The roles are **inferred from the bind skeleton's structure**, in this order. Each rule reads only
what the rules before it settled.

1. **The hips** are the solve root: the first bone below the figure node that branches. A bone is
   of the body if it descends from the hips.
2. **The legs** hang from the hips' children that have a foot-class joint below them, directly (a
   thigh each) or from one child carrying both (the pelvis bone). Down each leg's main chain
   (the child with the most mass below it) the twist helpers before the first real joint are
   thigh twists, that joint is the shin, the helpers after it shin twists, the first foot-class
   joint the foot. Leaf helpers beside the chain are the segment's twists too.
3. **Under the foot**, the first bone with two children or more is the ball (else the deepest
   bone with a child). The chain from the ankle to the ball is metatarsal. Each of the ball's
   children starts a toe; a childless, nameless stub among toe chains is no toe. A leaf under the
   ankle behind it is the heel, as is a leaf at the ankle that says so by its name.
4. **The spine** is the chain of the largest foot-free subtrees up from the hips, ending at the
   first link with side children of real size on both sides: the collars' link, the chest top.
5. **The collars** are the chest top's heaviest child on each side. Every other side child of a
   spine link is a pectoral (a skinless leaf, a helper). Down each arm's main chain: upper arm,
   its twists, forearm, its twists, hand, with the twist helpers as on the legs.
6. **Under the hand**, a skinless leaf is a helper (a prop anchor). A child that carries two
   chains or more, or whose own chain is four bones or longer, is a carpal and the chains below
   it fingers; any other child starts a finger.
7. **The neck and the head**: from the chest top's heaviest centre child, neck links while a
   bone has one child, the first with two or more the head (a chain that ends in a leaf ends in
   the head). Everything under the head is the face rig.
8. **What is left** of the body is a helper if it is a skinless leaf, else *other*, and listed.

Two things are read from a bone's **name**, where the geometry does not decide them: which digit
a finger or a toe is (the tokens thumb, index, mid, ring, pinky; big, index, mid, ring, pinky,
little), falling back to the digit's place across the hand or the foot when a rig has no such
tokens — except that **the thumb is the digit whose first joint sits nearest the wrist** where
one does by a fifth (a thumb's chain begins at the carpus, a finger's at the knuckle, a palm's
length out: a biped rig that names its digits Finger0 to Finger4 had its index finger's knuckle,
further forward than the thumb's base, taken for the thumb by place alone, and its first relaxed
hand curled the index sideways and folded the thumb like a finger); and the two face bones the
tests name, the eyes and the lower jaw.

The inference needs: the hierarchy, the bind positions, the solve root, which joints are
foot-class (low at bind), which bones are twist helpers, each subtree's mass, and the body mesh
sample (to tell a skinless helper from a joint). All of these the IK rig already has when it
builds; the roles are built with it and exposed as `IkRig::roles()`.

### The conformance report

`POSESTUDIO_RIG_ROLES_TRACE=1` prints the report when a rig is built: the chain lengths, the
missing required roles, then one line per bone of the body with its role, side, index and
joint. The IK harness runs the same check as a gated phase on every reference figure: every
required role found, every side role paired, and the roles in agreement with the names the test
tools speak.

### What reads the roles today

- The self-collision body volumes: the limb roots (the upper arms and the thighs), each limb's
  joints (the knee, the ankle, the elbow, the hand), the neck base and the head, the helpers to
  leave out, and the wider clearance of the hands and feet.
- The leaf twist helpers' token mass (a helper by structure, not by name).
- The test tools' bone names: a canonical name that matches neither a bone nor an alias resolves
  through the role it denotes (`bonealiases.cpp`), so a rig in an unknown naming convention runs
  the same harness and galleries.

The reference figures were the measure: with the roles in place of the name lookups, the eight
reference rigs produce the same poses to the last bit.

## Conforming a rig that carries no anatomy

A figure in its native scene format arrives with everything the posing engine lives by: a frame
on every bone, a rotation order, limits, locked twist helpers. A rig from an FBX file arrives
with none of it — names, a hierarchy, bind positions, skin weights, and whatever the authoring
tool's local axes happened to be. `scene/ik/rigconformer.{h,cpp}` turns the second kind into the
first, by role (2026-11-03). It runs inside the FBX import, on the raw skeleton, before the model
is uploaded; `POSESTUDIO_FBX_NO_CONFORM` is the A/B (the skeleton as read: identity frames, no
limits, which the engine poses as a static rig).

1. **A provisional armature**, enough structure for the roles: every frame along its bone, no
   limits — except that a bone whose name says *twist* or *roll* and carries nothing real
   below it (a neck named "NeckTwist" carries the head, and is a neck), and a bone that repeats
   its parent's segment word (a thigh cut into "thigh1" and "thigh2": the second piece), get
   two channels locked across the bone and the one along it free, so the rig reads them as
   twist helpers. The skin each bone dominates comes from the meshes' weights: a leaf no skin
   follows is an end marker, not a joint — the importer leaves those out of the skeleton
   altogether, keeping each one as its parent's **tip** (`ModelBone::tip`: the one thing a
   bone with no joint below it, a fingertip or a single-bone toe, does not say for itself).
2. **The roles**, as above, on the provisional rig.
3. **The limits**, per role, transcribed from the reference generation's authored ranges — a
   knee `-11..155` folding one way, an elbow `-135..20`, a thigh `-115..35` with `-26..85` of
   abduction, a collar's `-10..55` of shrug, a finger's three joints, the toes', the ball's,
   the neck's and each spine link's (a spine or a neck of any length is mapped onto the
   reference's chain by place, each link's limits interpolated at its relative place and then
   renormalized so the links together carry the reference chain's total range — three links
   that took links 1, 3 and 4 of the four had a third less flexion than the reference's trunk,
   and the bow rested short by that), the head's, the jaw's and the eyes'. The right side takes the left's with its side
   and twist channels mirrored. A segment's own twist channel is locked only when a twist
   helper of that segment lies **on the chain** between it and its next joint (the reference's
   thigh twist carries the shin); a helper hanging as a leaf beside the chain carries skin
   alone, and the segment keeps its twist. A helper is locked on every channel; a bone with no
   role is left free. The upper arm's elevation range is relative to the bind: a figure bound
   with its arms out horizontally keeps the same absolute reach as one bound at the reference's
   46 degrees. The foot's pitch is two joints' on the reference — the ankle (-45..65) and a
   metatarsal joint between it and the ball (-20..25), some 70 degrees of dorsiflexion in all,
   which is what a kneel spends: the shin leans forward over a planted, flat foot until the knee
   is nearly down. A foot with no metatarsal joint takes the metatarsal's range at the ankle
   (-70..85), or its kneel stops with the knees 10 cm off the floor.
4. **The frames**, by role, each the minimal rotation taking a world axis onto the bone's
   direction — X for the arms, hands and fingers, Z for the feet, toes and pectorals — and
   the direction itself by role: a chain bone points at its next joint through the helpers
   between (the thigh at the shin, the forearm at the hand), the hand at the mean of its
   fingers' bases, the ball at the mean of its toes', a leaf at its tip, a single-bone toe
   along its foot; a leaf twist helper takes its segment's frame; and a leaf **named twist**
   lies along its segment whatever its tip says (a biped export hangs an end marker off each
   twist bone that sits square to the arm, and read toward it no twist bone was one). **The
   thumb's roll is the palm's**: a finger's bend axis lies in the palm's plane, where the least
   rotation finds it on the binds seen, but a thumb lies out of that plane and flexes into it,
   and the reference generation authors its bend axis as the back-of-hand normal (the index and
   little fingers' bases about the wrist) rolled 28.5 degrees about the thumb's length, the
   same on every joint of the thumb and on four generations to a degree. **The trunk is
   world-aligned** — Y up, X lateral, Z forward on the hips, the pelvis, every spine link, the
   neck and the head, as the reference generation authors them whatever the spine's curve: the
   engine's trunk rules read flexion about the world's lateral axis. A frame along a leaning
   neck turned its flexion into a mix of flexion and roll; a ball framed at the first toe
   listed (the pinky on one side, the big toe on the other) stood 66 degrees off and mirrored
   25mm apart.
5. **The rotation order** per role, the reference's: `YZX` on the trunk and legs, `XYZ` on the
   arms, `XZY` on the forearm and thumb, `ZYX` on the feet and toes — the first letter the
   twist, the last the wide bend, as the engine composes them.
6. **The drivers.** A game-style rig keeps its correctives in the skeleton — twist helpers
   along a segment, "share" bones at a knee or an elbow — driven by expressions the file does
   not carry. Each helper gets a driver (`ModelBone::drivenBy`, re-derived from the driver's
   rotation after every pose change, never posed by hand or by a solve): a helper standing at a
   real joint takes half of that joint's rotation (counter-rotated by a half as the joint's
   child, a half as its sibling), and a leaf twist helper the share of its segment's twist that
   its place along the segment says, the skin twisting in proportion to its distance from the
   joint. A twist helper on the chain, carrying the next joint, is the solve's and gets none.
   And a rig authored for linear blend skinning is skinned linearly in the viewport
   (`MeshData::linearSkinning`): under dual quaternions its bends bulged on the outside.

The conformance report says how far it got (bones placed, chain lengths, twist helpers, finger
and toe joints, bones without a role, the missing required roles); the import log prints it and
`POSESTUDIO_RIG_ROLES_TRACE` the roles themselves.

Ten things the engine learned to read for such rigs, each bit-identical on the reference
figures: a driven bone (above); a bone's mirror twin by a **side token** anywhere in its name (`Thing_L_Part`,
`part.L`, `part_l`, `LeftPart`), then by role for what the names leave unpaired; a **helper**
bone by role that is no twist bone (a corrective bone beside a knee) is the flesh of the joint
it hangs from — a click on it selects that joint, as a click on a carpal selects the hand; an
idle arm at the **elevation it was bound at** is unposed (the reference binds at 46 degrees,
inside the hang's 60-degree cone; a game rig's A-pose may stand 63 off the vertical); the
**balance masses' side** is read off the same side tokens (the name table split by the first
letter alone, and a rig that puts the side last — `thigh_r`, `calf_twist_01_l` — had every thigh
a centre thigh, its share split six ways across both sides and their helpers: a leg weighed
3.5% of the body, a thigh socket was no girdle grab, and nothing dragged by the hips went
anywhere); a **helper is no riding part** of the joint it hangs off (a leaf twist helper beside
a shin carries the lower calf's skin to the ankle, and read as the knee's flesh it put the knee
"within the kneel band" 25cm up, where a game rig's kneel then planted both knees in mid-air
and stalled); a foot whose toes are **one childless bone** takes the standard's Ball role as its
ball when the structural search (a joint with children under the ankle) finds none; a **skinned
zero-length leaf at the hips** of a rig with no pelvis bone (a game-engine export re-parents the
thighs to the hips and leaves the pelvis bone as a leaf carrying the pelvis skin) is the Pelvis;
and where a rig calls its **hips "pelvis"** and has such a leaf, the canonical name "pelvis"
resolves to the Pelvis-role bone — the one collision of conventions the alias layer corrects by
role (every other literal name is right as read: the names are the standard's first convention,
the roles an inference, and letting the role win over any name reached the reference rigs, whose
third generation's big toe is its ball by role); and two rules of the Transform dials that the
reference's authored frames never needed: an even forward-bend channel that swings **sideways** (a
rib helper's, a palm bone's) takes the side-bend rule's sign — outward for a side's bone, her left
for a centre one — because on a conformed rig, whose two sides carry near-identical frames rather
than mirror images, the same local sign is an unmirrored turn (the Blender export's rib helpers
turned 10 degrees apart for the same dial); and a **zero-length leaf** at its parent's joint (a
rigging add-on's palm bones, each a leaf beside its finger) lies the way its parent's segment
lies, nothing of its own saying which way is distal.

And five more from a biped rig exported by an animation package (2026-11-03: its digits named
only by number, its twist bones with end markers of their own, its chest one long bone on a
two-link spine, its toe bone bound 11cm below a sole that rests on the floor), each bit-identical
on the reference figures but the two noted: **the top spine link is never the girdle** — the
chest top the collars and the neck hang from carries the arms and the head, and is a chest
however near the hips a short spine puts it (two bones from the root and 18cm up, it met the
girdle rule, and every grab of the chest was a hips drag: the trunk never bent, a bow was a walk
and the pinned seat moved under a lean); **the bone classes follow the roles** where the
structural rules and the roles disagree — a Finger or a Toe is a digit, a carpal, a metatarsal,
the heel, or the ball the toes fan from, is the hand's or the foot's own — while a ball with
no toe below it, the one-bone toe block of a game rig's, a rigging add-on's or a biped's foot,
IS the toes and keeps what the structure read, a digit that curls (the foot model's ball is a
role, not a class); on two reference generations the rule moves two foot bones each from digit
to the foot's own — the oldest generation's toes block, its ball by role with the big toe below
it, and the third generation's nameless mid-foot stub — so the oldest generation's big toe
dragged no longer bends the block with it;
**a foot-class joint bound below the floor keeps its bind height** as its clearance (clamped at
the floor, the ball's clearance stood 11cm over the joint and every floor reading of the rig
began 111mm through it); **the root is the seat** where the rig has no pelvis bone (the
buttocks' flesh is the root's own there; excluded from the contacts, a figure sat on the floor
had no seat, and reclined by the chest she leaned on nothing); and **a finger's Bend dial is its
long side**, the flexion, on every rig (the geometric rule reads the way a joint carries its
limb forward or down, which a finger hanging in an A-pose does neither: a thumb's dial extended
the thumb on one conformed rig, and the main family's knuckle read extension for the day the
sideways-swing rule above stood unqualified — it is for even channels only now).

## What the standard will grow to cover

These are the next steps, in order.

1. **Channel semantics per role.** Which rotation channel of each joint is its flexion, its side
   bend and its twist, and which direction is forward, outward and pointed. The Transform tab's
   dials already derive this by geometry for every joint, and the conformer's limits assume the
   reference's channel layout; the standard will make it a definition.
2. **The rest of the engine's name lookups**: the balance masses — their SIDE is read by role-
   grade tokens now, their CLASS still by name fragments, so a game-engine rig's spine weighs a
   third of the reference's trunk (`spine_01..03` share the one "spine" entry where the reference's
   four links share "abdomen" and "chest") and its bow rests 4cm short of a cursor the reference
   reaches: the masses by role are the next round — and the harness alias table itself.
3. **The standard's names in the test tools.** The names are decided (see *The standard's
   names*); the harness and the galleries still speak the reference generation's names and will
   move to the standard's.
4. **A rig setup panel** for figures the inference cannot place: the roles against the bones,
   prefilled, with a picker per role, and the humanoid limits applied on request. A figure that
   is not humanoid, or not mapped, still poses joint by joint with the dials and the mouse mode;
   the IK is off for it.
5. **Poses by role.** A pose saved by role loads onto any conforming figure.
