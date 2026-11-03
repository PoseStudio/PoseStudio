# Pose Files

A pose is saved as a small plain-text `.pose` file that you can load back onto a figure, keep in your library, or hand to someone else.

## Saving and loading

- **File → Save Pose…** writes the active figure's current pose. The dialog opens in the folder you used last time; `.pose` is added to the name if you leave it off. Any drag still settling is landed first, so the file holds the pose you see.
- **File → Load Pose…** applies a `.pose` file to the active figure — every joint's rotation, the figure's position, and its pins — as one undoable step: `Ctrl+Z` brings the previous pose back.
- Both need a figure in the scene; with none, PoseStudio says so and does nothing.
- A `.pose` file given on the command line, or dropped onto `PoseStudio.exe` after a figure, is applied to that figure once it has loaded.

A pose file names the figure's bones, so it applies best to the figure it was saved from — or to another figure of the same generation, which names its bones the same way. Loaded onto a figure that names them differently, the joints it cannot find are skipped and stay at rest. Characters imported from FBX files all carry the rig standard's bone names (see [Importing Content](importing.md)) and the standard's rotation frames and ranges, so a pose saved on one of them loads on any other, whatever tool rigged them; a pose from a native figure does not cross to them, nor theirs to a native figure.

Poses saved by version 0.3.15 or earlier may load a little differently: those versions turned each joint about axes that were slightly off the figure's own, most visibly at the thumbs, the fingers, the forearms and the feet. The rotations in the file are applied about the correct axes now.

## Pins travel with the pose

The joints you pinned when you saved are pinned again when you load. Older files without pin rows load fine — they simply set no pins.

## The file format

The file is text, one row per line, and can be edited by hand:

```
hip 0 12.5 0
lShldrBend -40 0 3.2
rShldrBend -40 0 -3.2
@trans:hip 0 -0.31 0.12
@pin:lHand 0.21 0.95 0.14
```

Three kinds of row can appear:

| Row | Meaning |
| --- | --- |
| `bone rx ry rz` | The bone's rotation in degrees, in the figure's own rotation order for that joint. |
| `@trans:bone x y z` | A bone's position offset in metres (full-body IK writes one for the root: a crouch, a walk). |
| `@pin:bone x y z` | A pinned joint and where it is held. |

Loading starts from the rest pose and applies the rows: a joint the file does not name returns to rest, a row naming a bone the figure does not have is ignored, and the pins in the file replace the current ones. A file with a damaged row — a missing number, a non-numeric value — is rejected whole (*"Could not read the pose file."*) rather than half applied. Rotations are wrapped into ±360° on load and then clamped to each joint's limits.
