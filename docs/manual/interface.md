# The Interface

PoseStudio has one main window: a menu bar, the 3D viewport with its floating control strip, and a side panel with four tabs. Everything else — Preferences, the manual, the import progress dialogs — opens on top of it.

## The main window

The viewport sits on the left and the side panel on the right, separated by a draggable divider. The window title shows the version you are running and, once the scene is saved or opened as a project, the project's file name — with a `*` in front while there are unsaved changes. The window opens at a comfortable size for your screen; there is no saved layout yet, so it opens the same way every time.

## The menu bar

**File** — starting a new scene, opening and saving the scene as a `.pss` project, importing models and figures, saving and loading poses, and quitting. The greyed entries — Open Recent, Save Copy, Export and the other import formats — are placeholders for planned features.

| Entry | What it does |
| --- | --- |
| New (`Ctrl+N`) | Resets the scene to a fresh launch: removes every model, returns the camera to home, restores the startup lighting and HDRI, the default shade mode and the skeleton overlay off, and clears the undo history. With unsaved changes it asks first: Save, Discard Changes or Cancel. |
| Open… (`Ctrl+O`) | Opens a `.pss` project — the whole scene. See [Project Files](project-files.md). |
| Save (`Ctrl+S`) / Save As… (`Ctrl+Shift+S`) | Saves the whole scene as a `.pss` project. See [Project Files](project-files.md). |
| Import → .OBJ (Wavefront) | Imports a static mesh. See [Importing Content](importing.md#static-models-obj). |
| Import → .DUF (DUF File) | Imports a rigged character figure. See [Importing Content](importing.md#character-figures-duf-dsf). |
| Save Pose… | Writes the active figure's pose to a `.pose` file. |
| Load Pose… | Applies a `.pose` file to the active figure (undoable). |
| Quit (`Ctrl+Q`) | Closes PoseStudio. With unsaved changes it asks first: Save, Discard Changes or Cancel. |

**Edit** — the undo history, the pose utilities, and the keys that turn a joint with the mouse.

| Entry | What it does |
| --- | --- |
| Undo (`Ctrl+Z`) / Redo (`Ctrl+Y`) | One history for pose edits and lighting edits alike. See [Posing](posing.md#undo-and-redo). |
| Delete Selected Object | Removes the outlined model from the scene. The `Delete` key does the same while the viewport has focus. Deleting an object also clears the undo history. |
| Turn Joint with Mouse → Bend (`B`) / Side-Side (`S`) / Twist (`T`) | Turns the selected joint by moving the mouse; see [Posing](posing.md#turning-a-joint-with-the-mouse-b-s-t). |
| Reset Selected Joint / Reset Limb / Reset Pose | Return the selected joint, the joint and everything below it, or the whole figure to the rest pose. |
| Mirror Pose / Mirror Limb to Other Side | Swap the pose's sides, or copy the selected limb's pose to the other side. |
| Preferences | Opens the Preferences dialog. |

**View** — the skeleton overlay and the camera views (all with keyboard shortcuts; see [Navigating the Viewport](navigation.md#the-camera-views)).

**Help** — this manual (`F1`), *What's New…* (this version's release notes in their own window — the one that opens by itself the first time a new version starts, see [Privacy and Updates](privacy-updates.md#whats-new-after-an-update)), the release notes (the manual's *What's New* page, the full history), the website, the update check, and the About screens. *Tutorials* and *Support* are placeholders.

## The viewport strip

A row of controls floats in the viewport's top-right corner:

| Control | What it does |
| --- | --- |
| **Shading picker** (reads *PBR Shaded* by default) | Chooses how the scene is drawn. See [Shading and Display](shading.md). |
| **View picker** (reads *Home View* by default) | Snaps the camera to a named view; reads *Perspective View* once you orbit away from one. |
| **Home** | Returns the camera to the default framing (also `5`). |
| **Ground** | Drops the active figure so its lowest point rests on the floor; landing on her feet, she absorbs the drop with her knees. See [Posing](posing.md#the-ground-button). |
| **Skeleton** | Toggles the bone overlay. Joints can be grabbed with it on or off. |
| **VR** (right of the view picker) | Enter or leave VR Mode — see [VR Mode](vr.md); lit in blue while the headset has the scene. The view picker's last entry, VR Mode, does the same, and `Esc` leaves. |

While the mouse is turning a joint (`B`, `S` or `T` with a joint selected: see [Posing](posing.md#turning-a-joint-with-the-mouse-b-s-t)), a small badge under the strip names the motion: Bend, Side-Side or Twist.

## The side panel

The four tabs sit on the panel's left edge.

- **Asset Manager** — a tree of your asset libraries, Favorites and Collections above a thumbnail grid, with a search field at the top. Double-clicking an asset imports it. See [The Asset Manager](asset-manager.md).
- **Transform** — the selected joint's rotation as three dials: Bend, Side-Side and Twist. See [The Transform tab](posing.md#the-transform-tab).
- **Soft Body Physics** — the switches and dials for the breasts' and the buttocks' motion: on and off, their softness, the breasts' bounce. See [The Soft Body Physics tab](posing.md#the-soft-body-physics-tab).
- **Environment** — the lighting controls: the HDRI picker, the backdrop, exposure and tone, image-based lighting, the key light, shadows, and skin and rim accents. See [Lighting and Environment](environment.md).

Drag the divider between the tree and the grid inside the Asset Manager to give either more room.

## Dialogs and overlays

- **Preferences** (Edit → Preferences) is a dialog with pages down its left side. Settings apply as you change them; Close is the only button. See [Preferences](preferences.md).
- **The User Manual** (Help → User Manual, `F1`) is this window. It stays above the main window so you can read while you work.
- **Import progress** dialogs appear while a model or figure loads; they have no cancel button, and the application waits for the import to finish.
- **The About screen** (Help → About PoseStudio) is the splash artwork with the version number; click anywhere to dismiss it.
