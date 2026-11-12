# The Interface

PoseStudio has one main window: a menu bar, the 3D viewport with its floating control strip, and a side panel with six tabs. Everything else — Preferences, the manual, the import progress dialogs — opens on top of it.

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
| Delete Selected Object | Removes the outlined model from the scene — or the selected light. The `Delete` key does the same while the viewport has focus. Deleting a model also clears the undo history; deleting a light is an undo step. |
| Add Light → Point Light / Spot Light / Area Light / Sun | Adds a light to the scene, two metres from the figure and aimed at it. See [Lights](lights.md). |
| Turn Joint with Mouse → Bend (`B`) / Side-Side (`S`) / Twist (`T`) | Turns the selected joint by moving the mouse; see [Posing](posing.md#turning-a-joint-with-the-mouse-b-s-t). |
| Reset Selected Joint / Reset Limb / Reset Pose | Return the selected joint, the joint and everything below it, or the whole figure to the rest pose. |
| Mirror Pose / Mirror Limb to Other Side | Swap the pose's sides, or copy the selected limb's pose to the other side. |
| Preferences | Opens the Preferences dialog. |

**View** — the skeleton overlay and the camera views (all with keyboard shortcuts; see [Navigating the Viewport](navigation.md#the-camera-views)).

**Render** — *Render Image* (`F12`) opens the Render window and renders the scene as the viewport shows it; *Show Render Window* brings the window and its last picture back. See [Rendering](rendering.md).

**Help** — this manual (`F1`), *What's New…* (this version's release notes in their own window — the one that opens by itself the first time a new version starts, see [Privacy and Updates](privacy-updates.md#whats-new-after-an-update)), the release notes (the manual's *What's New* page, the full history), the website, the update check, and the About screens. *Tutorials* and *Support* are placeholders.

## The viewport strip

A row of controls floats in the viewport's top-right corner:

| Control | What it does |
| --- | --- |
| **Render** (left of the shading picker) | Renders the scene: opens the Render window and starts the render, as **Render → Render Image** and `F12` do. See [Rendering](rendering.md). |
| **Shading picker** (reads *PBR Shaded* by default) | Chooses how the scene is drawn — its second entry, **Rendered**, path traces the scene in the viewport. See [Shading and Display](shading.md). |
| **View picker** (reads *Home View* by default) | Snaps the camera to a named view; reads *Perspective View* once you orbit away from one. |
| **Home** | Returns the camera to the default framing (also `5`). |
| **Ground** | Drops the active figure so its lowest point rests on the floor; landing on her feet, she absorbs the drop with her knees. See [Posing](posing.md#the-ground-button). |
| **Skeleton** | Toggles the bone overlay. Joints can be grabbed with it on or off. |
| **Selection** | Opens [Preferences → Selection](preferences.md#selection): the selection outline and the joint highlight, each switched on or off and given its color and opacity. |
| **VR** (right of the view picker) | Enter or leave VR Mode — see [VR Mode](vr.md); lit in blue while the headset has the scene. The view picker's last entry, VR Mode, does the same, and `Esc` leaves. |

While the mouse is turning a joint (`B`, `S` or `T` with a joint selected: see [Posing](posing.md#turning-a-joint-with-the-mouse-b-s-t)), a small badge under the strip names the motion: Bend, Side-Side or Twist. In the **Rendered** shading mode a line there says how far the traced picture is: *Rendered: 37 / 128 samples*, then *Rendered*.

## The side panel

The six tabs sit on the panel's left edge.

- **Asset Manager** — a tree of your asset libraries, Favorites and Collections above a thumbnail grid, with a search field at the top. Double-clicking an asset imports it. See [The Asset Manager](asset-manager.md).
- **Transform** — the selected joint's rotation as three dials: Bend, Side-Side and Twist. See [The Transform tab](posing.md#the-transform-tab).
- **Materials** — the selected object's material zones as sliders: color, roughness, metallic, specular, opacity, the skin layers — and the overlays layered over the skin: tattoos, marks, makeup. See [Materials](materials.md).
- **Soft Body Physics** — the switches and dials for the breasts' and the buttocks' motion: on and off, their softness, the breasts' bounce. See [The Soft Body Physics tab](posing.md#the-soft-body-physics-tab).
- **Environment** — the lighting controls: the HDRI picker, the backdrop, exposure and tone, image-based lighting, the key light, shadows, and skin and rim accents. See [Lighting and Environment](environment.md).
- **Lights** — the scene's light objects: point, spot, area and sun lights, added, placed and set. See [Lights](lights.md).

Drag the divider between the tree and the grid inside the Asset Manager to give either more room.

## Value fields and fine control

Every number in the tabs and in Preferences is the same kind of field: a bar that fills with the value, and the value written on it.

- **Drag** left or right across the field to change the value. A plain drag moves in the field's own steps — whole numbers for most: a joint's dial, a percentage, an opacity.
- **Shift + drag** is fine control: steps a tenth the size, at a tenth of the speed, so a dial that moves in whole numbers is set to 17.1, 17.2, 17.3. Press or let go of Shift at any point of a drag; the drag carries on from where it is at the other pace.
- **Click** the field once to type a value — any value in its range, to whatever precision you type. `Enter` confirms, `Esc` cancels.

A field shows the extra digit only when the value has one: 17 stays 17, and a value set finely, typed, or simply what the scene holds — a joint posed by dragging the body rarely lands on a whole number — reads 17.3. A plain drag takes such a value back onto whole steps.

A few fields differ. The Soft Body dials step by 5 and, with Shift, by 1; the VR scales step by 0.05 and, with Shift, by 0.01. Fields that are whole counts — how many times a detail map repeats, a subdivision level — have no fine steps.

## Dialogs and overlays

- **Preferences** (Edit → Preferences) is a dialog with pages down its left side. Settings apply as you change them; Close is the only button. See [Preferences](preferences.md).
- **The User Manual** (Help → User Manual, `F1`) is this window. It stays above the main window so you can read while you work.
- **Import progress** dialogs appear while a model or figure loads; they have no cancel button, and the application waits for the import to finish.
- **The About screen** (Help → About PoseStudio) is the splash artwork with the version number; click anywhere to dismiss it.
