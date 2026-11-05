# Importing Content

PoseStudio reads three kinds of content today: static models in the Wavefront `.obj` format, rigged character figures in their native scene format (`.duf` presets backed by `.dsf` data files), and rigged characters — or plain meshes — in the interchange formats other tools write: `.fbx`, glTF (`.glb` and `.gltf`), VRM avatars (`.vrm`) and USD (`.usd`, `.usda`, `.usdc` and `.usdz` packages).

There are four ways to import, and they all do the same thing:

- **File → Import → .OBJ (Wavefront)**, **File → Import → .DUF (DUF File)**, **File → Import → .FBX (FBX File)**, **File → Import → .GLB (GL Transmission Format .glTF)**, **File → Import → .USD (Universal Scene Description)** or **File → Import → .VRM (VRM Avatar)** — a file dialog, opening in the folder you imported from last time.
- **Double-click** an `.obj`, `.duf`, `.dsf`, `.fbx`, `.glb`, `.gltf`, `.vrm`, `.usd`, `.usda`, `.usdc` or `.usdz` asset in the [Asset Manager](asset-manager.md) grid.
- **Open with** PoseStudio, or **drag files onto** `PoseStudio.exe`: every model or figure file on the command line is imported in order, and a `.pose` file after a figure is applied to it.

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

## Characters from FBX, glTF and USD files

An `.fbx`, `.glb` / `.gltf` or `.usd` / `.usda` / `.usdc` / `.usdz` file carries everything in itself (or beside itself), so there is no content folder to locate. The three formats take the same route and end in the same place; what differs is listed under each format below. Importing one brings in:

- **The meshes** with their materials — the base colour, the normal or bump map, the roughness, metalness and opacity where the file links them — and their textures, embedded in the file or found by the path the file gives, relative to the file, or by name anywhere under the file's folder or the folder above it (exports commonly keep their textures in a folder beside the file).
- **The skeleton** with its skin weights. Skinless end markers at the tips of the chains are left out of the skeleton (they still tell PoseStudio which way a fingertip or a single-bone toe points). The bones take the rig standard's names — `hips`, `spine1`, `neck1`, `head`, `collar_l`, `upper_arm_l`, `forearm_l`, `hand_l`, `thumb1_l`, `index2_r`, `thigh_l`, `shin_l`, `foot_l`, `ball_l` and so on, the side last — whatever the file called them; the face rig, the helper bones and any bone PoseStudio could not place keep their file names. That is what the Transform tab and the joint menu show, and what a pose file carries: because every imported character shares the standard's names, frames and ranges, a pose saved on one loads on any other (see [Pose Files](pose-files.md)).
- **The bind pose**: an `.fbx` or a USD file saved mid-animation imports at rest, from the skin's bind; a glTF imports with its joints where the file's nodes stand (see the format notes below).

The figure lands in metres, standing on the floor over the origin, turned to face the camera's default view with her left at the viewer's right — whatever axes and units the file was written in.

### FBX files (.fbx)

Binary and ASCII FBX from version 6 on. The file's own axes and units are converted, its skinning method (linear or dual quaternion) is kept, and a file saved mid-animation imports at rest, from the skin's bind pose.

### glTF files (.glb, .gltf)

A `.glb` holds everything in one file; a `.gltf` keeps its buffers (`.bin`) and images beside it, or embedded as data URIs — both import. glTF is metres and Y-up by definition, so nothing is converted; a file written in centimetres (a character 20 m tall) is read as such and scaled down. The joints stand where the file's nodes put them — glTF stores no bind pose apart from the skins' matrices, and this is what every viewer shows at the default pose — so a file whose nodes are saved posed imports in that pose (its animations are not read). The metallic-roughness material model is read (the base colour and map, the metallic-roughness map's green channel as the roughness map, the normal map with its scale, the alpha mode as the opacity, with the base map's alpha as the cutout for masked and blended materials), the older specular-glossiness extension as a fallback. Not read: morph targets, vertex colours, texture transforms, a second UV set. A file that requires Draco or meshopt mesh compression, or Basis Universal textures, is refused with a message naming the extension: export it without compression.

### VRM avatars (.vrm)

A VRM avatar is a `.glb` with a humanoid bone map (VRM 0.x and VRM 1.0 alike), so everything said of glTF applies. The map names the bones outright — the hips, the spine links, each arm and leg, the hands, every finger joint, the eyes and the jaw — and PoseStudio takes the file's word over its own structural reading of the skeleton: those bones get the rig standard's names (`hips`, `spine1`, `upper_arm_l`, `thumb1_r` …) and roles as the map says, which matters on an avatar whose hair or skirt chains would otherwise be mistaken for limbs; the import log names every bone where the two readings differed. The avatar's title is logged. Hair, skirt and other spring-bone chains import as ordinary bones that ride their parents; the toon material (MToon) renders as its base colour and texture under the viewport's lighting; expressions (blend shapes) are not read.

### USD files (.usd, .usda, .usdc, .usdz)

ASCII (`.usda`), binary crate (`.usdc`, or `.usd` in either encoding) and USDZ packages, whose textures are read from inside the package. The stage's `metersPerUnit` and `upAxis` are honoured (a Z-up stage is turned Y-up; a stage that authors no unit is centimetres, as the format's own fallback says), and a file whose skeleton would stand 20 m tall is read as centimetres regardless. The skeleton is the UsdSkel one — the joints with their bind transforms, the meshes' joint weights — with the geometry bind transform and the skeleton's own transform applied; a file with several skeletons imports the largest as the figure and the others' meshes rigid. Materials are UsdPreviewSurface: diffuse colour and map, roughness (a value or a map, whichever channel the map's output names), metalness, the normal map, opacity (the base map's alpha when that is what the opacity reads, else its own map). Not read: blend shapes, animation, point instancers, MaterialX networks (such a mesh renders with its colour alone).

### Posing an imported character

An `.fbx`, glTF or USD file carries no anatomy: no joint limits, no notion of which bone is a knee. PoseStudio reads that off the skeleton's structure — the hips, the spine, the neck and head, each arm and leg with their twist helpers, the hands and fingers, the feet and toes — and gives every joint the standard rotation frame and the anatomical range the native figures author for that part of the body. From then on the character poses like any other: the full-body IK, the Transform tab's dials, `B` / `S` / `T`, pins, the Ground button and pose files all work, and a click on a helper bone (a corrective bone beside a knee or an elbow, an anchor) selects the joint it belongs to.

What to expect, compared with a native figure:

- **A T-pose character** imports with her arms out, and the posing engine reads arms held out as *posed* (see [Posing](posing.md)): lower them first, with the dials or the mouse mode, and the idle-arm behaviour follows. An A-pose character (arms down, a little out) needs nothing.
- **Skinning and helper bones.** A rig authored for linear blend skinning is skinned linearly, as in its authoring tool. Its helper bones — the twist helpers along a limb, the corrective bones at a knee or an elbow — are driven from the joints they serve (the file carries no driver expressions), so a twist spreads along the limb and a knee folds in two steps. There are no pose correctives, as a native figure has, so a very deep fold is the rig's own.
- **The face** is not posed (a face rig, where the file has one, lights with the head as the native figures' does).
- **A biped rig from an animation package** — the digits named only by number, the twist bones carrying end markers of their own, the chest one long bone on a two-link spine — reads like the others: the thumb is the digit whose base sits at the wrist, the twist helpers spread the limb's twist, the one toe bone curls as a toe, and she sits on her hips (such a rig has no pelvis bone). Take hold of the chest where the chest is, up by the breastbone, to bow her; the chest joint itself sits low on this kind of rig and is read as the waist.
- A rig whose structure PoseStudio cannot read completely — no arms found, say — still imports and poses joint by joint with the dials; the import log names the roles it could not place.

A static file (no skeleton) in any of the three formats imports as a model, like an `.obj` — a prop, a helmet, a building — with its textures and materials; it can be selected, framed and deleted, not posed.

## Gaussian splat scenes (.ply, .splat)

A Gaussian splat scene — a photographed place or person reconstructed as hundreds of thousands of soft, coloured, translucent points — imports as a static object: **File → Import → .PLY / .SPLAT (Gaussian Splat)**, a double-click on the file in the Asset Manager, or the file on the command line. PoseStudio reads the `.ply` the reference 3D Gaussian Splatting trainers write (positions, scales, rotations, opacity and spherical-harmonic colour up to degree 3) and the compact `.splat`. A compressed splat file (a chunked `.ply`, `.spz`, `.ksplat`) is refused with a message naming it; export an uncompressed `.ply` from your splat tool instead.

The scene is drawn as the trainers meant it: every splat sorted far to near for the current view and blended over what is behind it, its colour evaluated for the direction you look from, so highlights shift as you orbit. It keeps its photographed colours whatever the lighting dials say — a splat scene carries its own light, so the Environment tab's exposure, tonemap and lights leave it alone, and it casts and receives no shadow. Figures stand in it and in front of it as solid objects do: a figure between you and the scene hides that part of it, and a splat scene between you and the figure covers her. (In the PBR mode a very bright sky may glow a little; the Environment tab's Bloom dial turns that down.)

A trained scene almost always comes out of its camera frame upside down, so PoseStudio turns it over on import; most captures then stand the way they were photographed, at their trained scale (a scene trained from phone video is usually roughly life-size). If yours stands wrong, select it and use the Transform tab's **Figure** group: Turn, Tip and Tilt set its orientation, Left-Right / Up-Down / Back-Forward place it, and **Scale** sizes it in percent of the size it was imported at (see [Posing](posing.md#placing-the-figure)). A splat scene you are standing inside cannot be clicked — a click there goes to the figure in it — so select it from outside its extent or right after import, and remove it with Edit → Delete Selected Object.

Half a million splats sort in a few milliseconds when the camera moves; a scene of several million takes a moment per view change and a few hundred megabytes of graphics memory. A project (`.pss`) carries the scene by its file path and its placement.

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
| *Could not import "….glb"* / *"….gltf"* / *"….vrm"* | The file could not be read as glTF 2.0 (a glTF 1.0 file, malformed JSON, a `.bin` or image it refers to that is missing beside it), it holds no triangle mesh, or it requires Draco / meshopt compression or Basis Universal textures. The second line names the cause. |
| *Could not import "….usd"* (or `.usda`, `.usdc`, `.usdz`) | The file could not be read as USD (a damaged file, a layer it references that is missing), or it holds no mesh. The second line names the cause. |
| Nothing happens after choosing a file | The file's format is not supported, or (for `.obj`) it could not be parsed. |

A materials preset a figure lists for one of its followers is applied to every follower it addresses by name, so eyelashes and tears that share one preset both receive their maps.
