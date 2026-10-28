# Importing Content

PoseStudio reads two kinds of content today: static models in the Wavefront `.obj` format, and rigged character figures in their native scene format (`.duf` presets backed by `.dsf` data files). The other formats in the File → Import menu are placeholders for planned importers.

There are four ways to import, and they all do the same thing:

- **File → Import → .OBJ (Wavefront)** or **File → Import → .DUF (DUF File)** — a file dialog, opening in the folder you imported from last time.
- **Double-click** an `.obj`, `.duf` or `.dsf` asset in the [Asset Manager](asset-manager.md) grid.
- **Open with** PoseStudio, or **drag files onto** `PoseStudio.exe`: every `.obj`, `.duf` and `.dsf` on the command line is imported in order, and a `.pose` file after a figure is applied to it.

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
| Nothing happens after choosing a file | The file's format is not supported, or (for `.obj`) it could not be parsed. |

A materials preset a figure lists for one of its followers is applied to every follower it addresses by name, so eyelashes and tears that share one preset both receive their maps.
