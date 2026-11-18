# Navigating the Viewport

The viewport has an orbit camera: it looks at a target point and turns around it. The middle button pans and the wheel zooms. The left button orbits when the press lands on empty space or on an object that is not a figure; a left press on a figure poses her instead (see [Posing](posing.md)), and one on a light's marker moves the light (see [Lights](lights.md#lights-in-the-viewport)).

## Mouse

| Gesture | What it does |
| --- | --- |
| **Left-drag** on empty space, or on an object that is not a figure | Orbits, as if you were grabbing the scene: drag right and it turns right; drag down and it tips toward you, so you look from higher up; drag up and you look from lower down. |
| **Middle-drag** | Pans: the view slides with the mouse. |
| **Mouse wheel** | Zooms in and out (dollies toward and away from the target). |
| **Left-click** (the pointer stays within a few pixels) | Selects the model under the cursor, or clears the selection on empty space. See [Selecting objects](#selecting-objects). |
| **Right-click** on a model or joint | Opens the context menu (Delete, and the joint tools). |

A left-drag that starts on a **figure** does not orbit — it poses the part you pressed on. If you want to orbit around a figure, start the drag beside it; a click on the figure that never moves selects it, and the joint under the pointer.

The view starts to turn once the pointer has moved a few pixels from where you pressed, and then catches up with the whole movement. A click made with a slightly unsteady hand therefore turns nothing, and a Front, Side or Top view stays that view. Once the view does turn, any named view becomes a free **Perspective View**.

## The camera views

Six axis-aligned views and three utilities are on the View menu, on the strip's view picker, and on the keyboard. The keys follow the convention most 3D applications use on the numeric keypad, and they also work on the number row.

| View | Key | Notes |
| --- | --- | --- |
| Front View | `1` | Looks from the front — the side a freshly imported figure faces. |
| Back View | `Ctrl+1` | |
| Right View | `3` | Looks from the right as you face the figure from the front, so it shows her left side — as in every 3D application. |
| Left View | `Ctrl+3` | |
| Top View | `7` | From above, the figure's front at the bottom of the screen. |
| Bottom View | `Ctrl+7` | From below, mirrored so the front is still at the bottom. |
| Flip View | `9` | Turns the camera 180° about the vertical axis: front becomes back, left becomes right. |
| Home View | `5` | The default three-quarter perspective framing — the same as the strip's Home button. |
| Frame Selected | `.` (period) | Re-aims at the selected model and zooms so it fills the view. With nothing selected, frames everything. |

The keys work whichever panel has focus, except while you are typing in a text field. With NumLock off, the keypad's `End`, `PageDown`, `Home`, `PageUp`, `Clear` and `Del` keys stand in for `1`, `3`, `7`, `9`, `5` and `.`.

**The axis views are orthographic**: a true elevation or plan drawing with no perspective, so proportions can be compared and a side view shows the floor as a single line. The framing at the target stays the same when you switch, and the wheel still zooms. Orbiting switches back to perspective — an axis view is a locked drawing only until you turn it. The view picker on the strip always names the view you are in.

## Selecting objects

A **click** (press and release without dragging) selects the model under the cursor and outlines it in blue; a click on empty space — or `Esc` — clears the selection. The click takes **what is under the cursor**: a figure's or a prop's surface, or a Gaussian splat scene wherever its splats cover that spot — so with two objects close together, or one in front of the other, the one you see under the cursor is the one selected. Where the cursor is on nothing at all, the nearest object whose extent it falls within is taken, so a click just beside a lone model still selects it. Once the view has begun to turn the gesture is an orbit, and an orbit never changes the selection — not even one that ends where it began.

The selection matters for:

- **Frame Selected** (`.`).
- **Delete** — `Delete` or `Backspace` while the viewport has focus, **Edit → Delete Selected Object**, or the context menu's *Delete*. Deleting a model clears the undo history.
- **Posing** — clicking a joint selects that joint and makes its figure the active one; clearing the selection also drops the joint selection.

A **light** is selected by a click on its marker, and a light and a model are never selected together: `Delete` then removes the light, and `Esc` or a click on empty space deselects it. See [Lights](lights.md#lights-in-the-viewport).

An imported object arrives unselected; the selection stays as it was. To give the viewport keyboard focus (for `Delete` and `P`), click in it once.

## The floor and the grid

The floor is an infinite grid at height zero; its lines fade with distance and a soft shadow of the figure falls on it in the PBR shading mode. Figures import standing on it, and the [Ground button](posing.md#the-ground-button) puts a posed figure back on it. In the orthographic side views the floor appears as a single line.
