# In-app posing scripts

Scripted posing tests that run inside the real application — the picker, the press path, the
camera-parallel drag plane, the 60 Hz solve loop, the release settle, the renderer — and save
what the viewport rendered as PNG files. They exist so that a posing change can be *looked at*,
not only measured: the IK harness (`tools/ikharness`) checks numbers on the armature alone,
and a pose can pass every gate and still look wrong.

Nothing is injected into the operating system. The window's own gesture functions are called
with a scripted cursor, so a run does not touch the mouse or keyboard and is safe on a desktop
that is in use; frames are read back from the GPU, so the window does not need to be visible.
And nothing comes in: while a script runs the viewport ignores real mouse, wheel and key events
(a wheel notch from your own scrolling, the cursor merely over the window, once dollied the camera
under a scripted drag and showed up as a pop that no rerun reproduced).

## Running

```
POSESTUDIO_IK_SCRIPT=<script.txt> POSESTUDIO_IK_SCRIPT_OUT=<folder> PoseStudio <figure preset>
```

The app imports the figure, runs the script, writes the shots into the output folder and exits
with the number of `expect` lines that failed as its exit code. `run.sh` wraps that and builds a
contact sheet; `run_all.sh` runs every script in this folder as a test suite:

```
POSESTUDIO_TEST_FIGURE=<figure preset> tools/ikscripts/run.sh tools/ikscripts/gallery_side.txt out/side
POSESTUDIO_TEST_FIGURE=<figure preset> POSESTUDIO_IKSHOTS_REF=<reference folder> tools/ikscripts/run_all.sh out
```

`run_all.sh` prints each script's failed expectations and a pass/fail summary (its exit code is
the number of scripts with a failure), and — with `POSESTUDIO_IKSHOTS_REF` — compares every shot
with its reference (`compare.py`): the shots that LOOK different are listed and laid out beside
their references in `<out>/<script>/changes.png`, so after a change there is one image per
script to look at, holding only the poses that moved. `run_all.sh <out> --accept` makes the run's
shots the new references — after you have looked. The references are renders of a figure preset:
keep them outside the repository.

The scripts:

| Script | What it covers |
| --- | --- |
| `gallery_side.txt` | Trunk gestures from the side: chest and head nudged back, bows, the chest-walk, the hip hinge, a crouch, a hand reach. |
| `gallery_mixed.txt` | Sideways chest pull and hip sway (front), kneel, sit, the pinned seat, a bow with a posed arm. |
| `gallery_limbs.txt` | Hand across the body, elbow, knee raise, foot slide, stride, lunge, foot lifted behind, lifted by the hand. |
| `gallery_perspective.txt` | The Home and three-quarter views a user poses in: hips, chest, head, hand and foot drags where "down" on screen is down and toward the camera. |
| `gallery_floor.txt` | Floor poses from several drags each: toward all fours, a kneel sat back on the heels, a sit reclined by the chest, a sit with a leg stretched out, a sit LAID DOWN on her back and sat up again — with the SKIN's height over the floor, the pelvis's pitch and the worst single-tick move asserted, and the sat-up pose shot from the FRONT as well: a drag in the sagittal plane must leave her symmetric (the pelvis bone's twist and side bend are asserted), and legs folded to one side do not show from the side. |
| `gallery_group.txt` | Several joints dragged together (THE GROUP): both hands raised, pushed forward and brought down, both knees pushed forward — through `sclick` / `spress` — with the selection's bookkeeping asserted (a Shift+click on a member removes it, a plain click selects one joint alone). |
| `session_poses.txt` | Whole poses built from several drags in a row (a walk, a sit with a hand to the knee, a high reach, picking something up), shot from a three-quarter view. |
| `check_returns.txt` | Numbers only: out-and-home in one drag, a bow undone in a second drag, a walking drag let go of mid-step, a posed head. |
| `gallery_hands.txt` | Hands laid on the body: a hand on the hip, both hands on the hips, on the heart, on top of the head, on the belly, behind the back, seated on the knee, and a hand laid and taken away — with `palm.<bone>` asserted. |
| `gallery_scoped.txt` | Ctrl + drag. On a limb or the head, the SCOPED drag: a hand, a knee and the head each moved with only their own chain, the rest of the body asserted still (`movedxz.<bone>`, `moved.<bone>`). On the body itself, the FIGURE MOVE: the chest lifted and the hips carried with the whole figure going along as posed (no channel turned, the feet travelling with the grab), the Ground button bringing her back down, the floor stopping a push down, and a pinned hand staying where it is pinned while the body moves. |
| `gallery_digits.txt` | Fingers, toes and the face, in close-up: a fingertip curled, a finger pulled far beyond its reach, the thumb, a Ctrl-drag of a finger, a toe raised — each with the hand, the foot and the body asserted still (`moved.<bone>` under 0.01mm) — the back of the hand still dragging the arm, and clicks on the eye, the jaw and an ear each selecting the head (`selected.head`), a drag from the face moving the head with the face's bones untouched. |
| `gallery_landing.txt` | The Ground button's landing bounce: a figure lifted and dropped 5cm, 50cm and 2m, shot falling, at the deepest of the dip and after — the knees' fold asserted deeper for the higher drop, the pose afterwards asserted exactly the pose she was dropped in, the skin on the floor — a posed figure keeping her pose through it, and a kneeling one landing with no bounce. |
| `check_project.txt` | Numbers only: a posed scene saved as a `.pss` project, emptied with New and opened again — the figure re-imported, its pose and pin restored, the document clean; a pose edit dirties it; a bad file is refused with the scene intact. |
| `materials/sg_cube.txt`, `materials/textured_cube.txt`, `materials/obj_cube.txt`, `materials/ext_cube.txt`, `materials/showcase.txt` | NOT run by run_all.sh (the subfolder): each runs on ONE synthetic asset from `tools/materials/make_test_assets.py` (`POSESTUDIO_TEST_FIGURE=<asset> run.sh <script> <out>`; `select 0` selects the static model) — the KHR material extensions read into the wider material (`ext_cube.gltf`: emission × strength, IOR, specular tint, clearcoat, sheen, iridescence, anisotropy, transmission + volume, a texture transform as the UV tiling), the specular / glossiness conversion (a spec/gloss cube's gold comes out a metal with the gold as its base color and its red plastic a dielectric; `sg_scalar.gltf`, and `mr_cube.gltf` as the metallic-roughness reference it must match), the per-texel bake (the textured cube ends with a base-color, a metalness and a roughness map, `matmap.<name>`, its scalars at 1; `sg_textured.gltf`), and the MTL PBR extension with the classic `Ks` / `Ns` (`pbr_cube.obj`); `showcase.txt` is THE VIEWPORT'S MATERIALS (2026-11-10) — six spheres (`showcase.gltf`: glass, velvet, brushed metal, an iridescent metal, car paint, chalk) shot from the home and front views, each close up, then with every layer switched off and with the variants (a colored coat, the grain turned, a thin-walled glass): the sheet is what to LOOK at after a shader change (`SHEET_COLS=3`). |
| `check_materials.txt` | The Materials tab: a click on the chest selects the Torso zone (`zonename.<zone>`), a roughness edit reaches every skin zone and not the eyes (`material <field> <value>`, `mat.<field>`, `matimported.<field>`, `matlink on/off`; `material <color> r g b` and `mat.<color>.<r|g|b>` for any of the material's colors by the tab's names — `color`, `specularcolor`, `coatcolor`, `emissioncolor` …), undo takes it back, Reset Zone and Reset All Zones return the import, a tint and an opacity are shot, the edits survive a project save / New / open, and clicks on the eye and the thigh select their zones. `uishot` pictures the tab (run with POSESTUDIO_TAB=Materials). |
| `materials/textures.txt` | THE TEXTURE SLOTS, on the figure with the loose PNGs from `tools/materials/make_test_assets.py` (`POSESTUDIO_TEST_FIGURE=<figure> run.sh materials/textures.txt <out> POSESTUDIO_IK_SCRIPT_ASSETS=<assets>`): `texture <slot> <path|none|imported>` is the tab's name click / clear / arrow on the selected zone (the slot names are `basecolor`, `normal`, `bump`, `roughness`, `metalness`, `specmask`, `translucency`, `emission`, `pore`, `cutout`, `diffuseroughness`, `coatweight`, `coatcolor`, `coatroughness`, `coatreflectivity`, `coatbump`, `translucencyweight`, `specularcolor`, `anisotropy`, `anisotropyrotation`, `lobe2`, `loberatio`, `detailweight`, `detailroughness` (`pore` is the detail normal map), `displacement`, `ao`, `transmissioncolor`, `transmissionweight`, `coatanisotropy`, `coatanisotropyrotation`, `sheen`, `sheencolor`, `sheenroughness`, `thinfilm`, `thinfilmthickness`; the layer switches read and set as 0 / 1 fields: `topcoatenabled`, `sheenenabled`, `thinfilmenabled`, `transmissionenabled`, `subsurfaceenabled`, `emissionenabled`, `displacementenabled`, `detailenabled`, `baseenabled`, `specularenabled`, `geometryenabled`, `tilingenabled`, and `thinwalled` (the Transmission section's box); a relative path resolves against POSESTUDIO_IK_SCRIPT_ASSETS, else the output folder), `matmap.<slot>` reads 1 when the zone carries that map and `mattex.<slot>` where it comes from (0 none, 1 the imported map, 2 a chosen file) — a red base color chosen, undone, cleared and restored, a checker cutout on a zone without one and away again, a roughness ramp, an emission map, and a chosen map plus a cleared slot through a project save / New / open. |
| `materials/overlays.txt` | THE OVERLAYS (materialoverlay.h): images layered over a zone's maps — tattoos, bruises, makeup — on the figure with the `overlay_*.png` patterns from `tools/materials/make_test_assets.py` (`POSESTUDIO_TEST_FIGURE=<figure> run.sh materials/overlays.txt <out> POSESTUDIO_IK_SCRIPT_ASSETS=<assets>`; each pattern covers the whole UV square, so it shows on any zone of any figure). The commands are the Materials tab's Overlays section, each one undoable edit on the selected zone's stack — and on every zone painted on the same color map: `overlay add [image]` (a new overlay on top: EMPTY, as the tab's + Add Overlay makes it, or with its picture), `overlay remove <n>`, `overlay move <n> <to>`, `overlay <n> <field> <value>` (`enabled`, `opacity`, `brightness`, `contrast`, `gamma`, `hue`, `saturation`, `gloss`, `metallic`, `bumpstrength`, `normalstrength`, `displacementstrength` in the tab's units, and the overlay's place: `offsetx` / `offsety` in percent of the map, + to the right and + up, `rotation` in degrees, + clockwise on the map, and `scale` in percent of the painted size, both about the overlay's own centre; `blend <multiply\|normal\|softlight\|overlay\|screen\|darken\|lighten>`), `overlay <n> tint <r> <g> <b>` (the card's Base Color, display values 0..1), `overlay <n> name <text>` (a new one is "Overlay N") and `overlay <n> map <color\|opacity\|metallic\|bump\|normal\|displacement> <image\|none>` (`map opacity` is the Opacity row's map: the overlay's mask); `<n>` counts from 0, the lowest layer, and a relative image path resolves as `texture`'s does. The metrics: `overlays` (the zone's count), `overlayzones` (the zones the stack is painted on), `overlay.<n>.<field>`, `overlay.<n>.tint.<r\|g\|b>`, `overlay.<n>.has.<element>`, `overlay.<n>.named.<name>` (1 if that is its name; a space written `_`) and `metalbaked` (the Metallic value, in percent, the selected zone's bound metalness was baked for under a metallic overlay; -1 with none). The script: an overlay as it arrives (Multiply, full opacity), its controls set and undone one by one, the overlay moved half a grid cell and back, turned 45 degrees and enlarged and back, made metallic (whole, then through a metallic map, the zone's own Metallic value moved under it), a second overlay added empty and made up from a Base Color and an opacity map (glossy), over it and then under it and switched off, a relief-only overlay (a bump image raised and inverted, unchanged by an opacity map at opacity 0, a normal image), a remove undone, the stack through a project save / New / open, and an overlay added on the face showing on the lips' zone — the shots are what to LOOK at. `POSESTUDIO_OVERLAY_TRACE=1` prints what each edit cost (the images derived, the milliseconds). The section's own WIDGETS are driven in-process by `ui <verb> <id> [n] […]` — the n-th widget carrying the dynamic property `testId` == `<id>`: `click` (a button), `set <value>` (a scrub field), `check <0\|1>`, `pick <k>` (a picker's entry), `call <slot> [int]` (one of the widget's own slots: what a context-menu entry runs), `type <text>` (a line edit) and `key <enter\|esc>` — and read by `uicount.<id>`, `uivalue.<id>.<n>` (a label reads as its number), `uienabled.<id>.<n>` and `uivisible.<id>.<n>`; no desktop input. The folded Overlays header carries `overlay.section` (the heading: `ui click` folds it), `overlay.header.add` (its **+**, a mark and not a button) and `overlay.header.count`. |
| `check_selection_style.txt` | PREFERENCES → SELECTION: the selection outline's and the joint highlight's color and opacity — set apart, through the viewport (`outlinestyle` / `jointstyle`, read by `selstyle.*`; orange, 30 %, off; green, red, yellow, white, black) and through the Preferences page itself (`prefs Selection`, its fields and restore buttons driven with `ui`, pictured with `uishot` as `ui/<name>_dialog.png`, closed by `ui click preferences.close`), each mark's switch (`ui check selection.outline.enabled 0 0`), the strip's Selection button (`ui click viewport.selection 0`), and kept by File → New. |
| `check_picking.txt` | Numbers only: what a click selects (`selected.<bone>`) — hidden parts are never selected through the part in front, a click on a joint's own pixel takes that joint (the hip's, though its skin is the pelvis bone's). |
| `check_liftoff.txt` | Numbers only: lifting her off the floor by the HIPS and the TRUNK (2026-11-07) — the hips pulled up past the legs' stretch lift her off after a quarter second and she floats with the cursor (`lowest` well up, no step); a floating figure's next drag, a hand's, leaves her floating; lowered by the hips she lands; the chest lifts her off too; and a hip pulled only back up to standing does not lift off. |
| `gallery_softbody.txt` | THE SOFT BREASTS (armaturebreasts.cpp), with the Soft Body Physics tab's switches on as the app ships: at rest standing (`breastl` / `breastr` 0, `breasthangl` / `breasthangr` 0 mm), hung in a level-chested bow (the user's pose: the teardrop, the nipple at the floor, in close-up from the side, three-quarters and below) and in a deep bow during the drag and at rest after it, lifted toward the chin reclining, flattened and spread on her back and fallen toward the lower side on her side (`breastsidel` / `breastsider`), jolted by a quick chest drag up, down and sideways (`breastupl` / `breastupr`, mm along the chest, + toward the head: the bounce goes up and sideways, and below the sculpt only by the floor's give), and the Ground button's landing — shot through the bounce (wall-clock timed) and at rest after it; and THE SOFT BUTTOCKS (armaturebuttocks.cpp): 0 at rest after a Reset (`buttl` / `buttr`, mm — they never hang), swaying ten ticks into a held sideways drag of the hips, shot from behind, and at rest again after the settle. |
| `softbody/boneless.txt` | NOT run by run_all.sh (the subfolder). THE BONELESS BREAST (armaturebreastfield.cpp): a rig with no breast bone carries the soft breasts' mass on the chest's own skin, as a delta field. Run it on the base figure with every breast forced to a field (`POSESTUDIO_TEST_FIGURE=<base figure> run.sh softbody/boneless.txt <out> POSESTUDIO_BREAST_AS_FIELD=1`: the bone path's mass, the field's skin, on the figure the amounts are calibrated on) or, with no probe, on a rigged character whose rig has no breast bone: `breastfields` 2 (0 on a figure whose pectoral bones carry her breasts — `gallery_softbody.txt` asserts that on the base figure), the field at rest standing (`breastfieldl` / `breastfieldr`, mm: how far the field moves the skin at the apex), hung in a level bow, pressed and spread on her back, on her side with the upper breast held within the field's travel across the chest, jolted by a Ctrl lift of the hips, and through a landing — shot in close-up for looking. |
| `check_rigged.txt` | A rigged character from an interchange file (FBX, glTF / GLB, USD) — or any figure: shot from the front and the side, then posed by the hand and by the hips, with the grab on the cursor, the feet planted and nothing through the floor asserted; Reset Pose brings her back. Run it on one file: `POSESTUDIO_TEST_FIGURE=<file> tools/ikscripts/run.sh tools/ikscripts/check_rigged.txt <out>` — the import check for a new format or a new character. |

Useful beside it: `QT_LOGGING_TO_CONSOLE=1` (the `[ikscript]` lines reach a redirected stderr),
`POSESTUDIO_NO_PING=1`, `POSESTUDIO_NO_UPDATE_CHECK=1`, and any `IK_…` probe for an A/B
(`run.sh` passes extra `NAME=value` arguments through as environment). For a pop only a scripted
click makes: `POSESTUDIO_IK_SCRIPT_TICKS=1` prints the worst joint's move every tick with the grab
and the cursor, `POSESTUDIO_IK_SCRIPT_EULERS="lThigh,lShin"` those bones' channels every tick,
`POSESTUDIO_PICK_TRACE=1` what the click's surface cast counted and hit, and `IK_DRAG_STATE_TRACE=1`
the pins planted at each press and, per tick, the cursor, the grab offset and the target the rig
was handed (`[drag-map]`).
`montage.py` needs Python with Pillow.

## Commands

One command per line; `#` starts a comment. Bones are named as the reference figure names them
(`hip`, `chest`, `head`, `lHand` …); the runner maps them onto the other generations' names, so the
same script runs on any figure — set `POSESTUDIO_TEST_FIGURE` to another preset and look. `<bone>@<share>` means the point that share of the way
along the bone's limb segment — the flesh, where a user clicks — instead of the joint.

| Command | What it does |
| --- | --- |
| `view front\|back\|left\|right\|top\|bottom\|home` | The view keys. In `right` the camera looks from +X: screen-right is the figure's back. |
| `orbit <yaw°> <pitch°>` | An orbit drag, in degrees (back to perspective, like one). |
| `frame` | Frame the selection. |
| `closeup <bone> <radius m>` | Frame the camera on that joint, the given radius filling the view — a hand, a foot, the face, before posing a finger. |
| `lookat <x> <y> <z> <radius m>` | Frame the camera on a world point — a splat scene's face or hands, which no bone names. A script with no figure (a splat scene alone) starts once any model is in. |
| `shade <row>` | Shade mode, a row of the picker. |
| `pose <file>` / `reset` / `undo` / `redo` / `ground` | Load a pose file, Reset Pose, Undo, Redo, the Ground button. |
| `saveproject <name.pss>` / `openproject <name.pss>` / `new` | File → Save As into the run's folder / File → Open from it (the scene reset, every figure re-imported and restored; the missing-file dialog is not driven) / File → New's scene reset (no prompt, no panel reset). |
| `lift <metres>` | Raise the active figure that far off the floor, as posed; `ground` then drops her. The fall and the landing bounce run on the app's own timer, and under a script on SCRIPT time — a sixtieth a tick, so a `shot` mid-fall (a GPU read-back that stalls a frame) moves nothing (2026-11-07): `wait` through them (half a metre falls in 19 ticks, the bounce lasts 21-30). |
| `fk <bone> <dx°> <dy°> <dz°>` | Turn a joint about its own channels, as a Transform dial turns one (through the limits and the FK collision stop). |
| `modal <bend\|side\|twist>` | The `B` / `S` / `T` key, as a key event handed to the viewport's own handler: the joint mouse mode on that dial of the selected joint (the same key again drops it, another switches). |
| `app <b\|s\|t\|esc\|enter\|click\|rclick>`, `app move <dxPx> <dyPx> [steps]` | The same input arriving at the APPLICATION with the viewport not focused: the Edit menu's action for the key (only if it is enabled, as the shortcut would be), and key and mouse events delivered to the main window — where the mode's application-wide input filter must pick them up. |
| `modalmove <dxPx> <dyPx> [steps]` | Move the mouse that far with no button held (+y down), in that many move events (20 by default) through the viewport's own handler. |
| `modalkeep [enter]` / `modaldrop [esc]` | The left click (or `Enter`): keep, one undo entry / the right click (or `Esc`): the joint back as it was. Press and release go through the viewport's own handlers. |
| `dial <forward\|sideways\|twist> <value>` | Set the selected joint's Transform-tab dial to that value (0 = rest, 100 = the joint's limit), the way the tab does: one bracketed, undoable edit. Select the joint first (`click`). |
| `pin <bone>[@share]` / `unpinall` | Click there and press P; drop every pin. Like `press`, when the pick lands on another part of the body (the hanging hand covers the hip from the side) the named bone is pinned instead, and the log says `pinned by name instead`. |
| `click <bone>[@share] [dxPx dyPx]` | The real pick at that pixel (moved by the offset where given, +y down: a toe's tip lies past its last joint's pixel); prints what it found. The pixel is the NAMED bone's joint's, whatever a selection of it selects: `click lEye` clicks at the eye, and finds the head. |
| `pick2d on\|off` | The A/B: the 2D pick alone, as before 2026-09-26 (every joint and bone body projected to the screen, the nearest within 32px taken, no occlusion check) — a hidden part CAN be selected through another. `POSESTUDIO_PICK_2D=1` does the same for a whole run. |
| `press <bone>[@share]` | The real press: the pick at the bone's pixel, then the press path (an IK drag begins). The pick is honest — what it finds is what is under that pixel — and on another body that can be another limb (a broad character's hanging arm covers her chest from the side, one hand covers the other). When the rig would then drag a different joint than the one named, the bone is grabbed by name at the same point instead, and the log says `the pick found <other> there`. A small bone OF the named one (a pectoral for the chest) is what a user's click finds too, and stands. |
| `sclick <bone>` | The Shift+CLICK at the bone's pixel: adds the joint to THE GROUP (as its primary, the previous selection joining the group) or takes a member out; prints the group. |
| `spress <bone>[@share]` | The Shift press: the pressed joint joins the selection as its primary and a whole-body drag of the whole group begins through it — every selected joint moves by the same displacement. |
| `cpress <bone>[@share]` | The Ctrl press: the same pick and press path, beginning the Ctrl + drag — a SCOPED drag for a limb or the head (the grabbed chain alone moves), the FIGURE MOVE for the body itself (the hips, the abdomen, the chest: the whole figure as posed, nothing planted, the user's pins holding), the digit's own drag for a finger or a toe. |
| `drag <dxPx> <dyPx> <moveTicks> <holdTicks>` | Move the cursor in screen pixels (+y down), then hold. |
| `dragv <right m> <up m> <moveTicks> <holdTicks>` | The same, given in metres along the view's right/up axes at the grab's depth. |
| `dragw <dx> <dy> <dz> <moveTicks> <holdTicks>` | Move the cursor's *world* target (the harness's gestures, for parity). |
| `wheel <notches>` | Depth notches during a drag. |
| `release` | Mouse-up; waits for the settle. Prints a report first. |
| `wait <ticks>` | 60 Hz ticks. |
| `report [label]` | Gaze, lowest joint, grab-to-cursor, hips/head travel since the press, pelvis angles, steps, worst single-tick joint move, and the posed channels. |
| `expect <metric> <op> <value>` | A test assertion (`<`, `<=`, `>`, `>=`, `==`). A failure is printed and counted; the app's exit code is the count. |
| `shot <name>` | Save the next rendered frame as `<name>.png`. |
| `outlinestyle <rrggbb> <opacity%>` / `jointstyle <rrggbb> <opacity%>` | The selection outline's / the joint highlight's color (hex, no `#`) and opacity, as Preferences → Selection hands them to the viewport; nothing is stored. |
| `prefs <tab>` | Open the Preferences dialog on that tab, as Edit → Preferences does (from the event loop: `wait` a few ticks before driving it). Its widgets carry `testId`s for `ui` (`selection.outline.opacity`, `selection.joint.restore.color`, `selection.panel` …), `uishot` saves it, `ui click preferences.close` closes it. |
| `ui scrub <id> <n> <% of its width> [shift]` | A press, a drag and a release on a value field, through its own mouse handlers — the step it snaps to, and with `shift` the fine scrub (a tenth of the step and of the travel). The drag is a share of the field's width, so the value it lands on does not depend on how wide the panel is. (`transform.dial` 0 / 1 / 2 = Bend / Side-Side / Twist, `transform.figure` 0–6 the Figure rows.) |
| `uishot <name>` | Save a picture of the application's WIDGETS as `ui/<name>.png` (and the floating viewport strip, with its mode badge, as `ui/<name>_strip.png`, and an open dialog — the Preferences dialog — as `ui/<name>_dialog.png`) — the side tabs; the native viewport is not in it (`shot` reads that off the GPU). Run with `POSESTUDIO_TAB=Transform` (any tab's title) to start on that tab. Kept in a folder of its own, out of the contact sheet and the reference comparison. |
| `light <dial> <value>` | One of the Environment tab's dials, as the tab sets it (no undo entry, the tab's own widgets are not told): `exposure`, `diffuse`, `specular`, `ambient`, `key`, `azimuth`, `elevation`, `rotation`, `subsurface`, `rim`, `shadows` (0 / 1), `shadowintensity`, `shadowsoftness`, `backdrop` (0 off, 1 environment, 2 dome), `backdropblur`, `backdropbrightness`, `tonemap` (0 / 1). |
| `environment <file.hdr\|.exr>` | The Environment tab's panorama pick (a relative path resolves like `texture`'s). The bake runs off the GUI thread: `wait` for it. |
| `render <name> [<width> <height>] [<samples>] [<device text>\|cpu] [nodenoise] [transparent]` | A PRODUCTION RENDER of the scene as it stands, through the render engine (src/render/, the Cycles plugin), run to its end and saved as `render/<name>.png` — the picture Render → Render Image makes, without its window. The size defaults to the viewport's, the samples to 64, the device to the fastest found (`cpu`, or a piece of a device's name such as `cuda`, picks another). Kept in a folder of its own, out of the contact sheet. Needs an engine: `POSESTUDIO_CYCLES_DIR=<folder>` points at one that is not next to the executable. `POSESTUDIO_RENDER_TRACE=1` prints what the engine is handed — every zone's material, the lights, the environment. |
| `renderwindow [start]` | The Render window, as Render → Show Render Window opens it (`start`: as Render → Render Image does). From the event loop: `wait` before driving it. Its controls carry `testId`s for `ui` — `render.start`, `render.stop`, `render.save`, `render.size`, `render.samples`, `render.device`, `render.denoise`, `render.transparent` — and `uishot` saves the window as `ui/<name>_render.png`; `uienabled.render.stop.0` reads 1 while a render runs, `uienabled.render.save.0` once there is a picture. |
| `quit` | Exit (also at the end of the script). |

## Metrics (`expect`)

| Metric | Meaning |
| --- | --- |
| `grab` / `grabpx` | The grabbed point's distance from the cursor, in mm / on screen in pixels (during a drag). |
| `hips`, `head` | Moved since the last press, mm. |
| `steps` | Steps taken in the drag. |
| `jump` | Worst single-tick move of any joint, mm. |
| `lowest` | The lowest body joint's height over the floor, mm (negative = through it). |
| `meshlow` | The lowest point of the posed SKIN over the floor, mm (negative = flesh through the floor — invisible to every joint reading). |
| `gaze`, `headroll` | The head's look direction over the horizon and its sideways tilt, degrees. |
| `tilt.<bone>` | A bone's tilt from upright, degrees. |
| `swing.<bone>` | A bone's fore-aft angle from straight down, degrees, + forward of the figure: an upper arm swung forward by a stride reads about +20, one swung back -20. |
| `y.<bone>`, `moved.<bone>`, `movedxz.<bone>` | A joint's height, its travel since the last press, and that travel along the floor alone (a planted foot's place, a heel lift aside), mm. |
| `group` / `ingroup.<bone>` | The number of selected joints (the primary and THE GROUP's secondaries) / 1 if that joint is one of them, else 0. |
| `selected.<bone>` | 1 if the selected joint is that bone or a bone of its rigid segment (what a `click` picked), else 0. |
| `lit.<bone>` | 1 if the selection highlight tints that bone's flesh (the selected joint, or the twist bone of its limb segment), else 0. |
| `euler.<bone>.<x\|y\|z>` | A posed channel, degrees. |
| `dial.<forward\|sideways\|twist>` | The selected joint's Transform-tab dial as it reads now; `dialmin.<kind>` / `dialmax.<kind>` its range. Unknown (a failed `expect`) with no joint selected or for a dial the joint does not have. |
| `dialjoint.<bone>` | 1 if the Transform tab shows that joint (a selected twist bone shows its bend joint), else 0. |
| `modal` | The dial the joint mouse mode is turning (0 / 1 / 2 = bend / side / twist), -1 with the mode off. |
| `figures` / `dirty` | The models in the scene / 1 while the `.pss` document has unsaved changes. Neither needs a figure. |
| `selection` | The selected model's index, −1 with nothing selected (`app esc` clears it). |
| `selstyle.outline.<r\|g\|b\|opacity>`, `selstyle.joint.<r\|g\|b\|opacity>` | Preferences → Selection as the viewport holds it: the outline's / the joint highlight's color as display bytes (0–255) and its opacity in percent. |
| `render.ok` / `render.seconds` / `render.mean` / `render.covered` | Of the last `render`: 1 if it finished and was saved, its time in seconds, the picture's mean display value (0–255) and the share of it, in percent, that is not the backdrop (the top-left pixel's color). |
| `figure.scale` / `figure.static` | The Figure group's Scale row (percent) / 1 while the group reads a selected STATIC model (a Gaussian splat scene, an OBJ) instead of a figure. The `figure.*` metrics need no figure. |
| `palm.<bone>.<x\|y\|z>` | A hand's palm normal, that world component (+1/-1): where a hand laid on the body faces. The figures face +z, their right side is -x. |

Expectations are regression guards set from what the app does today, with margin — not
specifications. A gesture given in screen metres is only as exact as its guess at the figure's
proportions: where a gate reads looser than the harness's, the gesture is off the joint's arc.

## A scripted run and your preferences

A scripted run is **isolated from the stored preferences**: whatever a script sets — a Preferences page it drives, a tab's switches — serves that run and is never written, and the selection outline and joint highlight start at their default colors whatever you have chosen (the shots are compared with references). `POSESTUDIO_IK_SCRIPT_PREFS=1` makes a scripted run read and write the preferences as any other run does; that is how the storing itself is checked. A scripted run also shows no splash screen (its first mouse press would go to the splash).

## Do not minimize the app while a script runs

`shot` waits for the next rendered frame, and a minimized window renders none: the script then waits forever at that line (the app keeps ticking and responding). A window behind others is fine — the frames are read back from the GPU — but one minimized to the taskbar is not. If a run stalls, check the window's state before anything else.

## What to look for

The runner prints `POP at tick N: <joint> moved X mm in one tick` for every single-tick move over
10cm — never a drag's pace — and `report` names the joint that moved most and when.

A script that does not reach its end (the app went away: a crash, a lost device, a window someone
closed) is reported as DID NOT FINISH, and the app's whole output is kept beside the shots as
`unfinished.txt` with its exit code.


Numbers first (`report`): the grabbed point on the cursor, no unexpected steps, no large
single-tick moves. Then the sheet: balance (is the weight over the feet?), arms and head
(do idle limbs look held or natural?), feet flat, nothing through the floor or the body.
A change that improves a number and makes a sheet look worse is a regression.
