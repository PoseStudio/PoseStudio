# Keyboard and Mouse Reference

Every control in one place. "Viewport focus" means you have clicked in the 3D view since you last used another panel; the other shortcuts work whichever panel has focus (a text field you are typing in keeps its own keys).

## Mouse in the viewport

| Gesture | Where | What it does |
| --- | --- | --- |
| Left-drag | on a joint | Full-body IK: the point you grabbed follows the cursor, the body follows anatomically. |
| Left-drag | on a finger or a toe | That digit alone bends toward the cursor; the hand, the foot and the body stay (with or without Ctrl). |
| Left-drag | on empty space or a model's body | Orbit the camera. |
| Ctrl + left-drag | on a limb, the head or the torso | Scoped IK: only the chain the joint belongs to follows — a limb up to the body, the head and neck, the torso down to the pelvis. Nothing else moves; pinned joints below the chain stay where they are pinned. |
| Ctrl + left-drag | on the hips | Moves the whole figure as she is posed: nothing is anchored to the floor, no joint turns. Pinned joints stay where they are pinned. |
| Left-click | on a model | Select it (blue outline); on empty space, deselect. |
| Left-click | on a joint | Select the joint (and make its figure the active one). A click on the face selects the head; a click on the palm or the back of the hand selects the hand. |
| Shift + left-click | on a joint | Add the joint to the selection, or take a selected joint out of it again. |
| Shift + left-drag | on a selected joint | Move all the selected joints together, by the same amount; the body follows as in a plain drag. |
| Middle-drag | anywhere | Pan. |
| Wheel | normally | Zoom. |
| Wheel | during a left-drag of a joint | Move the grabbed point toward (scroll down) or away from (scroll up) the camera. |
| Right-click | on a model or joint | The context menu: pin / unpin, reset joint or limb, mirror limb, delete. |

## Camera

| Key | Action |
| --- | --- |
| `1` / `Ctrl+1` | Front / Back view |
| `3` / `Ctrl+3` | Right / Left view |
| `7` / `Ctrl+7` | Top / Bottom view |
| `9` | Flip the view 180° |
| `5` | Home view (the default framing) |
| `.` | Frame the selected model |

The keys work on the number row and on the numeric keypad. With NumLock off, the keypad's `End` / `PageDown` / `Home` / `PageUp` / `Clear` (5) / `Del` stand in for `1` / `3` / `7` / `9` / `5` / `.`. The axis views are orthographic; orbiting returns to perspective.

## Posing

| Key | Needs | Action |
| --- | --- | --- |
| `P` | viewport focus, a joint selected | Pin or unpin the selected joint |
| `B` | a joint selected | Bend the joint with the mouse: move to turn it, click to keep |
| `S` | a joint selected | The same for Side-Side |
| `T` | a joint selected | The same for Twist |
| Left click or `Enter` | while `B` / `S` / `T` is on | Keep the result |
| `Esc`, right click, or the same key again | while `B` / `S` / `T` is on | Put the joint back as it was |
| `Esc` | in VR Mode (anywhere in the window) | Leave VR Mode — see [VR Mode](vr.md) |
| `Esc` | otherwise, anywhere in the window | Deselect whatever is selected — the model and its joint |
| `Delete` or `Backspace` | viewport focus, a model selected | Delete the selected model (clears the undo history) |
| `Ctrl+Z` | | Undo |
| `Ctrl+Y` | | Redo |

`B`, `S` and `T` work whichever panel has the keyboard focus (they are the keys of **Edit → Turn Joint with Mouse**); `P` and `Delete` need the viewport to have it. Edit → Reset Selected Joint, Reset Limb, Reset Pose, Mirror Pose and Mirror Limb to Other Side have no keys; they are on the Edit menu and (the per-joint ones) on the joint's context menu.

## Application

| Key | Action |
| --- | --- |
| `F1` | Open this manual |
| `Ctrl+N` | File → New — reset the scene to a fresh launch |
| `Ctrl+O` | File → Open — load a `.pss` project |
| `Ctrl+S` | File → Save — save the scene to its `.pss` file (asks for a place first if it has none) |
| `Ctrl+Shift+S` | File → Save As… — save the scene under a new `.pss` name |
| `Ctrl+Q` | Quit |

## Asset Manager

| Key | Action |
| --- | --- |
| `Enter` in the search field | Search |
| `F2` on a collection | Rename it |
| Double-click an asset | Import it (`.obj`, `.duf`, `.dsf`) or open it with its application |
| Double-click a folder | Open the folder |
| Drag a thumbnail | Reorder (in Favorites and Collections), or drop onto Favorites / a Collection in the tree |

## Environment tab

| Gesture | Action |
| --- | --- |
| Drag across a value | Scrub it |
| Click a value | Type it; `Enter` confirms, `Esc` cancels |
| The small button beside a row | Restore that setting's default |

## Transform tab

| Gesture | Action |
| --- | --- |
| Drag across a dial | Scrub the selected joint's Bend, Side-Side or Twist |
| Click a dial | Type a value; `Enter` confirms, `Esc` cancels |
| The small button beside a dial | Return that dial to 0, the rest pose |

## Materials tab

| Gesture | Action |
| --- | --- |
| Click a surface in the viewport | Select the object and the material zone under the cursor |
| Drag across a value | Scrub it (every matching zone with it, while **Edit matching zones together** is on) |
| Click a value | Type it; `Enter` confirms, `Esc` cancels |
| Click the Base Color swatch | Open the colour picker; `Cancel` puts the colour back |
| The small button beside a row | Return that value to what the file imported with |

## This manual

| Key | Action |
| --- | --- |
| `Ctrl+F` | Search |
| `Enter` / `F3` | Next match; `Shift+F3` previous |
| `Alt+Left` / `Alt+Right` | Back / Forward |
| `Esc` | Close |
