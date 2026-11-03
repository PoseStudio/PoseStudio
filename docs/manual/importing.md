# Importing Content

PoseStudio reads three kinds of content today: static models in the Wavefront `.obj` format, rigged character figures in their native scene format (`.duf` presets backed by `.dsf` data files), and rigged characters in `.fbx` files. The other formats in the File → Import menu are placeholders for planned importers.

There are four ways to import, and they all do the same thing:

- **File → Import → .OBJ (Wavefront)**, **File → Import → .DUF (DUF File)** or **File → Import → .FBX (FBX File)** — a file dialog, opening in the folder you imported from last time.
- **Double-click** an `.obj`, `.duf`, `.dsf` or `.fbx` asset in the [Asset Manager](asset-manager.md) grid.
- **Open with** PoseStudio, or **drag files onto** `PoseStudio.exe`: every `.obj`, `.duf`, `.dsf` and `.fbx` on the command line is imported in order, and a `.pose` file after a figure is applied to it.

Every import lands a new object in the scene, selected (outlined in blue) and framed by the camera. Imports run one at a time on the main thread: the application is busy until the object appears.

## Static models (.obj)

An `.obj` file imports with its material groups, diffuse colours and diffuse textures (`map_Kd`) — the texture files are looked up relative to the `.obj`'s own folder through its `.mtl`. Missing normals are generated. Other texture maps (normal, roughness, metalness) are not read from `.obj` files, since the format has no standard way to carry them. The model is lit and shaded like everything else, but it has no skeleton: it can be selected, framed, deleted and looked at, not posed.

A model whose file uses an unsupported format is simply ignored — nothing appears and no message is shown.

## Character figures (.duf, .dsf)

A figure preset is a small file that *refers* to the figure's geometry, skeleton, skin weights, morphs, UV layout and materials, which live in a content folder alongside it, in a `data` subfolder. Importing one brings in the whole figure:

- **The shape** — the base figure with the character's own morphs baked in, then smoothed once (Catmull-Clark subdivision) so limbs and faces read round.
- **The materials** — per-zone physically based materials with diffuse, normal or bump, roughness, specular-mask and translucency maps, the eyes' clear shells rendered transparent, and eyelash and eyebrow cards cut out by their masks. Each zone's maps are read through the UV layout its material names, so a character that paints its skin on one generation's layout and its eyes on another's renders both correctly.
- **The rig** — every bone with its rest position, rotation order and the anatomical limits authored for it, plus the skin weights that let the mesh follow it.
- **Pose correctives** — the corrective shapes that fix the mesh at deep elbow, knee, shoulder and hip bends; they blend live as you pose.
- **Follower add-ons** the preset declares, such as eyelashes, eyebrows and replacement anatomy, merged into the figure.
- **The character's scale**, so a tall or a small character lands at its true height.

The figure is placed **standing on the floor** — its lowest skin point at height zero — facing the camera's default view. Several figures can be in the scene at once; see [Posing](posing.md#several-figures) for how posing chooses between them.

Import takes a few seconds for a fully textured character. The progress dialog names the stages: reading the figure, decoding textures, baking ambient occlusion, uploading to the graphics card. Textures larger than 4096 pixels on a side are downscaled while decoding, which saves memory and time at no visible cost in the viewport.

### Files that are not figures

Pose, material, light and other presets share the `.duf` extension. Importing one of those shows *"This file may be a pose, material, or other preset rather than a figure"* — pick the character's own preset instead. (Pose presets in this format are not readable yet; PoseStudio has its own [pose files](pose-files.md).)

## Characters from FBX files (.fbx)

An `.fbx` file carries everything in itself (or beside itself), so there is no content folder to locate. Importing one brings in:

- **The meshes** with their materials — the base colour, the normal or bump map, the roughness, metalness and opacity where the file links them — and their textures, embedded in the file or found by the path the file gives, relative to the `.fbx`, or by name anywhere under the file's folder or the folder above it (exports commonly keep their textures in a folder beside the file).
- **The skeleton** with its skin weights. Skinless end markers at the tips of the chains are left out of the skeleton (they still tell PoseStudio which way a fingertip or a single-bone toe points). The bones take the rig standard's names — `hips`, `spine1`, `neck1`, `head`, `collar_l`, `upper_arm_l`, `forearm_l`, `hand_l`, `thumb1_l`, `index2_r`, `thigh_l`, `shin_l`, `foot_l`, `ball_l` and so on, the side last — whatever the file called them; the face rig, the helper bones and any bone PoseStudio could not place keep their file names. That is what the Transform tab and the joint menu show, and what a pose file carries: because every imported character shares the standard's names, frames and ranges, a pose saved on one loads on any other (see [Pose Files](pose-files.md)).
- **The pose it was saved in** is discarded in favour of the bind pose, so a file saved mid-animation imports at rest.

The figure lands in metres, standing on the floor over the origin, turned to face the camera's default view with her left at the viewer's right — whatever axes and units the file was written in.

### Posing an FBX character

An `.fbx` file carries no anatomy: no joint limits, no notion of which bone is a knee. PoseStudio reads that off the skeleton's structure — the hips, the spine, the neck and head, each arm and leg with their twist helpers, the hands and fingers, the feet and toes — and gives every joint the standard rotation frame and the anatomical range the native figures author for that part of the body. From then on the character poses like any other: the full-body IK, the Transform tab's dials, `B` / `S` / `T`, pins, the Ground button and pose files all work, and a click on a helper bone (a corrective bone beside a knee or an elbow, an anchor) selects the joint it belongs to.

What to expect, compared with a native figure:

- **A T-pose character** imports with her arms out, and the posing engine reads arms held out as *posed* (see [Posing](posing.md)): lower them first, with the dials or the mouse mode, and the idle-arm behaviour follows. An A-pose character (arms down, a little out) needs nothing.
- **Skinning and helper bones.** A rig authored for linear blend skinning is skinned linearly, as in its authoring tool. Its helper bones — the twist helpers along a limb, the corrective bones at a knee or an elbow — are driven from the joints they serve (the file carries no driver expressions), so a twist spreads along the limb and a knee folds in two steps. There are no pose correctives, as a native figure has, so a very deep fold is the rig's own.
- **The face** is not posed (a face rig, where the file has one, lights with the head as the native figures' does).
- **A biped rig from an animation package** — the digits named only by number, the twist bones carrying end markers of their own, the chest one long bone on a two-link spine — reads like the others: the thumb is the digit whose base sits at the wrist, the twist helpers spread the limb's twist, the one toe bone curls as a toe, and she sits on her hips (such a rig has no pelvis bone). Take hold of the chest where the chest is, up by the breastbone, to bow her; the chest joint itself sits low on this kind of rig and is read as the waist.
- A rig whose structure PoseStudio cannot read completely — no arms found, say — still imports and poses joint by joint with the dials; the import log names the roles it could not place.

A static `.fbx` (no skeleton) imports as a model, like an `.obj`.

## When the figure's data cannot be found

A preset only works together with its content folder — the folder that directly contains a `data` folder. PoseStudio finds it by walking up from the file you imported. Content is often split, though: a folder of character presets may not contain the `data` folder they refer to. Then the import stops with the message *"PoseStudio couldn't find the geometry and morph data for…"* and the button **Locate Content Folder…**:

1. Click **Locate Content Folder…** and pick the folder that directly contains the `data` folder for this figure.
2. PoseStudio remembers that folder and retries the import at once.

Every folder you locate this way is remembered, so you only do this once per content folder. If you pick a folder without a `data` folder inside, PoseStudio warns you but still remembers it — pick again if the import still fails. The remembered folders are used for every later import, after the folder the file itself is in.

## Import problems

| Message | What it means |
| --- | --- |
| *This file may be a pose, material, or other preset rather than a figure.* | The `.duf` does not describe a figure. Import the character preset. |
| *The figure's geometry and morph data (its "data" folder) couldn't be found.* | Locate the content folder as described above. |
| *Could not import a character figure from… Details: …* | Something in the files could not be read; the details line names it. |
| *Could not import "….fbx"* | The file could not be read as FBX (a damaged file, or one older than the FBX 6 series), or it holds no mesh. The second line names the cause. |
| Nothing happens after choosing a file | The file's format is not supported, or (for `.obj`) it could not be parsed. |

A materials preset a figure lists for one of its followers is applied to every follower it addresses by name, so eyelashes and tears that share one preset both receive their maps.
