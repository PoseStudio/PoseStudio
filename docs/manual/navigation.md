# Navigating the Viewport

The viewport has an orbit camera: it looks at a target point and turns around it. Every navigation gesture that does not land on a figure's joint moves the camera; gestures on joints pose the figure instead (see [Posing](posing.md)).

## Mouse

| Gesture | What it does |
| --- | --- |
| **Left-drag** on empty space or on a model's body | Orbits: drag right to turn right, drag up to tilt the view up — as if you were grabbing the scene. |
| **Middle-drag** | Pans: the view slides with the mouse. |
| **Mouse wheel** | Zooms in and out (dollies toward and away from the target). |
| **Left-click** (no movement) | Selects the model under the cursor, or clears the selection on empty space. See [Selecting objects](#selecting-objects). |
| **Right-click** on a model or joint | Opens the context menu (Delete, and the joint tools). |

A left-drag that starts on a **joint** does not orbit — it poses. If you want to orbit around a figure, start the drag beside it or on a part of the body away from a joint; a click on the body that never moves still selects the model.

Orbiting even a little turns any named view into a free **Perspective View**.

## The camera views

Six axis-aligned views and three utilities are on the View menu, on the strip's view picker, and on the keyboard. The keys follow the convention most 3D applications use on the numeric keypad, and they also work on the number row.

| View | Key | Notes |
| --- | --- | --- |
| Front View | `1` | Looks from the front — the side a freshly imported figure faces. |
| Back View | `Ctrl+1` | |
| Right View | `3` | Looks from the figure's right, showing her left side — as in every 3D application. |
| Left View | `Ctrl+3` | |
| Top View | `7` | From above, the figure's front at the bottom of the screen. |
| Bottom View | `Ctrl+7` | From below, mirrored so the front is still at the bottom. |
| Flip View | `9` | Turns the camera 180° about the vertical axis: front becomes back, left becomes right. |
| Home View | `5` | The default three-quarter perspective framing — the same as the strip's Home button. |
| Frame Selected | `.` (period) | Re-aims at the selected model and zooms so it fills the view. With nothing selected, frames everything. |

The keys work whichever panel has focus, except while you are typing in a text field. With NumLock off, the keypad's `End`, `PageDown`, `Home`, `PageUp`, `Clear` and `Del` keys stand in for `1`, `3`, `7`, `9`, `5` and `.`.

**The axis views are orthographic**: a true elevation or plan drawing with no perspective, so proportions can be compared and a side view shows the floor as a single line. The framing at the target stays the same when you switch, and the wheel still zooms. Orbiting switches back to perspective — an axis view is a locked drawing only until you turn it. The view picker on the strip always names the view you are in.

## Selecting objects

A **click** (press and release without dragging) selects the model under the cursor and outlines it in blue; a click on empty space — or `Esc` — clears the selection. Selecting works on the model's bounding box, so a click just outside the mesh but within its extent still selects it. An orbit drag, however short, never changes the selection.

The selection matters for:

- **Frame Selected** (`.`).
- **Delete** — `Delete` or `Backspace` while the viewport has focus, **Edit → Delete Selected Object**, or the context menu's *Delete*. Deleting a model clears the undo history.
- **Posing** — clicking a joint selects that joint and makes its figure the active one; clearing the selection also drops the joint selection.

Each import selects the new object. To give the viewport keyboard focus (for `Delete`, `P` and the `X`/`Y`/`Z` keys), click in it once.

## The floor and the grid

The floor is an infinite grid at height zero; its lines fade with distance and a soft shadow of the figure falls on it in the PBR shading mode. Figures import standing on it, and the [Ground button](posing.md#the-ground-button) puts a posed figure back on it. In the orthographic side views the floor appears as a single line.
