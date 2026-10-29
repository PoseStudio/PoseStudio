# Posing

Posing in PoseStudio is direct: you grab a part of the body and move it, and the whole figure answers the way a person's body would. There is no gizmo to find and no chain to set up. This page explains the controls and what the body does with them; [Poses Step by Step](poses.md) shows how to make particular poses, and [Pose Files](pose-files.md) how to save them.

## Grabbing a joint

Move the pointer over the figure and **press the left button on the part you want to move**. PoseStudio picks the skin under the pointer and the joint that moves that skin — the wrist when you click on a hand, the knee or the hip when you click along the thigh — and tints the flesh that joint drives blue, so you always see what you have hold of. The point under your pointer is what you are holding: drag, and that point follows the cursor.

- What you see is what you get: the click lands on the **surface in front**, so a part hidden behind another is never selected through it. Clicking the chest takes the chest, not the shoulder or the arm behind it; to grab the far arm, orbit until you can see it.
- Clicking a **joint** itself (a knee, an elbow, a wrist) takes hold of that joint.
- Clicking **along a limb** takes hold of the point you clicked: the far end of the limb segment when you click nearer it (the knee for a thigh, the hand for a forearm), the near end otherwise.
- A click on the **hips** takes hold of the pelvis; a click on the **chest** or the **head** takes hold of the trunk there.
- A click anywhere on the **face** takes hold of the **head**, and the whole head lights up. The face's own bones — the eyes, the jaw, the lips, the brows, the ears — are there for expressions and are not posed: nothing on the face is selected by itself, and no drag turns one.
- A click anywhere on the **palm** or the **back of the hand** takes hold of the **hand**, and the whole hand lights up — the small bones inside the hand are its own flesh, not joints to pose; the same for the **foot** below the ankle (the ball, the heel, the mid-foot).
- A click on a **finger** or a **toe** takes hold of that digit, at the point you clicked. See [Fingers and toes](#fingers-and-toes).
- Joints stay clickable whether the [skeleton overlay](shading.md#the-skeleton-overlay) is on or off. Turn it on to see exactly where they are. A click just outside the body still takes the nearest joint, as long as nothing is in front of it.

A left press that finds no joint orbits the camera instead, and a plain click selects or deselects the model.

## Dragging: full-body IK

**Left-drag a joint** and the body follows. The grabbed point tracks the cursor in the plane of the screen, and everything else is worked out for you, every frame:

- **The feet stay planted.** Standing feet are anchored to the floor — the ball of the foot holds hard, the heel may lift when the ankle runs out, and the toes stay flat. Nothing you drag slides a planted foot.
- **The body balances.** The weight stays over the feet: pull a hand forward and she leans; pull it further and the whole body leans as far as it can and then stops.
- **She steps when she must.** Take the hips, the chest or the head far enough across the floor and the feet step under the body, one at a time, each with a swing and a landing. A step in flight always finishes when you let go.
- **She crouches, kneels, sits and lies down.** Push the hips down and she crouches; take them down and forward and she kneels, the knees landing on the floor and the feet lying flat on their tops behind her, toes back (and the knees staying there when the chest is then pulled forward past her reach: she goes down onto her hands rather than up into a plank); down and back and she sits. Knees, hands, the seat and the head all become supports when they reach the floor and let go again when the body rises off them. See [Poses Step by Step](poses.md).
- **Joints stay within their limits.** Every joint has the anatomical range its figure was authored with — a knee bends one way, a neck turns so far — and a drag never takes a joint past it.
- **Limbs stay out of the body and out of each other.** Arms cross the chest without passing through it, a hand laid on the other forearm rests on it, crossed legs stay apart.
- **Reaching high, and lifting her off the floor.** Pull a hand up past the arm's reach and she rises onto tiptoe — heels up, body tall, the shoulder shrugging no further than a shoulder does; bring it back within reach and the heels come down. Pull steadily further up and, after a moment, she lifts off and hangs from it; lower her and she lands and stands back up.
- **A hand behind the head** is made the way you would make it: drag the hand up beside the head, then roll the mouse wheel a notch at a time to push it back. Dragged there in one diagonal motion the arm may swing back first and come round late; the path up and then back gives the pose cleanly.
- **A hand laid on the body lies on its palm.** Bring a hand to the hip, the chest, the top of the head, the belly or a knee and hold it there: it turns to lie flat, palm to the body, the way a hand put on the floor does. Brought to the small of the back it rests on the back of the hand instead — the forearm cannot turn the palm round that far. The hand settles only where it comes to rest; dragged past the body it keeps its angle. A hand that leaves the surface keeps the turn it took there; Reset Joint or the Transform tab's Twist dial straightens it.

The response is deliberately smooth: the figure follows the cursor with a small lag rather than snapping, and a still cursor gives a perfectly still figure. Releasing the button ends the drag; if a step or a landing was still in flight, it completes on its own, and then the drag becomes one entry in the undo history.

### Moving toward and away from the camera

The cursor moves the grabbed point across the screen. To move it **in depth** — toward or away from you — **roll the mouse wheel while the button is held**: scrolling up pushes the point away from the camera, scrolling down pulls it toward you, in steps of about 2% of its distance from the camera. The point stays under the pointer as its depth changes. Orbit to a side view to check the depth you reached.

### Special joints

Some joints have their own way of being dragged, because that is what a person means by moving them:

- **A knee** dragged swings over its planted foot — out, in, forward, back — while the foot pivots on its heel. Pull it up past what the shin allows and the foot lets go, trails, and swings in under the knee; bring it back down and it returns to its spot.
- **Lifting a foot shifts the weight.** As a foot leaves the floor — dragged up itself, or let go of by a raised knee — the hips move over the foot she stands on, the standing leg angles in under her, and the pelvis drops a little on the free side with the chest kept level, so a knee raise, a kick or a foot put up on a step is a stance she could hold. Bring the foot back down and the hips come back. A foot that was already in the air when you took hold of something shifts nothing.
- **Idle arms swing with a stride.** Arms hanging at her sides swing against the legs as the feet step apart or a foot is slid forward — forward on the side of the trailing foot, back on the side of the leading one, as a walker's do, the forward arm's elbow bending a little — and come back as the stance closes. The swing fades as the hips come down, so a lunge keeps its arms at her sides. Arms you have posed are left alone.
- **Idle hands relax.** The fingers of an arm that hangs curl gently into a relaxed hand as you pose, and a hand coming down onto the floor opens again. Fingers you have turned yourself are left as you set them; Reset Pose flattens them.
- **Figures that import in a T-pose** (arms straight out to the sides) keep their arms out while you pose the rest: arms held out are posed arms, and only arms hanging at her sides follow the body as it leans, bows and crouches. Drag each hand down to her side first (or turn the shoulders with the Transform tab's dials), and the arms hang and move as they do on figures that import with their arms down.
- **A hand** taken down folds the trunk at the hips as it goes — a bow over softened knees to bring a hand to the knee, a fold at the hips and the knees to reach the floor — the hips going back to keep her balance and the head looking down where the hand goes. Bring the hand back up and she stands up with it.
- **An elbow** dragged swivels about a hand that stays where it is (elbows out, elbows in, an arm tucked); a straight arm's elbow carries the arm.
- **A foot** on the floor dragged along it slides: the hips come between the feet and the sole stays flat. A foot lifted in the air is placed by its leg, the weight going over the other foot; set down from a tilt, its sole rolls flat as it comes down.
- **The hips** dragged sideways sway — the pelvis rolls and the chest stays level; dragged back they hinge so the head comes forward over the feet; dragged across the floor they walk.
- **The head** dragged takes the body with it through the neck — it leans, steps, bows and crouches. To *turn* the head, rotate it instead (below).

### Fingers and toes

**A finger or a toe dragged moves that one digit and nothing else.** Every joint of the digit bends toward the cursor, up to where the digit joins the hand or the foot; the hand, the foot and the whole body stay exactly where they are. It makes no difference whether `Ctrl` is held.

- Take hold of a **fingertip** to curl or straighten the finger; take hold nearer the knuckle to swing the finger from the knuckle.
- A click **selects the joint whose segment you clicked**, and that segment alone lights up: the base, the middle or the tip of a finger, the base or the tip of a toe. The Transform tab names it.
- Pulled beyond its reach, the digit points after the cursor as far as its joints allow and rests there. The joint limits and the floor still hold.
- To move the **hand** or the **foot** itself, take hold of it, not of a digit: the back of the hand, the palm or the wrist; the instep, the heel or the ankle.
- To turn a single finger joint, select it and press `B` (or `S`, `T`) and move the mouse, or use the Transform tab (both below).
- Zoom in before posing fingers. From a view of the whole figure a finger is a few pixels wide, and it is easy to take hold of the hand instead.

## Posing one part: Ctrl + drag

Sometimes you want to move one part and leave the rest exactly as it is. **Ctrl + left-drag** a limb, the head or the torso and only the chain it belongs to follows the cursor:

- **A hand, elbow, foot or knee**: the limb moves, up to where it joins the body — the arm with its collar bone, the leg from the hip. The torso, the head and the other limbs stay where they were. A knee dragged this way swings over its planted foot, as it does in a plain drag.
- **The head or the upper neck**: the neck bends toward the cursor; the chest and everything below stay.
- **The chest, the upper chest, the base of the neck, the collar bones or the belly**: the torso bends — the spine from the grabbed joint down to the pelvis, as one — and stops there. The pelvis and the legs stay exactly where they are; the arms and the head ride the chest. Use it to lean, twist or arch the upper body on a stance you have already set.
- **A finger or a toe**: that digit alone, as in a plain drag ([Fingers and toes](#fingers-and-toes)).

Nothing else happens in this mode: no balance, no step, no lift-off, and the arms and head are not re-posed for you. The joint limits, the floor and the body still hold, so the chain stops where it would collide. Pulled beyond the chain's reach, the part points toward the cursor and rests there. A joint you pinned below the chain holds: pin a hand and Ctrl-drag the chest, and the hand stays where it is while the arm folds or straightens to keep it there — when the arm runs out, the pin stops the chest. Each Ctrl-drag is one undo step, and the wheel's depth control works during it as in a plain drag.

## Moving the whole figure: Ctrl + drag the hips

**Ctrl + left-drag the hips** and the whole figure follows the cursor exactly as she is posed. Nothing is anchored to the floor: her feet, or a knee or a hand she has on the floor, come along. No joint turns, she does not balance or step. Use it to lift a posed figure, carry her across the scene, or set her down somewhere else.

- **Up and across**: she goes where the cursor goes; roll the wheel while dragging to move her toward or away from the camera.
- **Down**: the floor stops her. She comes down until the lowest part of her pose rests on it and no further.
- **Your pins hold.** A joint you pinned stays exactly where you pinned it, and its limb gives so that it can: pin a hand and move the body, and the arm bends or straightens to keep the hand in place. When the limb has no more to give, the pin stops the body there. A pin on the hips or the torso holds the whole figure where she is.
- A figure left in the air stays in the air. Press the [Ground button](#the-ground-button) to drop her onto the floor.
- The move is one undo step, like any drag.

To pose the torso with the whole body answering — lean, bow, crouch, walk — drag it without `Ctrl`; with `Ctrl` the torso bends alone over a still pelvis (above).

## Moving several joints together: Shift

Some poses are made with two hands at once — hands on a bar, a push against a wall, both arms raised — or with a hand and a foot together. **Shift + click** a joint to add it to the selection (Shift + click it again to take it out), then **Shift + drag** any of the selected joints and they all move together, each by the same amount, through the joint you pressed. The body answers exactly as it does to one joint: the feet stay planted, she balances, steps when she must, folds or crouches under a push, and rises onto tiptoe under a pull beyond reach.

- The selected joints light up together. The one you press is the one the drag plane passes through and the one the body's policy is read from; the others keep their offset from it.
- Both hands raised rise together with the body standing; both hands pushed forward lean her and walk her; both hands brought down fold her at the hips; both knees pushed forward swing over their planted feet.
- A plain click on any joint, or on empty space, selects one joint alone again. A Shift + click on a finger or a toe selects that digit alone (digits are posed on their own), and a click on the face stands for the head.
- `Ctrl` + drag is not a group gesture: it moves the pressed joint's chain, or (by the hips) the figure, as described above.
- A group drag is one undo step, and the wheel's depth control works during it.

## Turning a joint with the mouse: B, S, T

To turn one joint and nothing else — fold a knee, tilt the head, twist a forearm — select it (click it), then press one of three keys. They work whichever panel you last clicked in, and they are also on the menu, under **Edit → Turn Joint with Mouse**:

| Key | Turns |
| --- | --- |
| `B` | **Bend**: the joint's natural direction. A knee or an elbow folds, a finger curls, the spine and the head bend forward. |
| `S` | **Side-Side**: its secondary direction. A thigh moves out to the side, an arm is raised or lowered, the head tilts. |
| `T` | **Twist**: the turn about the joint's own length. A forearm or a thigh rotates, the head turns. |

These are the same three motions as the dials of the [Transform tab](#the-transform-tab). While the mode is on, a badge under the viewport strip names it, the pointer becomes a four-way arrow, and **moving the mouse turns the joint — no button held**, wherever in the PoseStudio window the pointer is:

- **Move the mouse the way the limb should go.** The limb's far end follows the pointer's movement: with a knee selected in a side view, moving the mouse toward her back folds the shin back.
- For a **twist**, and for a bend that goes straight toward or away from you in the current view, move the mouse **right for more and left for less**.
- **Left click** (or `Enter`) keeps the result. The click belongs to the mode: whatever is under the pointer is not clicked.
- **`Esc`**, a **right click**, or the same key again puts the joint back as it was.
- Pressing another of the three keys drops what the running one did and starts that one. To combine two, click to keep the first, then press the next key.
- The mouse wheel still zooms while the mode is on, and the mode is not available in the middle of a drag.

Rotations stop at the joint's limits, and a rotation that would push a limb into the body stops at the body. Only the one joint turns: no balance, no planted feet (press the [Ground button](#the-ground-button) afterwards if the feet left the floor). Each kept turn is one undo step. The keys are live only while the selected joint has that motion (an elbow has no Side-Side), and a text field you are typing in keeps its letters.

## The Transform tab

The **Transform** tab on the side panel shows the selected joint's rotation as three dials, and lets you set it by number. Click a joint on the figure; its name appears at the top of the tab with:

| Dial | What it turns |
| --- | --- |
| **Bend** | The joint's natural direction: a knee or an elbow folds, a finger curls, the spine, the neck and the head bend forward, a thigh or an upper arm swings forward. |
| **Side-Side** | Its secondary direction: a thigh moves out to the side, an arm is raised or lowered, the spine leans to one side. |
| **Twist** | The turn about the joint's own length: a forearm or a thigh rotates, the head turns. |

Every dial reads on the same scale, whatever the joint:

- **0** is the rest pose — the joint as the figure was imported.
- **100** is fully bent: the joint's limit in that direction.
- **Negative values** bend it the other way, in the same proportion, and stop where the joint's other limit is. A knee that folds a long way and straightens only a little past straight reads from about -7 to 100; a joint that turns equally far both ways reads from -100 to 100.

The dials are scrub fields like the ones on the Environment tab: drag left or right across one to change it, or click it once to type a number (`Enter` confirms, `Esc` cancels). The bar fills from the rest pose toward the value, to the right for a positive value and to the left for a negative one. The small button beside a dial returns that one dial to 0, and **Reset All** returns all three.

- The figure moves as you scrub. A dial turns the one joint and nothing else follows: no balance, no planted feet. Press the [Ground button](#the-ground-button) afterwards if the feet left the floor.
- A dial stops at the joint's limits, and at the body: a rotation that would push a limb into the torso or another limb stops there, and the dial shows where it stopped.
- A dial the joint does not have is greyed out. An elbow and the end joints of the fingers only fold, so their Side-Side is disabled.
- On a left and a right joint the same value gives the mirrored pose: 50 on both thighs' Side-Side moves both legs outward.
- The dials follow the figure. Drag a joint in the viewport, turn it with `B` / `S` / `T`, undo, or load a pose, and they show the selected joint's new rotation.
- Each scrub, each typed value and each reset is one undo step.

## Joint pins

A **pin** holds a joint exactly where it is through every later drag: a hand kept on a hip, a foot that must stay on its spot, hips held on a chair.

- Select a joint (click it) and press **`P`** to pin it; press `P` again to unpin. The context menu (right-click a joint) offers **Pin Joint**, **Unpin Joint** and **Unpin All Joints**.
- Pinned joints show an **orange marker**. While you drag, the feet the body is standing on show smaller **cyan** markers — those are the drag's own contacts, not pins.
- A pin holds both the joint's place and its orientation, to microns, under any drag of another joint. Dragging the pinned joint itself moves it.
- **Pinning the hips makes a seat.** PoseStudio knows nothing about chairs, so this is how you tell it that something holds her up: sit her down by the hips, pin them, and pose the rest — balance and stepping stand down, and the pelvis rocks under a chest lean like a seated person's.
- Pins are part of the pose: they are saved in [pose files](pose-files.md), and undoing a drag restores the pins of that moment.

## The Ground button

A pose that bends the knees lifts the feet off the floor, because the hips stayed where they were. The **Ground** button on the viewport strip drops the active figure so the lowest point of its current pose rests on the floor — animated as a real fall (a metre takes about half a second). A figure sunk below the floor is lifted out at once. Grounding is refused while you are dragging. It is a pose edit like any other: one undo step (`Ctrl+Z` puts her back where she was), saved with the pose in a [pose file](pose-files.md), and taken back by **Reset Pose** along with everything else.

**She lands like a person.** A figure that comes down on her feet absorbs the landing: her knees fold, her hips dip over her planted feet, and she comes back up — all in under half a second. The higher the drop, the deeper the dip: a few centimetres' drop barely bends the knees, a drop from a metre or more is a real knee bend. It is for the eye only. When it is over she stands in exactly the pose you dropped her in: the dip itself leaves no trace, and undoing the drop is the one undo step.

- There is no bounce when she lands on anything but her feet — her knees, her hands, her seat — or when her hips are [pinned](#joint-pins).
- Clicking, pressing a key or starting any edit while she is still falling or dipping finishes it at once.

Figures import standing on the floor, so you rarely need this after an IK drag: planted feet keep the figure on the ground.

## Reset and mirror

The **Edit** menu and the joint context menu offer five utilities, each one undo step:

| Utility | What it does |
| --- | --- |
| **Reset Selected Joint** | Returns the selected joint to its rest rotation. |
| **Reset Limb** | Returns the selected joint and everything below it. |
| **Reset Pose** | Returns the whole figure to its rest pose — the figure as it was imported. |
| **Mirror Pose** | Swaps the body's left and right sides: a pose that leans left leans right. |
| **Mirror Limb to Other Side** | Copies the selected joint's pose, and everything below it, to the matching joint on the other side — pose one arm, then mirror it to the other. From a centre joint such as the chest it mirrors both sides. |

Pins are left alone by all five.

## Undo and redo

**Edit → Undo** (`Ctrl+Z`) and **Redo** (`Ctrl+Y`) walk one history of everything you changed: each drag, each rotation, each dial change on the Transform tab, each pin toggle, each utility, each drop to the ground and each pose file loaded — and each lighting change on the Environment tab. An entry is recorded when a gesture ends, so a long drag is one step. Deleting an object clears the history.

## Several figures

You can import as many figures as you like. Posing addresses the **active figure** — the one whose joint you clicked last — and the utilities, the pins, the pose files and the Ground button all act on it. Click a joint on another figure to make that one active. While a model that is not a figure (an `.obj`) is the selection, the posing utilities do nothing.

## What the figure knows

Everything the body does comes from the figure you imported, not from PoseStudio's guesses:

- **Joint limits** are the ranges authored in the figure. Locked channels (a twist bone's bending axes) never move.
- **Correctives** — the sculpted shapes that fix a deep elbow or knee bend — blend live while you drag, so the mesh looks right at every frame.
- **Body proportions** set every threshold: a small character crouches, steps and kneels at her own scale.

## Tips

- Orbit to a **side view** (`3`) before pushing a figure down onto the floor or forward onto her hands — you can see the depth, and the drag plane is the one you want.
- To move a whole figure, drag the hips: they walk her across the floor. To move her without changing her pose, [Ctrl + drag the hips](#moving-the-whole-figure-ctrl--drag-the-hips).
- If a drag does not go where you expect, look at where the feet are: a planted foot is an anchor, and a pull the body cannot follow without stepping ends in a lean. Take the hips there instead, or lift a foot first.
- A figure left kneeling, sitting or lying starts the next drag from that pose — pull the hips or the chest up to get her up again (a kneeler's feet roll back onto their toes as she rises, and her knees come up with them).
- Reset Pose is always one step away when a pose gets away from you.
